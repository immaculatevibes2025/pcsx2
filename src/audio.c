// PS2 Dolby Pro Logic II surround test - signal generator
#include "audio.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265f

#define GAP_SAMPLES (AUDIO_RATE * 3 / 10) // after the spoken label
#define BURST_SAMPLES (AUDIO_RATE * 6 / 10)
#define PAUSE_SAMPLES (AUDIO_RATE * 4 / 10)
#define FADE_SAMPLES (AUDIO_RATE / 100) // 10 ms fades so bursts don't click
#define SWEEP_DEG_PER_SAMPLE (180.0f / (AUDIO_RATE * 12.0f)) // full circle in 12 s
#define FAST_SWEEP_DEG_PER_SAMPLE (180.0f / (AUDIO_RATE * 2.5f)) // full circle in 2.5 s
#define DIALOGUE_GAP (AUDIO_RATE / 2)

#define NOISE_GAIN 0.25f // about -18 dBFS RMS pink noise
#define VOICE_GAIN 0.9f
#define LFE_GAIN 0.35f

static float Pink(struct AudioState* a, int g)
{
	// LCG white noise, Paul Kellet's refined pink filter
	a->rng[g] = a->rng[g] * 1664525u + 1013904223u;
	const float white = ((int)(a->rng[g] >> 8) - 0x800000) / 8388608.0f;
	float* b = a->pk[g];
	b[0] = 0.99886f * b[0] + white * 0.0555179f;
	b[1] = 0.99332f * b[1] + white * 0.0750759f;
	b[2] = 0.96900f * b[2] + white * 0.1538520f;
	b[3] = 0.86650f * b[3] + white * 0.3104856f;
	b[4] = 0.55000f * b[4] + white * 0.5329522f;
	b[5] = -0.7616f * b[5] - white * 0.0168980f;
	const float pink = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + white * 0.5362f;
	b[6] = white * 0.115926f;
	return pink * 0.11f;
}

static float Fade(unsigned int pos, unsigned int len)
{
	if (pos < FADE_SAMPLES)
		return (float)pos / FADE_SAMPLES;
	if (pos + FADE_SAMPLES > len)
		return (float)(len - pos) / FADE_SAMPLES;
	return 1.0f;
}

static short Clip(float v)
{
	int i = (int)(v * 32767.0f);
	if (i > 32767)
		i = 32767;
	if (i < -32768)
		i = -32768;
	return (short)i;
}

void Audio_Init(struct AudioState* a, const struct VoiceClip* voices)
{
	memset(a, 0, sizeof(*a));
	a->voices = voices;
	a->rng[0] = 0x12345678u;
	a->rng[1] = 0x9abcdef1u;
}

void Audio_Start(struct AudioState* a, const struct TestDef* test)
{
	a->test = test;
	a->phase = PHASE_VOICE;
	a->pos = 0;
	a->bursts_done = 0;
	a->sweep_theta = 0.0f;
	a->laps = 0;
	a->dialogue_pos = 0;
	a->lfe_phase = 0.0f;
	a->current_theta = test->theta;
	a->playing = 1;
}

void Audio_Stop(struct AudioState* a)
{
	a->playing = 0;
}

void Audio_Render(struct AudioState* a, short* out, unsigned int frames)
{
	for (unsigned int i = 0; i < frames; i++)
	{
		float l = 0.0f, r = 0.0f;
		if (a->playing && a->test->kind != KIND_DTS)
		{
			const struct TestDef* t = a->test;
			// The spoken label comes from the test position too (the sweep announces from its start, Front Left;
			// ambience and the two-source test announce from the centre)
			float theta = (t->kind == KIND_AMBIENCE || (t->kind == KIND_DUAL && a->phase == PHASE_VOICE)) ? DEG_C : t->theta;

			switch (a->phase)
			{
				case PHASE_VOICE:
				{
					const struct VoiceClip* v = &a->voices[t->voice];
					if (a->pos >= v->count)
					{
						a->phase = PHASE_GAP;
						a->pos = 0;
						break;
					}
					const float s = v->samples[a->pos] / 32768.0f * VOICE_GAIN;
					l = cosf(theta * PI / 180.0f) * s;
					r = sinf(theta * PI / 180.0f) * s;
					a->pos++;
					break;
				}

				case PHASE_GAP:
					// voice-sample tests wait longer, so the streamed label has finished before the sample starts
					if (++a->pos >= ((t->kind == KIND_VOICE_SWEEP || t->kind == KIND_VOICE_NOISE) ? GAP_SAMPLES * 4 : GAP_SAMPLES))
					{
						a->phase = PHASE_BURST;
						a->pos = 0;
					}
					break;

				case PHASE_BURST:
				{
					// Voice-sample tests: the sample plays on an SPU2 voice (started by main), nothing streamed
					if (t->kind == KIND_VOICE_SWEEP || t->kind == KIND_VOICE_NOISE)
						break;
					if (t->kind == KIND_SWEEP || t->kind == KIND_FAST_SWEEP)
					{
						// continuous, never pauses
						theta = a->sweep_theta;
						a->sweep_theta += (t->kind == KIND_FAST_SWEEP) ? FAST_SWEEP_DEG_PER_SAMPLE : SWEEP_DEG_PER_SAMPLE;
						if (a->sweep_theta >= 180.0f)
						{
							a->sweep_theta -= 180.0f;
							a->laps++;
						}
						const float s = Pink(a, 0) * NOISE_GAIN * ((a->pos < FADE_SAMPLES) ? (float)a->pos / FADE_SAMPLES : 1.0f);
						l = cosf(theta * PI / 180.0f) * s;
						r = sinf(theta * PI / 180.0f) * s;
						a->current_theta = theta;
						if (a->pos < FADE_SAMPLES)
							a->pos++;
						break;
					}

					const float f = Fade(a->pos, BURST_SAMPLES);
					if (t->kind == KIND_AMBIENCE)
					{
						l = Pink(a, 0) * NOISE_GAIN * 0.7071f * f;
						r = Pink(a, 1) * NOISE_GAIN * 0.7071f * f;
					}
					else
					{
						float s;
						if (t->kind == KIND_LFE)
						{
							s = sinf(a->lfe_phase) * LFE_GAIN * f;
							a->lfe_phase += 2.0f * PI * 40.0f / AUDIO_RATE;
							if (a->lfe_phase > 2.0f * PI)
								a->lfe_phase -= 2.0f * PI;
						}
						else if (t->kind == KIND_LEVELS)
						{
							static const float levels[3] = {1.0f, 0.1f, 0.01f}; // 0, -20, -40 dB
							s = Pink(a, 0) * NOISE_GAIN * levels[a->bursts_done % 3] * f;
						}
						else
						{
							s = Pink(a, 0) * NOISE_GAIN * f;
						}
						l = cosf(theta * PI / 180.0f) * s;
						r = sinf(theta * PI / 180.0f) * s;
					}

					if (++a->pos >= BURST_SAMPLES)
					{
						a->phase = PHASE_PAUSE;
						a->pos = 0;
						a->bursts_done++;
					}
					break;
				}

				case PHASE_PAUSE:
					if (++a->pos >= PAUSE_SAMPLES)
					{
						a->phase = PHASE_BURST;
						a->pos = 0;
					}
					break;
			}

			// Two-source test: dialogue in the centre runs continuously under the bursts
			if (t->kind == KIND_DUAL && (a->phase == PHASE_BURST || a->phase == PHASE_PAUSE))
			{
				const struct VoiceClip* v = &a->voices[VOICE_DIALOGUE];
				if (a->dialogue_pos < v->count)
				{
					const float s = v->samples[a->dialogue_pos] / 32768.0f * VOICE_GAIN * 0.8f;
					l += 0.70710678f * s;
					r += 0.70710678f * s;
				}
				if (++a->dialogue_pos >= v->count + DIALOGUE_GAP)
					a->dialogue_pos = 0;
			}
		}

		out[i * 2 + 0] = Clip(l);
		out[i * 2 + 1] = Clip(r);
	}
}

float Audio_ThetaToAzimuth(float theta)
{
	// Lt/Rt angle -> where a PLII decoder puts it, for a 5.1 layout with surrounds at the sides (+/-100 deg).
	// Like a receiver (and PCSX2's PLII mode), out-of-phase sources stay fully in the surround until they are
	// nearly single-channel, so the game style mixes are shown in the surround speaker.
	static const float th[] = {0.0f, 45.0f, 90.0f, 102.0f, 125.26f, 135.0f, 144.74f, 168.0f, 180.0f};
	static const float az[] = {-30.0f, 0.0f, 30.0f, 100.0f, 100.0f, 180.0f, 260.0f, 260.0f, 330.0f};
	if (theta <= 0.0f)
		return az[0];
	for (int i = 0; i < 8; i++)
	{
		if (theta <= th[i + 1])
		{
			const float t = (theta - th[i]) / (th[i + 1] - th[i]);
			return az[i] + (az[i + 1] - az[i]) * t;
		}
	}
	return az[8];
}
