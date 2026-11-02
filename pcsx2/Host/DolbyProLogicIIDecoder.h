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
//  * Dual time constants: a slow envelope for stable steering and a fast one that takes over on transients.
//  * Full-bandwidth stereo surrounds, delayed by 10 ms (precedence effect keeps front sounds in front).
//  * Game-mode style bass redirection: a Linkwitz-Riley 4th order crossover splits bass off before decoding
//    and sends it to the LFE channel; the main channels get the complementary high-pass, so nothing is doubled.
//
// Output channel order matches FreeSurround's 5.1 layout: FL, FC, FR, RL, RR, LFE.

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

class DolbyProLogicIIDecoder
{
public:
	static constexpr unsigned NUM_OUTPUT_CHANNELS = 6;

	// lfe_crossover_hz: 0 disables bass redirection (full-range mains, silent LFE).
	// lfe_level: linear gain on the LFE channel. Receivers play the LFE channel of multichannel PCM 10 dB hotter,
	// so the default of -10 dB keeps bass level-matched with the main speakers.
	DolbyProLogicIIDecoder(unsigned sample_rate, float lfe_crossover_hz, float lfe_level = 0.31623f,
		float surround_delay_ms = 10.0f)
		: m_fs(static_cast<float>(sample_rate))
		, m_lfe_level(lfe_level)
	{
		m_use_lfe = lfe_crossover_hz > 0.0f;
		if (m_use_lfe)
		{
			for (int i = 0; i < 2; i++)
			{
				m_lp[0][i].SetButterworth(lfe_crossover_hz, m_fs, false);
				m_lp[1][i].SetButterworth(lfe_crossover_hz, m_fs, false);
				m_hp[0][i].SetButterworth(lfe_crossover_hz, m_fs, true);
				m_hp[1][i].SetButterworth(lfe_crossover_hz, m_fs, true);
			}
		}

		// The control path ignores deep bass (which is mostly mono and would drag steering to the centre)
		m_ctl_hp[0].SetButterworth(100.0f, m_fs, true);
		m_ctl_hp[1].SetButterworth(100.0f, m_fs, true);

		m_slow_coef = Coef(150.0f);
		m_fast_coef = Coef(8.0f);
		m_gain_coef = Coef(5.0f);

		m_delay_len = std::max(1u, static_cast<unsigned>(surround_delay_ms * 0.001f * m_fs + 0.5f));
		m_delay[0].assign(m_delay_len, 0.0f);
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
		m_slow = {};
		m_fast = {};
		m_w = {};
		std::fill(m_delay[0].begin(), m_delay[0].end(), 0.0f);
		std::fill(m_delay[1].begin(), m_delay[1].end(), 0.0f);
		m_delay_pos = 0;
	}

	// in: interleaved stereo, out: interleaved 6 channels (FL, FC, FR, RL, RR, LFE)
	void Decode(const float* in, float* out, unsigned frames)
	{
		for (unsigned i = 0; i < frames; i++)
		{
			float l = in[i * 2 + 0];
			float r = in[i * 2 + 1];

			// Bass redirection (LR4 = two cascaded 2nd order Butterworths, sums flat in magnitude)
			float lfe = 0.0f;
			if (m_use_lfe)
			{
				const float bl = m_lp[0][1].Process(m_lp[0][0].Process(l));
				const float br = m_lp[1][1].Process(m_lp[1][0].Process(r));
				l = m_hp[0][1].Process(m_hp[0][0].Process(l));
				r = m_hp[1][1].Process(m_hp[1][0].Process(r));
				lfe = (bl + br) * 0.70710678f * m_lfe_level;
			}

			// Control path: track the 2x2 covariance of (Lt, Rt) with a slow and a fast time constant.
			// The principal axis gives the direction of the dominant source on the Lt/Rt plane, and the
			// eigenvalue spread gives how dominant it is (1 = single source, 0 = diffuse/uncorrelated).
			const float cl = m_ctl_hp[0].Process(l);
			const float cr = m_ctl_hp[1].Process(r);
			const std::array<float, 3> sq = {cl * cl, cr * cr, cl * cr};
			for (int k = 0; k < 3; k++)
			{
				m_slow[k] += m_slow_coef * (sq[k] - m_slow[k]);
				m_fast[k] += m_fast_coef * (sq[k] - m_fast[k]);
			}
			const float dom_slow = Dominance(m_slow), dom_fast = Dominance(m_fast);
			const auto& cov = (dom_fast > dom_slow + 0.25f) ? m_fast : m_slow;
			const float dom = std::max(dom_slow, (dom_fast > dom_slow + 0.25f) ? dom_fast : 0.0f);

			// Direction as an angle in [0, 180): 0 = L, 45 = C, 90 = R, ~125 = RR, 135 = rear centre, ~145 = RL, 180 = L.
			float theta = 0.5f * std::atan2(2.0f * cov[2], cov[0] - cov[1]) * (180.0f / 3.14159265f);
			if (theta < 0.0f)
				theta += 180.0f;
			const float ux = std::cos(theta * (3.14159265f / 180.0f)), uy = std::sin(theta * (3.14159265f / 180.0f));

			// Target weights: per output channel, a 2-vector applied to (Lt, Rt) for the steered part, and the
			// projection matrix that removes the steered part from the ambience path. Both are continuous across
			// the 0/180 wrap, so they can be smoothed directly.
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
				m_w[k] += m_gain_coef * (target[k] - m_w[k]);

			// Ambience: what is left after removing the steered source, through a passive matrix
			const float al = l - (m_w[10] * l + m_w[11] * r);
			const float ar = r - (m_w[11] * l + m_w[12] * r);

			const float fl = m_w[0] * l + m_w[1] * r + al;
			const float fc = m_w[2] * l + m_w[3] * r + (al + ar) * 0.35355339f;
			const float fr = m_w[4] * l + m_w[5] * r + ar;
			const float rs = m_w[6] * l + m_w[7] * r + (-0.5774f * al + 0.8165f * ar) * 0.70710678f;
			const float ls = m_w[8] * l + m_w[9] * r + (-0.8165f * al + 0.5774f * ar) * 0.70710678f;

			// Surround delay
			const float ls_d = m_delay[0][m_delay_pos];
			const float rs_d = m_delay[1][m_delay_pos];
			m_delay[0][m_delay_pos] = ls;
			m_delay[1][m_delay_pos] = rs;
			m_delay_pos = (m_delay_pos + 1 == m_delay_len) ? 0 : (m_delay_pos + 1);

			float* o = &out[i * NUM_OUTPUT_CHANNELS];
			o[0] = fl;
			o[1] = fc;
			o[2] = fr;
			o[3] = ls_d;
			o[4] = rs_d;
			o[5] = lfe;
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

	static float Dominance(const std::array<float, 3>& c)
	{
		const float tr = c[0] + c[1];
		const float d = c[0] - c[1];
		return (tr > 1e-12f) ? std::min(1.0f, std::sqrt(d * d + 4.0f * c[2] * c[2]) / tr) : 0.0f;
	}

	// Constant-power pairwise panning around the decode circle.
	// Output index order: 0 FL, 1 FC, 2 FR, 3 RR, 4 RL. L appears at both ends, inverted at 180 degrees
	// because the direction vector has flipped sign there.
	static void NodeGains(float theta, std::array<float, 5>& g)
	{
		static constexpr float ang[] = {0.0f, 45.0f, 90.0f, 125.26f, 144.74f, 180.0f};
		static constexpr int ch[] = {0, 1, 2, 3, 4, 0};
		static constexpr float sgn[] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, -1.0f};
		for (int i = 0; i < 5; i++)
		{
			if (theta <= ang[i + 1] || i == 4)
			{
				const float t = std::clamp((theta - ang[i]) / (ang[i + 1] - ang[i]), 0.0f, 1.0f);
				g[ch[i]] += sgn[i] * std::cos(t * 1.57079633f);
				g[ch[i + 1]] += sgn[i + 1] * std::sin(t * 1.57079633f);
				return;
			}
		}
	}

	float m_fs;
	float m_lfe_level;
	bool m_use_lfe = false;

	Biquad m_lp[2][2], m_hp[2][2], m_ctl_hp[2];

	float m_slow_coef, m_fast_coef, m_gain_coef;
	std::array<float, 3> m_slow = {}, m_fast = {}; // E[l^2], E[r^2], E[lr]
	std::array<float, 13> m_w = {};

	std::vector<float> m_delay[2];
	unsigned m_delay_len = 1, m_delay_pos = 0;
};
