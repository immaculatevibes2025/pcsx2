// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// Decodes the compressed surround that PS2 games send out of the optical (S/PDIF) port.
//
// Games with in-game DTS (e.g. SSX 3, SSX Tricky) and games with Dolby Digital FMV soundtracks (e.g. Metal Gear
// Solid 2, Onimusha 3) put the SPU2 into bitstream mode and stream IEC 61937 bursts through core 0's input. On a
// real console a receiver decodes them; PCSX2 used to read that data and mute it. Here the bursts are unpacked
// and decoded with FFmpeg (AC-3 or DTS) into discrete channels for the audio stream.
namespace BitstreamDecoder
{
	static constexpr u32 NUM_CHANNELS = 6; // FL, FC, FR, RL, RR, LFE (the same order as the PLII decoder output)

	void Reset();
	void Shutdown();

	// Called for every output sample while the SPU2 is in a bypass (S/PDIF) mode, with core 0's raw input words.
	void Feed(s16 left, s16 right, int play_mode);

	// What is being decoded, e.g. "Dolby Digital 5.1" or "DTS 4.0". Empty until the first frame is decoded.
	const char* GetFormatName();

	// Frames of decoded audio waiting to be played.
	u32 GetAvailableFrames();

	// Pops `frames` frames (NUM_CHANNELS floats each). Returns false if not enough are buffered.
	bool PopFrames(float* out, u32 frames);
} // namespace BitstreamDecoder
