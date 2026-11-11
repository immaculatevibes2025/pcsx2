// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Time-domain active matrix decoder modelled on Dolby Pro Logic II "Game" mode.
//
// This is an independent implementation of the publicly described behaviour of a Pro Logic II style decoder,
// not Dolby's code. It differs from FreeSurround in the ways that matter for PS2 games:
//
//  * Steering is broadband and time-domain. Two control signals are derived from signal envelopes:
//    left/right dominance from |Lt| vs |Rt|, and centre/surround dominance from |Lt+Rt| vs |Lt-Rt|.
//    The dominant source is cancelled from the channels it does not belong to and all channels move
//    together, so sounds keep their full timbre. FreeSurround steers every FFT bin independently,
//    which pulls individual frequencies of one sound into different speakers (thin fronts, smeared rears).
//  * Steering speed varies continuously and independently on each axis (left/right and front/back): slow and
//    stable while the soundfield is steady, fast as soon as a source moves or a transient appears.
//  * Autobalance (Game/Movie): slowly corrects a constant left/right level offset in the source so it cannot
//    bias steering.
//  * Full-bandwidth stereo surrounds with an adjustable delay (precedence effect keeps front sounds in front).
//  * Modes, following Dolby's description of PLII:
//      Game  - Movie behaviour plus bass redirection to the LFE (Linkwitz-Riley 4th order crossover; the mains
//              get the complementary high-pass, so nothing is doubled).
//      Movie - fixed presets, autobalance on, full-range mains (leave bass management to the receiver).
//      Music - autobalance off, mild high-frequency shelf on the surrounds, and the Centre Width, Dimension and
//              Panorama controls.
//
// Output channel order matches FreeSurround's 5.1 layout: FL, FC, FR, RL, RR, LFE.

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// Ambience (unsteered) path: share of L+R sent to the centre. 0.707 is the passive Pro Logic matrix, which
// puts unsteered in-phase sound (dialogue under an effect that has the steering) loudest in the centre.
#ifndef DPL2_AMB_C
#define DPL2_AMB_C 0.70710678f
#endif

class DolbyProLogicIIDecoder
{
public:
	static constexpr unsigned NUM_OUTPUT_CHANNELS = 6;

	enum class Mode : unsigned char
	{
		Game,
		Movie,
		Music,
	};

	struct Settings
	{
		Mode mode = Mode::Game;
		float lfe_crossover_hz = 120.0f; // Game mode only; 0 disables bass redirection
		// Receivers play the LFE channel of multichannel PCM 10 dB hotter, so -10 dB keeps bass level-matched.
		float lfe_level = 0.31623f;
		float surround_delay_ms = 10.0f;
		int center_width = 0; // Music: 0 = all in centre speaker .. 7 = all in left/right (phantom centre)
		int dimension = 0; // Music: -3 = towards rear .. +3 = towards front
		bool panorama = false; // Music: extend front left/right image into the surrounds
		// Steer four frequency bands independently instead of the whole signal at once. Not something a real
		// Pro Logic II decoder does: it keeps dialogue in the centre while a broadband effect plays in a
		// surround, where single-band steering has to choose one of them.
		bool multiband = false;
		// Stereo game on a 5.1 stream: front left/right only, no matrix decoding. Used so that stereo games which
		// have Dolby Digital / DTS FMVs can still switch to discrete 5.1 for those without being upmixed otherwise.
		bool stereo_passthrough = false;
	};

	DolbyProLogicIIDecoder(unsigned sample_rate, const Settings& settings)
		: m_fs(static_cast<float>(sample_rate))
		, m_settings(settings)
	{
		m_use_lfe = (settings.mode == Mode::Game && settings.lfe_crossover_hz > 0.0f);
		if (m_use_lfe)
		{
			for (int i = 0; i < 2; i++)
			{
				m_lp[0][i].SetButterworth(settings.lfe_crossover_hz, m_fs, false);
				m_lp[1][i].SetButterworth(settings.lfe_crossover_hz, m_fs, false);
				m_hp[0][i].SetButterworth(settings.lfe_crossover_hz, m_fs, true);
				m_hp[1][i].SetButterworth(settings.lfe_crossover_hz, m_fs, true);
			}
		}

		// The control path ignores deep bass (which is mostly mono and would drag steering to the centre)
		m_ctl_hp[0].SetButterworth(100.0f, m_fs, true);
		m_ctl_hp[1].SetButterworth(100.0f, m_fs, true);

		m_slow_coef = Coef(150.0f);
		m_fast_coef = Coef(8.0f);
		m_gain_coef = Coef(GAIN_MS);
		m_balance_coef = Coef(4000.0f);
		m_release_coef = Coef(150.0f);
		m_autobalance = (settings.mode != Mode::Music);

		const bool music = (settings.mode == Mode::Music);
		// Music mode controls
		const float cw = music ? std::clamp(settings.center_width, 0, 7) / 7.0f : 0.0f;
		m_center_keep = std::cos(cw * 1.57079633f);
		m_center_spread = std::sin(cw * 1.57079633f) * 0.70710678f;
		const float dim = music ? std::clamp(settings.dimension, -3, 3) / 3.0f * 0.5f : 0.0f;
		m_dim_sum = std::sqrt(1.0f + dim);
		m_dim_diff = std::sqrt(1.0f - dim);
		m_panorama = music && settings.panorama;
		m_rear_shelf = music;
		m_shelf_hp[0].SetFirstOrderHighpass(4000.0f, m_fs);
		m_shelf_hp[1].SetFirstOrderHighpass(4000.0f, m_fs);

		m_delay_len = std::max(1u,
			static_cast<unsigned>(std::clamp(settings.surround_delay_ms, 0.0f, 50.0f) * 0.001f * m_fs + 0.5f));
		m_delay[0].assign(m_delay_len, 0.0f);
		m_la_len = std::max(1u, static_cast<unsigned>(LOOKAHEAD_MS * 0.001f * m_fs + 0.5f));
		m_la_lfe.assign(m_la_len, 0.0f);
		for (Steerer& st : m_steer)
			st.Init(m_la_len);
		m_split[0].Init(m_fs);
		m_split[1].Init(m_fs);
		m_band_ctl_hp[0].SetButterworth(100.0f, m_fs, true);
		m_band_ctl_hp[1].SetButterworth(100.0f, m_fs, true);
		m_delay[1].assign(m_delay_len, 0.0f);
	}

	void Flush()
	{
		for (auto& b : m_lp)
			for (auto& f : b)
				f.Reset();
		for (auto& b : m_hp)
			for (auto& f : b)
				f.Reset();
		m_ctl_hp[0].Reset();
		m_ctl_hp[1].Reset();
		m_shelf_hp[0].Reset();
		m_shelf_hp[1].Reset();
		m_power = {};
		m_balance_conf = 0.0f;
		m_ax = m_ay = 0.0f;
		m_audible = false;
		std::fill(m_delay[0].begin(), m_delay[0].end(), 0.0f);
		std::fill(m_la_lfe.begin(), m_la_lfe.end(), 0.0f);
		m_la_pos = 0;
		for (Steerer& st : m_steer)
			st.Reset();
		m_split[0].Reset();
		m_split[1].Reset();
		m_band_ctl_hp[0].Reset();
		m_band_ctl_hp[1].Reset();
		std::fill(m_delay[1].begin(), m_delay[1].end(), 0.0f);
		m_delay_pos = 0;
		m_limit_gain = 1.0f;
	}

	// in: interleaved stereo, out: interleaved 6 channels (FL, FC, FR, RL, RR, LFE)
	void Decode(const float* in, float* out, unsigned frames)
	{
		if (m_settings.stereo_passthrough)
		{
			for (unsigned i = 0; i < frames; i++)
			{
				float* o = &out[i * NUM_OUTPUT_CHANNELS];
				o[0] = in[i * 2 + 0];
				o[2] = in[i * 2 + 1];
				o[1] = o[3] = o[4] = o[5] = 0.0f;
			}
			return;
		}

		for (unsigned i = 0; i < frames; i++)
		{
			float l = in[i * 2 + 0];
			float r = in[i * 2 + 1];

			// Music mode Dimension: shift the soundfield front/back by re-weighting sum and difference
			if (m_dim_sum != 1.0f)
			{
				const float sum = (l + r) * 0.5f * m_dim_sum, diff = (l - r) * 0.5f * m_dim_diff;
				l = sum + diff;
				r = sum - diff;
			}

			// Bass redirection (LR4 = two cascaded 2nd order Butterworths, sums flat in magnitude)
			float lfe = 0.0f;
			if (m_use_lfe)
			{
				const float bl = m_lp[0][1].Process(m_lp[0][0].Process(l));
				const float br = m_lp[1][1].Process(m_lp[1][0].Process(r));
				l = m_hp[0][1].Process(m_hp[0][0].Process(l));
				r = m_hp[1][1].Process(m_hp[1][0].Process(r));
				lfe = (bl + br) * 0.70710678f * m_settings.lfe_level;
			}

			// Control path
			float cl = m_ctl_hp[0].Process(l);
			float cr = m_ctl_hp[1].Process(r);
			m_ctl_raw[0] = cl;
			m_ctl_raw[1] = cr;

			// Autobalance: long-term (4 s) level of each input, corrects up to +/-2 dB of constant offset.
			// It only learns from diffuse, unsteered audio (ambience, music beds), so a deliberately one-sided
			// source such as a sustained rear-left effect is never "corrected" towards the middle.
			if (m_autobalance && m_audible)
			{
				const float diffuse = std::max(0.0f, 1.0f - 2.0f * std::sqrt(m_ax * m_ax + m_ay * m_ay));
				const float k = m_balance_coef * diffuse * diffuse;
				m_power[0] += k * (cl * cl - m_power[0]);
				m_power[1] += k * (cr * cr - m_power[1]);
				// Confidence grows with the amount of diffuse audio seen (about one time constant's worth),
				// so a brief moment before steering settles cannot lock in a correction.
				m_balance_conf = std::min(1.0f, m_balance_conf + k);
				const float bal = std::clamp(std::sqrt((m_power[1] + 1e-12f) / (m_power[0] + 1e-12f)), 0.7943f, 1.2589f);
				const float g = std::pow(bal, 0.5f * m_balance_conf);
				cl *= g;
				cr /= g;
			}

			// Full-band dominance, used to gate autobalance (it only learns from diffuse audio)
			{
				const std::array<float, 3> sq = {cl * cl, cr * cr, cl * cr};
				for (int k = 0; k < 3; k++)
					m_fast[k] += m_fast_coef * (sq[k] - m_fast[k]);
				const float tr = m_fast[0] + m_fast[1];
				m_audible = (tr > SILENCE_POWER);
				if (m_audible)
				{
					const float fx = (m_fast[0] - m_fast[1]) / tr, fy = (2.0f * m_fast[2]) / tr;
					m_ax += AdaptiveCoef(fx - m_ax) * (fx - m_ax);
					m_ay += AdaptiveCoef(fy - m_ay) * (fy - m_ay);
				}
			}
			const float bal_l = (cl != 0.0f && m_ctl_raw[0] != 0.0f) ? cl / m_ctl_raw[0] : 1.0f;
			const float bal_r = (cr != 0.0f && m_ctl_raw[1] != 0.0f) ? cr / m_ctl_raw[1] : 1.0f;

			std::array<float, 5> mix = {}; // fl, fc, fr, rs, ls
			if (!m_settings.multiband)
			{
				m_steer[0].Process(*this, cl, cr, l, r, mix);
			}
			else
			{
				float bl[4], br[4];
				SplitBands(0, l, bl);
				SplitBands(1, r, br);
				for (int b = 0; b < 4; b++)
				{
					// The lowest band keeps the 100 Hz high-pass on its control signal, like the single-band path
					const float ccl = (b == 0) ? m_band_ctl_hp[0].Process(bl[0]) : bl[b];
					const float ccr = (b == 0) ? m_band_ctl_hp[1].Process(br[0]) : br[b];
					m_steer[b].Process(*this, ccl * bal_l, ccr * bal_r, bl[b], br[b], mix);
				}
			}

			// LFE look-ahead, to stay aligned with the mains
			{
				const float dlfe = m_la_lfe[m_la_pos];
				m_la_lfe[m_la_pos] = lfe;
				m_la_pos = (m_la_pos + 1 == m_la_len) ? 0 : (m_la_pos + 1);
				lfe = dlfe;
			}

			float fl = mix[0], fc = mix[1], fr = mix[2], rs = mix[3], ls = mix[4];

			// Music mode Centre Width: spread the centre into a phantom image on L/R (constant power)
			if (m_center_keep != 1.0f)
			{
				fl += fc * m_center_spread;
				fr += fc * m_center_spread;
				fc *= m_center_keep;
			}

			// Music mode Panorama: wrap the front left/right image around into the surrounds
			if (m_panorama)
			{
				ls += fl * 0.5f;
				rs += fr * 0.5f;
				fl *= 0.8660254f;
				fr *= 0.8660254f;
			}

			// Music mode: mild high-frequency shelf (about -3 dB above ~4 kHz) on the surrounds
			if (m_rear_shelf)
			{
				ls -= 0.29f * m_shelf_hp[0].Process(ls);
				rs -= 0.29f * m_shelf_hp[1].Process(rs);
			}

			// Surround delay
			const float ls_d = m_delay[0][m_delay_pos];
			const float rs_d = m_delay[1][m_delay_pos];
			m_delay[0][m_delay_pos] = ls;
			m_delay[1][m_delay_pos] = rs;
			m_delay_pos = (m_delay_pos + 1 == m_delay_len) ? 0 : (m_delay_pos + 1);

			// Peak limiter, one gain for all six channels so the image does not shift. Steering can add a little
			// gain on loud, strongly steered passages (up to ~1.4x full scale), which would otherwise clip.
			// Instant attack, 150 ms release, threshold just under full scale; inaudible below it.
			const float peak = std::max({std::abs(fl), std::abs(fc), std::abs(fr), std::abs(ls_d), std::abs(rs_d), std::abs(lfe)});
			const float limit_target = (peak > LIMIT_THRESHOLD) ? (LIMIT_THRESHOLD / peak) : 1.0f;
			if (limit_target < m_limit_gain)
				m_limit_gain = limit_target;
			else
				m_limit_gain += m_release_coef * (limit_target - m_limit_gain);
			const float g = m_limit_gain;

			float* o = &out[i * NUM_OUTPUT_CHANNELS];
			o[0] = fl * g;
			o[1] = fc * g;
			o[2] = fr * g;
			o[3] = ls_d * g;
			o[4] = rs_d * g;
			o[5] = lfe * g;
		}
	}

private:
	struct Biquad
	{
		float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
		float z1 = 0, z2 = 0;

		void SetButterworth(float fc, float fs, bool highpass)
		{
			const float w = 2.0f * 3.14159265f * fc / fs;
			const float cw = std::cos(w), alpha = std::sin(w) / (2.0f * 0.70710678f);
			const float a0 = 1.0f + alpha;
			if (highpass)
			{
				b0 = (1.0f + cw) * 0.5f / a0;
				b1 = -(1.0f + cw) / a0;
			}
			else
			{
				b0 = (1.0f - cw) * 0.5f / a0;
				b1 = (1.0f - cw) / a0;
			}
			b2 = b0;
			a1 = -2.0f * cw / a0;
			a2 = (1.0f - alpha) / a0;
		}
		void SetFirstOrderHighpass(float fc, float fs)
		{
			const float k = std::tan(3.14159265f * fc / fs);
			b0 = 1.0f / (1.0f + k);
			b1 = -b0;
			b2 = 0.0f;
			a1 = (k - 1.0f) / (1.0f + k);
			a2 = 0.0f;
		}
		void Reset() { z1 = z2 = 0; }
		float Process(float x)
		{
			const float y = b0 * x + z1;
			z1 = b1 * x - a1 * y + z2;
			z2 = b2 * x - a2 * y;
			return y;
		}
	};

	float Coef(float ms) const { return 1.0f - std::exp(-1.0f / (ms * 0.001f * m_fs)); }

	float AdaptiveCoef(float deviation) const
	{
		const float t = std::min(1.0f, std::abs(deviation) / 0.3f);
		return m_slow_coef + (m_fast_coef - m_slow_coef) * t;
	}

	// Constant-power pairwise panning around the decode circle.
	// Output index order: 0 FL, 1 FC, 2 FR, 3 RR, 4 RL. L appears at both ends, inverted at 180 degrees
	// because the direction vector has flipped sign there.
	//
	// Out-of-phase (rear-half) sources are held fully in the surrounds over a wide zone. The reference rear
	// positions from a PLII encoder are ~125.3 (RR) and ~144.7 (RL), but games encoded surround themselves
	// (PS2 SPU2 phase-inverted voices) and often use more one-sided mixes. Like a receiver, only treat a source
	// as moving towards the front speakers once it is nearly single-channel.
	static void NodeGains(float theta, std::array<float, 5>& g)
	{
		static constexpr float ang[] = {0.0f, 45.0f, 90.0f, RR_EDGE, 125.26f, 144.74f, RL_EDGE, 180.0f};
		static constexpr int ch[] = {0, 1, 2, 3, 3, 4, 4, 0};
		static constexpr float sgn[] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, -1.0f};
		static constexpr int count = static_cast<int>(std::size(ang)) - 1;
		for (int i = 0; i < count; i++)
		{
			if (theta <= ang[i + 1] || i == count - 1)
			{
				const float t = std::clamp((theta - ang[i]) / (ang[i + 1] - ang[i]), 0.0f, 1.0f);
				if (ch[i] == ch[i + 1])
				{
					// plateau: stay fully in this speaker
					g[ch[i]] += sgn[i];
					return;
				}
				g[ch[i]] += sgn[i] * std::cos(t * 1.57079633f);
				g[ch[i + 1]] += sgn[i + 1] * std::sin(t * 1.57079633f);
				return;
			}
		}
	}

	// Angles where a rear source starts to move towards the front side speaker. 180/0 = L only, 90 = R only.
	// 168 corresponds to Lt:Rt of roughly 1 : -0.2; anything with more opposite-phase content stays in the rear.
	static constexpr float RL_EDGE = 168.0f;
	static constexpr float RR_EDGE = 102.0f;

	// One steering + matrix stage: tracks the dominant direction of its (Lt, Rt) input, cancels it from the
	// channels it does not belong to, and renders the rest through a passive matrix. The single-band decoder
	// uses one; the multi-band option uses one per band.
	struct Steerer
	{
		std::array<float, 3> fast = {}; // E[l^2], E[r^2], E[lr]
		float ax = 0.0f, ay = 0.0f;
		bool audible = false;
		std::array<float, 13> w = {};
		std::vector<float> la[2];
		unsigned la_pos = 0;

		void Init(unsigned la_len)
		{
			la[0].assign(la_len, 0.0f);
			la[1].assign(la_len, 0.0f);
			Reset();
		}

		void Reset()
		{
			fast = {};
			ax = ay = 0.0f;
			audible = false;
			w = {};
			std::fill(la[0].begin(), la[0].end(), 0.0f);
			std::fill(la[1].begin(), la[1].end(), 0.0f);
			la_pos = 0;
		}

		// cl/cr: control signal (not delayed). l/r: audio (delayed here by the look-ahead). Adds to mix.
		void Process(const DolbyProLogicIIDecoder& d, float cl, float cr, float l, float r, std::array<float, 5>& mix)
		{
			// Fast covariance of (Lt, Rt). Its principal axis is the direction of the dominant source, described
			// by two normalised axes: X = left/right, Y = front/back (in-phase vs out-of-phase).
			const std::array<float, 3> sq = {cl * cl, cr * cr, cl * cr};
			for (int k = 0; k < 3; k++)
				fast[k] += d.m_fast_coef * (sq[k] - fast[k]);
			// Below about -70 dBFS there is nothing meaningful to steer on (silence, or a filter's decaying tail),
			// so hold the current steering instead of letting it drift towards "diffuse" in every quiet gap.
			const float tr = fast[0] + fast[1];
			audible = (tr > SILENCE_POWER);
			if (audible)
			{
				const float fx = (fast[0] - fast[1]) / tr;
				const float fy = (2.0f * fast[2]) / tr;
				// Each axis follows the measurement at its own continuously variable speed: slow (150 ms) for
				// small deviations, rising to fast (8 ms) as the deviation grows.
				ax += d.AdaptiveCoef(fx - ax) * (fx - ax);
				ay += d.AdaptiveCoef(fy - ay) * (fy - ay);
			}
			const float dom = std::min(1.0f, std::sqrt(ax * ax + ay * ay));

			// Direction as an angle in [0, 180): 0 = L, 45 = C, 90 = R, ~125 = RR, 135 = rear centre, ~145 = RL.
			float theta = 0.5f * std::atan2(ay, ax) * (180.0f / 3.14159265f);
			if (theta < 0.0f)
				theta += 180.0f;
			const float ux = std::cos(theta * (3.14159265f / 180.0f)), uy = std::sin(theta * (3.14159265f / 180.0f));

			// Target weights: per output channel, a 2-vector applied to (Lt, Rt) for the steered part, and the
			// projection that removes the steered part from the ambience path. Both are continuous across the
			// 0/180 wrap, so they can be smoothed directly.
			std::array<float, 5> node = {};
			NodeGains(theta, node);
			std::array<float, 13> target;
			for (int c = 0; c < 5; c++)
			{
				target[c * 2 + 0] = dom * node[c] * ux;
				target[c * 2 + 1] = dom * node[c] * uy;
			}
			target[10] = dom * ux * ux;
			target[11] = dom * ux * uy;
			target[12] = dom * uy * uy;
			for (int k = 0; k < 13; k++)
				w[k] += d.m_gain_coef * (target[k] - w[k]);

			// Look-ahead: the control path above saw this audio LOOKAHEAD_MS ago, so the steering is already in
			// place when a sound starts. Without it, the first 20-40 ms of every new sound leaked into the
			// same-side front speaker before steering caught up (and arrived ahead of the delayed surround).
			const float dl = la[0][la_pos], dr = la[1][la_pos];
			la[0][la_pos] = l;
			la[1][la_pos] = r;
			la_pos = (la_pos + 1 == la[0].size()) ? 0 : (la_pos + 1);
			l = dl;
			r = dr;

			// Ambience: what is left after removing the steered source, through a passive matrix
			const float al = l - (w[10] * l + w[11] * r);
			const float ar = r - (w[11] * l + w[12] * r);
			mix[0] += w[0] * l + w[1] * r + al;
			mix[1] += w[2] * l + w[3] * r + (al + ar) * DPL2_AMB_C;
			mix[2] += w[4] * l + w[5] * r + ar;
			mix[3] += w[6] * l + w[7] * r + (-0.5774f * al + 0.8165f * ar) * 0.70710678f;
			mix[4] += w[8] * l + w[9] * r + (-0.8165f * al + 0.5774f * ar) * 0.70710678f;
		}
	};

	// 4-band Linkwitz-Riley split (crossovers 250 Hz, 1.2 kHz, 4 kHz) whose bands sum back to an all-pass,
	// so a single source steered the same way in every band comes out with a flat magnitude response.
	struct LR4
	{
		Biquad a, b;
		void Set(float fc, float fs, bool hp)
		{
			a.SetButterworth(fc, fs, hp);
			b.SetButterworth(fc, fs, hp);
		}
		void Reset()
		{
			a.Reset();
			b.Reset();
		}
		float Process(float x) { return b.Process(a.Process(x)); }
	};

	struct BandSplit
	{
		LR4 lp_mid, hp_mid; // at 1.2 kHz
		LR4 ap_hi_lp, ap_hi_hp; // all-pass at 4 kHz on the low branch
		LR4 lp_lo, hp_lo; // at 250 Hz
		LR4 ap_lo_lp, ap_lo_hp; // all-pass at 250 Hz on the high branch
		LR4 lp_hi, hp_hi; // at 4 kHz

		void Init(float fs)
		{
			lp_mid.Set(1200.0f, fs, false);
			hp_mid.Set(1200.0f, fs, true);
			ap_hi_lp.Set(4000.0f, fs, false);
			ap_hi_hp.Set(4000.0f, fs, true);
			lp_lo.Set(250.0f, fs, false);
			hp_lo.Set(250.0f, fs, true);
			ap_lo_lp.Set(250.0f, fs, false);
			ap_lo_hp.Set(250.0f, fs, true);
			lp_hi.Set(4000.0f, fs, false);
			hp_hi.Set(4000.0f, fs, true);
		}
		void Reset()
		{
			for (LR4* f : {&lp_mid, &hp_mid, &ap_hi_lp, &ap_hi_hp, &lp_lo, &hp_lo, &ap_lo_lp, &ap_lo_hp, &lp_hi, &hp_hi})
				f->Reset();
		}
		void Process(float x, float* bands)
		{
			float lo = lp_mid.Process(x);
			float hi = hp_mid.Process(x);
			lo = ap_hi_lp.Process(lo) + ap_hi_hp.Process(lo);
			hi = ap_lo_lp.Process(hi) + ap_lo_hp.Process(hi);
			bands[0] = lp_lo.Process(lo);
			bands[1] = hp_lo.Process(lo);
			bands[2] = lp_hi.Process(hi);
			bands[3] = hp_hi.Process(hi);
		}
	};

	void SplitBands(int ch, float x, float* bands) { m_split[ch].Process(x, bands); }

	float m_fs;
	Settings m_settings;
	bool m_use_lfe = false;
	bool m_autobalance = true;
	bool m_panorama = false;
	bool m_rear_shelf = false;
	float m_center_keep = 1.0f, m_center_spread = 0.0f;
	float m_dim_sum = 1.0f, m_dim_diff = 1.0f;

	Biquad m_lp[2][2], m_hp[2][2], m_ctl_hp[2], m_shelf_hp[2];

	float m_slow_coef, m_fast_coef, m_gain_coef, m_balance_coef;
	std::array<float, 3> m_fast = {}; // E[l^2], E[r^2], E[lr]
	std::array<float, 2> m_power = {}; // long-term level for autobalance
	float m_balance_conf = 0.0f;
	float m_ax = 0.0f, m_ay = 0.0f; // adaptive steering axes
	bool m_audible = false; // previous sample's control signal was above the silence floor

	static constexpr float SILENCE_POWER = 1e-7f; // about -70 dBFS

	std::vector<float> m_delay[2];
	unsigned m_delay_len = 1, m_delay_pos = 0;

	static constexpr float LIMIT_THRESHOLD = 0.97f;
#ifndef DPL2_LOOKAHEAD_MS
#define DPL2_LOOKAHEAD_MS 12.0f
#endif
#ifndef DPL2_GAIN_MS
#define DPL2_GAIN_MS 2.0f
#endif
	static constexpr float LOOKAHEAD_MS = DPL2_LOOKAHEAD_MS;
	static constexpr float GAIN_MS = DPL2_GAIN_MS;
	std::vector<float> m_la_lfe;
	unsigned m_la_len = 1, m_la_pos = 0;
	Steerer m_steer[4];
	BandSplit m_split[2];
	Biquad m_band_ctl_hp[2];
	float m_ctl_raw[2] = {};
	float m_limit_gain = 1.0f;
	float m_release_coef = 0.0f;
};
