// PS2 Dolby Pro Logic II surround test - signal generator
#pragma once

#include "tests.h"

#define AUDIO_RATE 48000

struct VoiceClip
{
	const short* samples; // mono, 48 kHz
	unsigned int count;
};

enum Phase
{
	PHASE_VOICE,
	PHASE_GAP,
	PHASE_BURST,
	PHASE_PAUSE,
};

struct AudioState
{
	const struct TestDef* test;
	const struct VoiceClip* voices;
	enum Phase phase;
	unsigned int pos; // samples into the current phase
	unsigned int bursts_done;
	float sweep_theta; // current sweep angle (degrees, 0..180)
	unsigned int laps; // completed sweep laps
	unsigned int dialogue_pos; // position in the looped dialogue (two-source test)
	float lfe_phase;
	float current_theta; // angle being played right now (for the display)
	int playing;
	// pink noise state, two independent generators (for ambience)
	unsigned int rng[2];
	float pk[2][7];
};

void Audio_Init(struct AudioState* a, const struct VoiceClip* voices);
void Audio_Start(struct AudioState* a, const struct TestDef* test);
void Audio_Stop(struct AudioState* a);
// Interleaved stereo s16 output
void Audio_Render(struct AudioState* a, short* out, unsigned int frames);
// Map an Lt/Rt angle to a room azimuth in degrees (0 = front centre, clockwise, side surround layout)
float Audio_ThetaToAzimuth(float theta);
