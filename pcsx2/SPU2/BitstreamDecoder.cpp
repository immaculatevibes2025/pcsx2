// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SPU2/BitstreamDecoder.h"

#include "common/Console.h"
#include "common/DynamicLibrary.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "Config.h"
#include "VMManager.h"

#include "fmt/format.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavcodec/version.h"
#include "libavutil/channel_layout.h"
#include "libavutil/frame.h"
#include "libavutil/version.h"
}

// FFmpeg is loaded on demand, like GSCapture does, so PCSX2 still runs without it.
#define VISIT_BS_AVCODEC_IMPORTS(X) \
	X(avcodec_find_decoder) \
	X(avcodec_alloc_context3) \
	X(avcodec_open2) \
	X(avcodec_free_context) \
	X(avcodec_send_packet) \
	X(avcodec_receive_frame) \
	X(av_packet_alloc) \
	X(av_packet_free) \
	X(av_new_packet) \
	X(av_packet_unref) \
	X(av_parser_init) \
	X(av_parser_parse2) \
	X(av_parser_close)

#define VISIT_BS_AVUTIL_IMPORTS(X) \
	X(av_frame_alloc) \
	X(av_frame_free) \
	X(av_frame_unref)

#define DECLARE_IMPORT(X) static decltype(X)* bs_##X;
VISIT_BS_AVCODEC_IMPORTS(DECLARE_IMPORT);
VISIT_BS_AVUTIL_IMPORTS(DECLARE_IMPORT);
#undef DECLARE_IMPORT

namespace
{
	// IEC 61937 burst preamble and data types
	static constexpr u16 IEC_PA = 0xF872;
	static constexpr u16 IEC_PB = 0x4E1F;
	enum : u8
	{
		IEC_AC3 = 1,
		IEC_DTS1 = 11,
		IEC_DTS2 = 12,
		IEC_DTS3 = 13,
	};

	enum class Codec
	{
		None,
		AC3,
		DTS,
	};

	struct State
	{
		bool load_attempted = false;
		bool loaded = false;
		DynamicLibrary avcodec, avutil;

		Codec codec = Codec::None;
		AVCodecContext* ctx = nullptr;
		AVCodecParserContext* parser = nullptr; // raw (not IEC 61937 wrapped) DTS
		AVPacket* pkt = nullptr;
		AVFrame* frame = nullptr;

		// IEC 61937 burst assembly, in 16-bit words
		u16 prev_word = 0;
		int header_pos = 0; // 0 = searching for Pa/Pb, 1 = next word is Pc, 2 = next word is Pd
		u16 pc = 0;
		bool in_burst = false;
		u8 burst_type = 0;
		u32 burst_words_left = 0;
		std::vector<u8> payload;

		// Raw DTS fallback: big-endian byte stream
		std::vector<u8> raw;
		u32 raw_dts_syncs = 0;

		// Decoded output FIFO, NUM_CHANNELS interleaved
		std::vector<float> fifo;
		u32 fifo_read = 0, fifo_frames = 0;

		// Diagnostics
		int last_play_mode = -1;
		u64 words_seen = 0, bursts = 0, decoded_frames = 0, decode_errors = 0;
		bool announced = false;
		std::FILE* dump = nullptr;
		u64 dump_words = 0;
	};

	static State s;

	static constexpr u32 FIFO_CAPACITY = 48000; // one second
	static constexpr u64 DUMP_LIMIT_WORDS = 48000ull * 2 * 30; // first 30 seconds of bitstream

	static bool LoadFFmpeg()
	{
		if (s.load_attempted)
			return s.loaded;
		s.load_attempted = true;

		bool ok = s.avutil.Open(DynamicLibrary::GetVersionedFilename("avutil", LIBAVUTIL_VERSION_MAJOR).c_str(), nullptr) &&
				  s.avcodec.Open(DynamicLibrary::GetVersionedFilename("avcodec", LIBAVCODEC_VERSION_MAJOR).c_str(), nullptr);
#define RESOLVE(X) ok = ok && s.avcodec.GetSymbol(#X, &bs_##X);
		VISIT_BS_AVCODEC_IMPORTS(RESOLVE);
#undef RESOLVE
#define RESOLVE(X) ok = ok && s.avutil.GetSymbol(#X, &bs_##X);
		VISIT_BS_AVUTIL_IMPORTS(RESOLVE);
#undef RESOLVE
		s.loaded = ok;
		if (!ok)
			Console.Error("SPU2 bitstream: could not load FFmpeg (avcodec/avutil), Dolby Digital/DTS will not be decoded.");
		return ok;
	}

	static void CloseCodec()
	{
		if (s.parser)
			bs_av_parser_close(s.parser);
		if (s.ctx)
			bs_avcodec_free_context(&s.ctx);
		if (s.pkt)
			bs_av_packet_free(&s.pkt);
		if (s.frame)
			bs_av_frame_free(&s.frame);
		s.parser = nullptr;
		s.codec = Codec::None;
	}

	static bool OpenCodec(Codec codec)
	{
		if (s.codec == codec && s.ctx)
			return true;
		if (!LoadFFmpeg())
			return false;
		CloseCodec();

		const AVCodec* dec = bs_avcodec_find_decoder(codec == Codec::AC3 ? AV_CODEC_ID_AC3 : AV_CODEC_ID_DTS);
		if (!dec)
		{
			Console.Error("SPU2 bitstream: this FFmpeg build has no %s decoder.", codec == Codec::AC3 ? "AC-3" : "DTS");
			return false;
		}
		s.ctx = bs_avcodec_alloc_context3(dec);
		s.pkt = bs_av_packet_alloc();
		s.frame = bs_av_frame_alloc();
		if (!s.ctx || !s.pkt || !s.frame || bs_avcodec_open2(s.ctx, dec, nullptr) < 0)
		{
			Console.Error("SPU2 bitstream: failed to open the %s decoder.", codec == Codec::AC3 ? "AC-3" : "DTS");
			CloseCodec();
			return false;
		}
		s.codec = codec;
		return true;
	}

	static float Sample(const AVFrame* f, int ch, int i)
	{
		switch (f->format)
		{
			case AV_SAMPLE_FMT_FLTP: return reinterpret_cast<const float*>(f->extended_data[ch])[i];
			case AV_SAMPLE_FMT_FLT: return reinterpret_cast<const float*>(f->extended_data[0])[i * f->ch_layout.nb_channels + ch];
			case AV_SAMPLE_FMT_S16P: return reinterpret_cast<const s16*>(f->extended_data[ch])[i] * (1.0f / 32768.0f);
			case AV_SAMPLE_FMT_S16: return reinterpret_cast<const s16*>(f->extended_data[0])[i * f->ch_layout.nb_channels + ch] * (1.0f / 32768.0f);
			case AV_SAMPLE_FMT_S32P: return reinterpret_cast<const s32*>(f->extended_data[ch])[i] * (1.0f / 2147483648.0f);
			case AV_SAMPLE_FMT_S32: return reinterpret_cast<const s32*>(f->extended_data[0])[i * f->ch_layout.nb_channels + ch] * (1.0f / 2147483648.0f);
			default: return 0.0f;
		}
	}

	// Maps one decoded channel onto the 6-channel output (FL, FC, FR, RL, RR, LFE). Returns a destination index
	// and gain; a mono surround goes to both rear channels at -3 dB.
	static void AddChannel(u64 id, float v, float* out)
	{
		switch (id)
		{
			case AV_CH_FRONT_LEFT: out[0] += v; break;
			case AV_CH_FRONT_CENTER: out[1] += v; break;
			case AV_CH_FRONT_RIGHT: out[2] += v; break;
			case AV_CH_BACK_LEFT:
			case AV_CH_SIDE_LEFT: out[3] += v; break;
			case AV_CH_BACK_RIGHT:
			case AV_CH_SIDE_RIGHT: out[4] += v; break;
			case AV_CH_BACK_CENTER:
				out[3] += v * 0.70710678f;
				out[4] += v * 0.70710678f;
				break;
			case AV_CH_LOW_FREQUENCY: out[5] += v; break;
			default: break; // wides, heights etc. do not occur in PS2 streams
		}
	}

	static void PushFrame(const AVFrame* f)
	{
		if (f->sample_rate != 48000)
		{
			s.decode_errors++;
			return; // PS2 bitstreams are 48 kHz; anything else is not something we can play in sync
		}
		const int nch = f->ch_layout.nb_channels;
		u64 ids[16] = {};
		if (f->ch_layout.order == AV_CHANNEL_ORDER_NATIVE)
		{
			u64 mask = f->ch_layout.u.mask;
			for (int c = 0; c < nch && c < 16 && mask; c++)
			{
				const u64 bit = mask & (~mask + 1);
				ids[c] = bit;
				mask &= ~bit;
			}
		}
		else
		{
			// Unknown order: treat as stereo
			ids[0] = AV_CH_FRONT_LEFT;
			ids[1] = AV_CH_FRONT_RIGHT;
		}

		for (int i = 0; i < f->nb_samples; i++)
		{
			if (s.fifo_frames == FIFO_CAPACITY)
			{
				// Overflow: drop the oldest frame rather than stall the emulator
				s.fifo_read = (s.fifo_read + 1) % FIFO_CAPACITY;
				s.fifo_frames--;
			}
			const u32 w = (s.fifo_read + s.fifo_frames) % FIFO_CAPACITY;
			float* out = &s.fifo[w * BitstreamDecoder::NUM_CHANNELS];
			std::fill(out, out + BitstreamDecoder::NUM_CHANNELS, 0.0f);
			for (int c = 0; c < nch && c < 16; c++)
				AddChannel(ids[c], Sample(f, c, i), out);
			s.fifo_frames++;
		}
		s.decoded_frames += f->nb_samples;
	}

	static void DecodePacket(const u8* data, int size)
	{
		if (bs_av_new_packet(s.pkt, size) < 0)
			return;
		std::memcpy(s.pkt->data, data, size);
		if (bs_avcodec_send_packet(s.ctx, s.pkt) < 0)
			s.decode_errors++;
		bs_av_packet_unref(s.pkt);
		while (bs_avcodec_receive_frame(s.ctx, s.frame) == 0)
		{
			PushFrame(s.frame);
			bs_av_frame_unref(s.frame);
		}
	}

	static void Announce(const char* what)
	{
		if (s.announced)
			return;
		s.announced = true;
		Console.WriteLn(Color_StrongGreen, "SPU2 bitstream: %s detected (S/PDIF play mode %d), decoding to discrete surround.", what,
			s.last_play_mode);
	}

	static void OnBurstComplete()
	{
		s.bursts++;
		const Codec codec = (s.burst_type == IEC_AC3) ? Codec::AC3 :
							(s.burst_type >= IEC_DTS1 && s.burst_type <= IEC_DTS3) ? Codec::DTS :
																						Codec::None;
		if (codec == Codec::None || s.payload.empty())
			return;
		Announce(codec == Codec::AC3 ? "Dolby Digital (AC-3)" : "DTS");
		if (OpenCodec(codec))
			DecodePacket(s.payload.data(), static_cast<int>(s.payload.size()));
	}

	// Raw DTS (no IEC 61937 wrapper): let FFmpeg's parser find the frames in the byte stream
	static void FeedRawDTS()
	{
		if (!OpenCodec(Codec::DTS))
			return;
		if (!s.parser)
			s.parser = bs_av_parser_init(AV_CODEC_ID_DTS);
		if (!s.parser)
			return;
		const u8* data = s.raw.data();
		int left = static_cast<int>(s.raw.size());
		while (left > 0)
		{
			u8* out = nullptr;
			int out_size = 0;
			const int used = bs_av_parser_parse2(s.parser, s.ctx, &out, &out_size, data, left, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
			if (used < 0)
				break;
			data += used;
			left -= used;
			if (out_size > 0)
			{
				Announce("DTS (raw)");
				DecodePacket(out, out_size);
			}
		}
		s.raw.clear();
	}

	static void FeedWord(u16 w)
	{
		s.words_seen++;

		if (s.in_burst)
		{
			if (s.burst_words_left > 0)
			{
				// Payload words are big-endian byte pairs
				s.payload.push_back(static_cast<u8>(w >> 8));
				s.payload.push_back(static_cast<u8>(w));
				if (--s.burst_words_left == 0)
				{
					s.in_burst = false;
					OnBurstComplete();
				}
			}
			s.prev_word = w;
			return;
		}

		// Looking for Pa Pb, then Pc (data type) and Pd (length in bits)
		if (s.header_pos == 0 && s.prev_word == IEC_PA && w == IEC_PB)
		{
			s.header_pos = 1;
		}
		else if (s.header_pos == 1)
		{
			s.pc = w;
			s.header_pos = 2;
		}
		else if (s.header_pos == 2)
		{
			const u32 bits = w;
			s.header_pos = 0;
			s.burst_type = static_cast<u8>(s.pc & 0x1F);
			const u32 bytes = (bits + 7) / 8;
			if (bytes > 0 && bytes <= 65536)
			{
				s.payload.clear();
				s.burst_words_left = (bytes + 1) / 2;
				s.in_burst = true;
			}
		}
		else
		{
			// Not in an IEC 61937 burst: watch for a raw (unwrapped) DTS stream, 16-bit big-endian or 14-bit
			if ((s.prev_word == 0x7FFE && w == 0x8001) || (s.prev_word == 0x1FFF && (w & 0xFFF0) == 0xE800))
				s.raw_dts_syncs++;
			if (s.raw_dts_syncs >= 2)
			{
				s.raw.push_back(static_cast<u8>(w >> 8));
				s.raw.push_back(static_cast<u8>(w));
				if (s.raw.size() >= 8192)
					FeedRawDTS();
			}
		}
		s.prev_word = w;
	}

	static void OpenDump()
	{
		if (s.dump || s.dump_words >= DUMP_LIMIT_WORDS)
			return;
		const std::string serial = VMManager::GetDiscSerial();
		const std::string path = Path::Combine(EmuFolders::Logs, fmt::format("{}_spu2_bitstream.bin", serial.empty() ? "unknown" : serial));
		s.dump = FileSystem::OpenCFile(path.c_str(), "wb");
		if (s.dump)
			Console.WriteLn("SPU2 bitstream: saving the first 30 seconds of S/PDIF data to %s", path.c_str());
	}
} // namespace

void BitstreamDecoder::Reset()
{
	if (s.dump)
	{
		std::fclose(s.dump);
		s.dump = nullptr;
	}
	if (s.loaded)
		CloseCodec();
	const bool attempted = s.load_attempted, loaded = s.loaded;
	s.in_burst = false;
	s.burst_words_left = 0;
	s.payload.clear();
	s.raw.clear();
	s.raw_dts_syncs = 0;
	s.prev_word = 0;
	s.header_pos = 0;
	s.fifo.assign(FIFO_CAPACITY * NUM_CHANNELS, 0.0f);
	s.fifo_read = s.fifo_frames = 0;
	s.last_play_mode = -1;
	s.words_seen = s.bursts = s.decoded_frames = s.decode_errors = 0;
	s.announced = false;
	s.dump_words = 0;
	s.load_attempted = attempted;
	s.loaded = loaded;
}

void BitstreamDecoder::Shutdown()
{
	Reset();
	if (s.loaded)
	{
		s.avcodec.Close();
		s.avutil.Close();
	}
	s.loaded = s.load_attempted = false;
}

void BitstreamDecoder::Feed(s16 left, s16 right, int play_mode)
{
	if (s.fifo.empty())
		s.fifo.assign(FIFO_CAPACITY * NUM_CHANNELS, 0.0f);

	if (play_mode != s.last_play_mode)
	{
		Console.WriteLn("SPU2 bitstream: S/PDIF play mode %d (%s).", play_mode,
			(play_mode == 4) ? "bitstream bypass" : (play_mode == 2) ? "PCM bypass" : "other");
		s.last_play_mode = play_mode;
	}

	if (left != 0 || right != 0)
		OpenDump();
	if (s.dump && s.dump_words < DUMP_LIMIT_WORDS)
	{
		const s16 w[2] = {left, right};
		std::fwrite(w, sizeof(w), 1, s.dump);
		s.dump_words += 2;
		if (s.dump_words >= DUMP_LIMIT_WORDS)
		{
			std::fclose(s.dump);
			s.dump = nullptr;
			Console.WriteLn("SPU2 bitstream: capture finished (%llu bursts seen, %llu frames decoded, %llu errors).",
				static_cast<unsigned long long>(s.bursts), static_cast<unsigned long long>(s.decoded_frames),
				static_cast<unsigned long long>(s.decode_errors));
		}
	}

	// S/PDIF carries the left subframe first, then the right
	FeedWord(static_cast<u16>(left));
	FeedWord(static_cast<u16>(right));
}

u32 BitstreamDecoder::GetAvailableFrames()
{
	return s.fifo_frames;
}

bool BitstreamDecoder::PopFrames(float* out, u32 frames)
{
	if (s.fifo_frames < frames)
		return false;
	for (u32 i = 0; i < frames; i++)
	{
		std::memcpy(out + i * NUM_CHANNELS, &s.fifo[s.fifo_read * NUM_CHANNELS], NUM_CHANNELS * sizeof(float));
		s.fifo_read = (s.fifo_read + 1) % FIFO_CAPACITY;
	}
	s.fifo_frames -= frames;
	return true;
}
