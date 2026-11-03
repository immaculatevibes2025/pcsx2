// PS2 Dolby Pro Logic II surround test - test definitions
#pragma once

#include <stddef.h>

// Where a test sound is encoded.
//
// Every position is an angle "theta" on the Lt/Rt plane: Lt = cos(theta), Rt = sin(theta).
//   0 = Front Left, 45 = Centre, 90 = Front Right,
//   125.26 = Surround Right (PLII reference, Lt:Rt = -0.577 : 0.816),
//   135 = both surrounds (Lt = -Rt, fully out of phase),
//   144.74 = Surround Left (PLII reference, Lt:Rt = -0.816 : 0.577),
//   180 = Front Left again (the signal is just inverted).
// Out-of-phase positions are produced by a negative Lt or Rt gain, which is what PS2 games do with
// phase-inverted SPU2 voice volumes.

enum TestKind
{
	KIND_NOISE, // pink noise bursts at a fixed position
	KIND_SWEEP, // continuous pink noise moving around the room
	KIND_LFE, // 40 Hz tone bursts at the centre
	KIND_AMBIENCE, // uncorrelated pink noise in Lt and Rt
};

struct TestDef
{
	const char* name;
	const char* expected; // what a correct PLII decoder should do
	const char* encoding; // human readable Lt/Rt mix, NULL = computed from theta
	float theta;
	enum TestKind kind;
	int voice; // index into the spoken label table
};

#define DEG_FL 0.0f
#define DEG_C 45.0f
#define DEG_FR 90.0f
#define DEG_SR 125.26f
#define DEG_S 135.0f
#define DEG_SL 144.74f

static const struct TestDef g_tests[] = {
	{"Front Left", "Front Left speaker only", NULL, DEG_FL, KIND_NOISE, 0},
	{"Center", "Center speaker only", NULL, DEG_C, KIND_NOISE, 1},
	{"Front Right", "Front Right speaker only", NULL, DEG_FR, KIND_NOISE, 2},
	{"Surround Right", "Surround Right speaker only", NULL, DEG_SR, KIND_NOISE, 3},
	{"Surround Left", "Surround Left speaker only", NULL, DEG_SL, KIND_NOISE, 4},
	{"Both Surrounds", "Both surrounds equally, nothing in front", NULL, DEG_S, KIND_NOISE, 5},
	{"Between Front Left and Center", "Phantom image between Front Left and Center", NULL, 22.5f, KIND_NOISE, 6},
	{"Between Center and Front Right", "Phantom image between Center and Front Right", NULL, 67.5f, KIND_NOISE, 7},
	{"Surround Left, game style mix A", "Surround Left only (receivers keep this in the surround)", NULL, 153.43f, KIND_NOISE, 8},
	{"Surround Left, game style mix B", "Surround Left only (receivers keep this in the surround)", NULL, 163.30f, KIND_NOISE, 9},
	{"Surround Right, game style mix B", "Surround Right only (receivers keep this in the surround)", NULL, 106.70f, KIND_NOISE, 10},
	{"Circle Sweep", "Moves smoothly FL > C > FR > SR > SL > FL, no jumps", "Lt cos(t)  Rt sin(t)", 0.0f, KIND_SWEEP, 11},
	{"Subwoofer 40 Hz", "Subwoofer in Game mode, front speakers in Movie/Music", "40 Hz tone, Lt 0.707 Rt 0.707", DEG_C, KIND_LFE, 12},
	{"Ambience", "Spread evenly around the room, no single speaker", "Uncorrelated noise in Lt and Rt", 0.0f, KIND_AMBIENCE, 13},
};

#define NUM_TESTS ((int)(sizeof(g_tests) / sizeof(g_tests[0])))
