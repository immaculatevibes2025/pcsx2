// PS2 Dolby Pro Logic II surround test
//
// Plays test sounds matrix-encoded the way PS2 games encode Pro Logic II: positions are set by the
// level of the sound in Lt and Rt, and surround positions use an out-of-phase (negative) gain.
// Output it as plain stereo and let a decoder (PCSX2's expansion or a receiver) sort out the speakers.

#include "audio.h"
#include "tests.h"

#include <audsrv.h>
#include <dmaKit.h>
#include <gsKit.h>
#include <iopcontrol.h>
#include <kernel.h>
#include <libpad.h>
#include <loadfile.h>
#include <math.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>

extern unsigned char libsd_irx[];
extern unsigned int size_libsd_irx;
extern unsigned char audsrv_irx[];
extern unsigned int size_audsrv_irx;
extern unsigned char sio2man_irx[];
extern unsigned int size_sio2man_irx;
extern unsigned char padman_irx[];
extern unsigned int size_padman_irx;

extern const struct VoiceClip g_voices[];

#define PI 3.14159265f
#define AUDIO_CHUNK 512

// ---------------------------------------------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------------------------------------------

#define RGBA(r, g, b, a) GS_SETREG_RGBAQ((r), (g), (b), (a), 0x00)
#define COL_BG RGBA(0x0c, 0x0f, 0x16, 0x80)
#define COL_PANEL RGBA(0x16, 0x1b, 0x26, 0x80)
#define COL_ROOM RGBA(0x10, 0x14, 0x1e, 0x80)
#define COL_EDGE RGBA(0x2a, 0x33, 0x45, 0x80)
#define COL_TEXT RGBA(0xe6, 0xea, 0xf2, 0x80)
#define COL_DIM RGBA(0x80, 0x8a, 0x9c, 0x80)
#define COL_ACCENT RGBA(0x3d, 0xa9, 0xfc, 0x80)
#define COL_GLOW RGBA(0x3d, 0xa9, 0xfc, 0x30)
#define COL_SPK RGBA(0x30, 0x3a, 0x4e, 0x80)
#define COL_SPK_ON RGBA(0x3d, 0xa9, 0xfc, 0x80)
#define COL_SEL RGBA(0x22, 0x2b, 0x3c, 0x80)
#define COL_GREEN RGBA(0x4c, 0xd1, 0x7a, 0x80)

static GSGLOBAL* gs;
static GSFONTM* font;

static void Rect(float x1, float y1, float x2, float y2, u64 c)
{
	gsKit_prim_sprite(gs, x1, y1, x2, y2, 1, c);
}

static void Text(float x, float y, float scale, u64 c, const char* s)
{
	gsKit_fontm_print_scaled(gs, font, x, y, 2, scale, c, s);
}

static void Disc(float cx, float cy, float rad, u64 c)
{
	const int seg = 24;
	for (int i = 0; i < seg; i++)
	{
		const float a0 = 2 * PI * i / seg, a1 = 2 * PI * (i + 1) / seg;
		gsKit_prim_triangle(gs, cx, cy, cx + rad * cosf(a0), cy + rad * sinf(a0), cx + rad * cosf(a1),
			cy + rad * sinf(a1), 1, c);
	}
}

static void Ring(float cx, float cy, float rad, u64 c)
{
	const int seg = 64;
	for (int i = 0; i < seg; i++)
	{
		const float a0 = 2 * PI * i / seg, a1 = 2 * PI * (i + 1) / seg;
		gsKit_prim_line(gs, cx + rad * cosf(a0), cy + rad * sinf(a0), cx + rad * cosf(a1), cy + rad * sinf(a1), 1, c);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Room diagram
// ---------------------------------------------------------------------------------------------------------------

struct Speaker
{
	const char* label;
	float az; // degrees, 0 = front, clockwise
};

static const struct Speaker g_speakers[] = {
	{"FL", -30.0f}, {"C", 0.0f}, {"FR", 30.0f}, {"SR", 100.0f}, {"SL", -100.0f}};

static float AzDiff(float a, float b)
{
	float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f;
	return fabsf(d);
}

static void DrawRoom(float cx, float cy, float rad, const struct AudioState* a, int frame)
{
	Rect(cx - rad - 30, cy - rad - 30, cx + rad + 30, cy + rad + 30, COL_ROOM);
	Ring(cx, cy, rad, COL_EDGE);
	Ring(cx, cy, rad * 0.5f, COL_EDGE);

	const struct TestDef* t = a->test;
	const int active = a->playing && t;
	const int sounding = active && (a->phase == PHASE_BURST || a->phase == PHASE_VOICE);
	const int spread = active && t->kind == KIND_AMBIENCE;
	const float az = active ? Audio_ThetaToAzimuth(a->current_theta) : 0.0f;

	for (unsigned i = 0; i < sizeof(g_speakers) / sizeof(g_speakers[0]); i++)
	{
		const struct Speaker* s = &g_speakers[i];
		const float sx = cx + rad * sinf(s->az * PI / 180.0f);
		const float sy = cy - rad * cosf(s->az * PI / 180.0f);
		float on = 0.0f;
		if (sounding)
		{
			if (spread)
				on = 0.5f;
			else if (t->kind != KIND_LFE)
			{
				// Behind the listener (between the side surrounds) both surrounds share the sound
				const float d = (AzDiff(az, 180.0f) < 80.0f && fabsf(s->az) > 90.0f) ?
									AzDiff(az, 180.0f) * 0.5f + (AzDiff(az, s->az) < AzDiff(az, -s->az) ? 0.0f : 20.0f) :
									AzDiff(az, s->az);
				on = fmaxf(0.0f, 1.0f - d / 45.0f);
			}
		}
		if (on > 0.0f)
			Disc(sx, sy, 22.0f + 6.0f * on, COL_GLOW);
		Rect(sx - 13, sy - 13, sx + 13, sy + 13, on > 0.3f ? COL_SPK_ON : COL_SPK);
		Text(sx - 8 * (float)strlen(s->label) * 0.5f, sy - 6, 0.5f, COL_TEXT, s->label);
	}

	// Subwoofer
	const int sub_on = sounding && t->kind == KIND_LFE;
	if (sub_on)
		Disc(cx + rad * 0.62f, cy - rad * 0.62f, 18.0f, COL_GLOW);
	Rect(cx + rad * 0.62f - 11, cy - rad * 0.62f - 11, cx + rad * 0.62f + 11, cy - rad * 0.62f + 11,
		sub_on ? COL_SPK_ON : COL_SPK);
	Text(cx + rad * 0.62f - 8, cy - rad * 0.62f - 6, 0.5f, COL_TEXT, "SW");

	// Listener
	Disc(cx, cy, 7.0f, COL_DIM);
	Text(cx - 24, cy + 12, 0.45f, COL_DIM, "you");

	// Expected position marker
	if (active && !spread && t->kind != KIND_LFE)
	{
		const float px = cx + rad * 0.82f * sinf(az * PI / 180.0f);
		const float py = cy - rad * 0.82f * cosf(az * PI / 180.0f);
		const float pulse = sounding ? 1.0f + 0.25f * sinf(frame * 0.25f) : 0.7f;
		Disc(px, py, 9.0f * pulse, COL_GLOW);
		Disc(px, py, 5.0f, COL_ACCENT);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Pad
// ---------------------------------------------------------------------------------------------------------------

static char pad_buf[256] __attribute__((aligned(64)));
static u32 pad_old;

static u32 PadPressed(void)
{
	struct padButtonStatus st;
	const int state = padGetState(0, 0);
	if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1)
		return 0;
	if (padRead(0, 0, &st) == 0)
		return 0;
	const u32 now = 0xffff ^ st.btns;
	const u32 pressed = now & ~pad_old;
	pad_old = now;
	return pressed;
}

// ---------------------------------------------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------------------------------------------

static void LoadModules(void)
{
	SifInitRpc(0);
	while (!SifIopReset("", 0))
		;
	while (!SifIopSync())
		;
	SifInitRpc(0);
	sbv_patch_enable_lmb();
	sbv_patch_disable_prefix_check();

	int ret;
	SifExecModuleBuffer(libsd_irx, size_libsd_irx, 0, NULL, &ret);
	SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, NULL, &ret);
	SifExecModuleBuffer(sio2man_irx, size_sio2man_irx, 0, NULL, &ret);
	SifExecModuleBuffer(padman_irx, size_padman_irx, 0, NULL, &ret);
}

int main(void)
{
	LoadModules();

	// Graphics
	gs = gsKit_init_global();
	gs->PrimAlphaEnable = GS_SETTING_ON;
	gs->DoubleBuffering = GS_SETTING_ON;
	gs->ZBuffering = GS_SETTING_OFF;
	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);
	gsKit_init_screen(gs);
	gsKit_mode_switch(gs, GS_ONESHOT);
	font = gsKit_init_fontm();
	gsKit_fontm_upload(gs, font);
	font->Spacing = 0.95f;

	// Audio
	audsrv_init();
	struct audsrv_fmt_t fmt = {AUDIO_RATE, 16, 2};
	audsrv_set_format(&fmt);
	audsrv_set_volume(MAX_VOLUME);
	static short chunk[AUDIO_CHUNK * 2] __attribute__((aligned(64)));
	struct AudioState audio;
	Audio_Init(&audio, g_voices);

	// Pad
	padInit(0);
	padPortOpen(0, 0, pad_buf);

	int sel = 0, frame = 0, autoplay = 0;
	const float W = gs->Width, H = gs->Height;
	char line[96];

	for (;;)
	{
		const u32 p = PadPressed();
		if (p & (PAD_UP | PAD_LEFT))
		{
			sel = (sel + NUM_TESTS - 1) % NUM_TESTS;
			if (audio.playing)
				Audio_Start(&audio, &g_tests[sel]);
		}
		if (p & (PAD_DOWN | PAD_RIGHT))
		{
			sel = (sel + 1) % NUM_TESTS;
			if (audio.playing)
				Audio_Start(&audio, &g_tests[sel]);
		}
		if (p & PAD_CROSS)
		{
			autoplay = 0;
			Audio_Start(&audio, &g_tests[sel]);
		}
		if (p & PAD_START)
		{
			autoplay = 1;
			Audio_Start(&audio, &g_tests[sel]);
		}
		if (p & PAD_CIRCLE)
		{
			autoplay = 0;
			Audio_Stop(&audio);
		}

		// Auto mode: three bursts (one lap for the sweep), then next test
		if (autoplay && audio.playing)
		{
			const int done = (audio.test->kind == KIND_SWEEP) ?
								 (audio.phase == PHASE_BURST && audio.sweep_theta > 179.0f) :
								 (audio.bursts_done >= 3);
			if (done)
			{
				sel = (sel + 1) % NUM_TESTS;
				Audio_Start(&audio, &g_tests[sel]);
			}
		}

		// Keep the audio buffer topped up
		while (audsrv_available() >= (int)sizeof(chunk))
		{
			Audio_Render(&audio, chunk, AUDIO_CHUNK);
			audsrv_play_audio((const char*)chunk, sizeof(chunk));
		}

		// ---- draw ----
		gsKit_clear(gs, COL_BG);

		Rect(0, 0, W, 44, COL_PANEL);
		Text(20, 12, 0.75f, COL_TEXT, "Pro Logic II Surround Test");
		Text(W - 150, 16, 0.5f, COL_DIM, "PS2 / PCSX2");

		const float rcx = 170, rcy = 250, rr = 120;
		DrawRoom(rcx, rcy, rr, &audio, frame);

		// Test list
		const float lx = 330, ly = 62;
		Rect(lx - 10, ly - 8, W - 14, ly + NUM_TESTS * 17 + 4, COL_PANEL);
		for (int i = 0; i < NUM_TESTS; i++)
		{
			const float y = ly + i * 17;
			if (i == sel)
				Rect(lx - 6, y - 2, W - 18, y + 15, COL_SEL);
			snprintf(line, sizeof(line), "%2d  %s", i + 1, g_tests[i].name);
			Text(lx, y, 0.48f, i == sel ? COL_TEXT : COL_DIM, line);
			if (i == sel && audio.playing)
				Disc(W - 30, y + 6, 4.0f, (frame / 15) & 1 ? COL_GREEN : COL_ACCENT);
		}

		// Details for the selected test
		const struct TestDef* t = &g_tests[sel];
		const float dy = ly + NUM_TESTS * 17 + 18;
		Text(lx, dy, 0.45f, COL_ACCENT, "EXPECTED");
		Text(lx, dy + 16, 0.45f, COL_TEXT, t->expected);
		Text(lx, dy + 38, 0.45f, COL_ACCENT, "ENCODED AS");
		if (t->encoding)
			snprintf(line, sizeof(line), "%s", t->encoding);
		else
			snprintf(line, sizeof(line), "Lt %+.3f   Rt %+.3f", cosf(t->theta * PI / 180.0f), sinf(t->theta * PI / 180.0f));
		Text(lx, dy + 54, 0.45f, COL_TEXT, line);

		// Footer
		Rect(0, H - 34, W, H, COL_PANEL);
		Text(20, H - 25, 0.45f, COL_DIM, "X play   O stop   START auto-cycle   D-pad select");
		if (autoplay)
			Text(W - 110, H - 25, 0.45f, COL_GREEN, "AUTO");

		gsKit_queue_exec(gs);
		gsKit_sync_flip(gs);
		frame++;
	}
	return 0;
}
