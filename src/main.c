// PS2 Dolby Pro Logic II surround test
//
// Plays test sounds matrix-encoded the way PS2 games encode Pro Logic II: positions are set by the
// level of the sound in Lt and Rt, and surround positions use an out-of-phase (negative) gain.
// Output it as plain stereo and let a decoder (PCSX2's expansion or a receiver) sort out the speakers.

#include "audio.h"
#include "tests.h"
#include "dts.h"

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
#include <malloc.h>
#include <string.h>

extern unsigned char libsd_irx[];
extern unsigned int size_libsd_irx;
extern unsigned char audsrv_irx[];
extern unsigned int size_audsrv_irx;
extern unsigned char sio2man_irx[];
extern unsigned int size_sio2man_irx;
extern unsigned char dtsplay_irx[];
extern unsigned int size_dtsplay_irx;
extern unsigned char padman_irx[];
extern unsigned int size_padman_irx;

extern const struct VoiceClip g_voices[];
extern unsigned char vag_sweep[], vag_noise[];
extern const int vag_sweep_size, vag_noise_size;

#define TEST_VOICE_CH 0

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
static GSTEXTURE font_tex;

extern const int g_font_tex_w, g_font_tex_h, g_font_cell_w, g_font_cell_h;
extern const int g_font_ascent, g_font_first, g_font_last, g_font_cols;
extern const unsigned char g_font_adv[];
extern const unsigned char g_font_alpha[];

// Everything is drawn from triangles: plain sprites did not show up (no background clear, no panels).
static void Quad(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, u64 c)
{
	gsKit_prim_triangle(gs, x1, y1, x2, y2, x3, y3, 1, c);
	gsKit_prim_triangle(gs, x2, y2, x4, y4, x3, y3, 1, c);
}

static void Rect(float x1, float y1, float x2, float y2, u64 c)
{
	Quad(x1, y1, x2, y1, x1, y2, x2, y2, c);
}

// Thick line segment
static void Bar(float x1, float y1, float x2, float y2, float w, u64 c)
{
	float dx = x2 - x1, dy = y2 - y1;
	const float len = sqrtf(dx * dx + dy * dy);
	if (len <= 0.0f)
		return;
	const float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
	Quad(x1 + nx, y1 + ny, x2 + nx, y2 + ny, x1 - nx, y1 - ny, x2 - nx, y2 - ny, c);
}

static void Arc(float cx, float cy, float r0, float r1, float a0, float a1, int seg, u64 c)
{
	for (int i = 0; i < seg; i++)
	{
		const float t0 = a0 + (a1 - a0) * i / seg, t1 = a0 + (a1 - a0) * (i + 1) / seg;
		Quad(cx + r1 * cosf(t0), cy + r1 * sinf(t0), cx + r1 * cosf(t1), cy + r1 * sinf(t1), cx + r0 * cosf(t0),
			cy + r0 * sinf(t0), cx + r0 * cosf(t1), cy + r0 * sinf(t1), c);
	}
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
	Arc(cx, cy, rad - 1.0f, rad + 1.0f, 0.0f, 2 * PI, 64, c);
}

static void RoundRect(float x1, float y1, float x2, float y2, float r, u64 c)
{
	Rect(x1 + r, y1, x2 - r, y2, c);
	Rect(x1, y1 + r, x1 + r, y2 - r, c);
	Rect(x2 - r, y1 + r, x2, y2 - r, c);
	Arc(x1 + r, y1 + r, 0.0f, r, PI, 1.5f * PI, 6, c);
	Arc(x2 - r, y1 + r, 0.0f, r, 1.5f * PI, 2.0f * PI, 6, c);
	Arc(x1 + r, y2 - r, 0.0f, r, 0.5f * PI, PI, 6, c);
	Arc(x2 - r, y2 - r, 0.0f, r, 0.0f, 0.5f * PI, 6, c);
}

// ---------------------------------------------------------------------------------------------------------------
// Text (proportional font atlas generated at build time; scale 1.0 = 20 px)
// ---------------------------------------------------------------------------------------------------------------

static void FontInit(void)
{
	const int n = g_font_tex_w * g_font_tex_h;
	u32* px = (u32*)memalign(128, n * 4);
	for (int i = 0; i < n; i++)
		px[i] = g_font_alpha[i] ? (((u32)g_font_alpha[i] << 24) | 0x00ffffffu) : 0u;
	font_tex.Width = g_font_tex_w;
	font_tex.Height = g_font_tex_h;
	font_tex.PSM = GS_PSM_CT32;
	font_tex.Mem = px;
	font_tex.Clut = NULL;
	font_tex.Filter = GS_FILTER_LINEAR;
	font_tex.Vram = gsKit_vram_alloc(gs, gsKit_texture_size(font_tex.Width, font_tex.Height, font_tex.PSM), GSKIT_ALLOC_USERBUFFER);
	gsKit_texture_upload(gs, &font_tex);
}

static float TextWidth(float scale, const char* s)
{
	float w = 0.0f;
	for (; *s; s++)
	{
		const int ch = (unsigned char)*s;
		if (ch >= g_font_first && ch <= g_font_last)
			w += g_font_adv[ch - g_font_first] * scale;
	}
	return w;
}

static void Text(float x, float y, float scale, u64 c, const char* s)
{
	for (; *s; s++)
	{
		const int ch = (unsigned char)*s;
		if (ch < g_font_first || ch > g_font_last)
			continue;
		const int i = ch - g_font_first;
		if (ch != ' ')
		{
			const float u = (float)((i % g_font_cols) * g_font_cell_w), v = (float)((i / g_font_cols) * g_font_cell_h);
			const float gw = (float)(g_font_adv[i] + 2);
			const float w = gw * scale, h = g_font_cell_h * scale;
			gsKit_prim_quad_texture(gs, &font_tex, x, y, u, v, x + w, y, u + gw, v, x, y + h, u,
				v + g_font_cell_h, x + w, y + h, u + gw, v + g_font_cell_h, 2, c);
		}
		x += g_font_adv[i] * scale;
	}
}

// Word-wrapped text, returns the height used
static float TextWrap(float x, float y, float max_w, float scale, u64 c, const char* s)
{
	char line[128];
	const float lh = g_font_cell_h * scale;
	float yy = y;
	while (*s)
	{
		int len = 0, last_space = -1;
		while (s[len] && len < (int)sizeof(line) - 1)
		{
			if (s[len] == ' ')
				last_space = len;
			memcpy(line, s, len + 1);
			line[len + 1] = 0;
			if (TextWidth(scale, line) > max_w && last_space > 0)
			{
				len = last_space;
				break;
			}
			len++;
		}
		memcpy(line, s, len);
		line[len] = 0;
		Text(x, yy, scale, c, line);
		yy += lh;
		s += len;
		while (*s == ' ')
			s++;
	}
	return yy - y;
}

// ---------------------------------------------------------------------------------------------------------------
// PS2 controller button icons
// ---------------------------------------------------------------------------------------------------------------

#define COL_BTN RGBA(0x24, 0x28, 0x32, 0x80)
#define COL_BTN_EDGE RGBA(0x3a, 0x40, 0x4e, 0x80)
#define COL_CROSS RGBA(0x7c, 0xb2, 0xff, 0x80)
#define COL_CIRCLE RGBA(0xff, 0x6b, 0x70, 0x80)
#define COL_TRIANGLE RGBA(0x4f, 0xd6, 0xa6, 0x80)
#define COL_SQUARE RGBA(0xf2, 0x8c, 0xd6, 0x80)

enum Button
{
	BTN_CROSS,
	BTN_CIRCLE,
	BTN_TRIANGLE,
	BTN_SQUARE,
	BTN_START,
	BTN_DPAD,
};

// Draws a button icon centred vertically on cy, starting at x. Returns its width.
static float ButtonIcon(float x, float cy, enum Button b)
{
	const float r = 11.0f;
	switch (b)
	{
		case BTN_CROSS:
		case BTN_CIRCLE:
		case BTN_TRIANGLE:
		case BTN_SQUARE:
		{
			const float cx = x + r;
			Disc(cx, cy, r + 1.5f, COL_BTN_EDGE);
			Disc(cx, cy, r, COL_BTN);
			if (b == BTN_CROSS)
			{
				Bar(cx - 5, cy - 5, cx + 5, cy + 5, 2.5f, COL_CROSS);
				Bar(cx - 5, cy + 5, cx + 5, cy - 5, 2.5f, COL_CROSS);
			}
			else if (b == BTN_CIRCLE)
			{
				Arc(cx, cy, 4.2f, 6.6f, 0.0f, 2 * PI, 20, COL_CIRCLE);
			}
			else if (b == BTN_TRIANGLE)
			{
				Bar(cx, cy - 6, cx - 6, cy + 4.5f, 2.2f, COL_TRIANGLE);
				Bar(cx, cy - 6, cx + 6, cy + 4.5f, 2.2f, COL_TRIANGLE);
				Bar(cx - 6, cy + 4.5f, cx + 6, cy + 4.5f, 2.2f, COL_TRIANGLE);
			}
			else
			{
				Bar(cx - 5, cy - 5, cx + 5, cy - 5, 2.2f, COL_SQUARE);
				Bar(cx - 5, cy + 5, cx + 5, cy + 5, 2.2f, COL_SQUARE);
				Bar(cx - 5, cy - 6, cx - 5, cy + 6, 2.2f, COL_SQUARE);
				Bar(cx + 5, cy - 6, cx + 5, cy + 6, 2.2f, COL_SQUARE);
			}
			return 2 * r;
		}
		case BTN_START:
		{
			const float w = TextWidth(0.55f, "START") + 14.0f;
			RoundRect(x - 1, cy - 9, x + w + 1, cy + 9, 9.0f, COL_BTN_EDGE);
			RoundRect(x, cy - 8, x + w, cy + 8, 8.0f, COL_BTN);
			Text(x + 7, cy - g_font_cell_h * 0.55f * 0.5f, 0.55f, RGBA(0xc8, 0xcd, 0xd8, 0x80), "START");
			return w;
		}
		case BTN_DPAD:
		{
			const float cx = x + 11, s = 4.0f, l = 11.0f;
			Rect(cx - s - 1, cy - l - 1, cx + s + 1, cy + l + 1, COL_BTN_EDGE);
			Rect(cx - l - 1, cy - s - 1, cx + l + 1, cy + s + 1, COL_BTN_EDGE);
			Rect(cx - s, cy - l, cx + s, cy + l, COL_BTN);
			Rect(cx - l, cy - s, cx + l, cy + s, COL_BTN);
			gsKit_prim_triangle(gs, cx, cy - l + 2, cx - 2.5f, cy - l + 6, cx + 2.5f, cy - l + 6, 1, COL_DIM);
			gsKit_prim_triangle(gs, cx, cy + l - 2, cx - 2.5f, cy + l - 6, cx + 2.5f, cy + l - 6, 1, COL_DIM);
			gsKit_prim_triangle(gs, cx - l + 2, cy, cx - l + 6, cy - 2.5f, cx - l + 6, cy + 2.5f, 1, COL_DIM);
			gsKit_prim_triangle(gs, cx + l - 2, cy, cx + l - 6, cy - 2.5f, cx + l - 6, cy + 2.5f, 1, COL_DIM);
			return 22.0f;
		}
	}
	return 0.0f;
}

// A simple person silhouette (head + shoulders) for the listening position
static void Person(float cx, float cy, u64 c)
{
	Disc(cx, cy - 9, 6.5f, c);
	Arc(cx, cy + 12, 0.0f, 12.0f, PI, 2.0f * PI, 12, c);
	Rect(cx - 12, cy + 12, cx + 12, cy + 14, c);
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

// How much of a sound at azimuth `az` a PLII decoder sends to the speaker at `spk`: constant-power panning
// between the two adjacent speakers only (SL -100, FL -30, C 0, FR 30, SR 100, and SR -> SL across the back).
static float SpeakerGain(float az, float spk)
{
	static const float ring[] = {-100.0f, -30.0f, 0.0f, 30.0f, 100.0f, 260.0f};
	while (az < -100.0f)
		az += 360.0f;
	while (az >= 260.0f)
		az -= 360.0f;
	for (int i = 0; i < 5; i++)
	{
		if (az >= ring[i] && az <= ring[i + 1])
		{
			const float t = (az - ring[i]) / (ring[i + 1] - ring[i]);
			const float a = ring[i], b = (i == 4) ? -100.0f : ring[i + 1];
			float g = 0.0f;
			if (AzDiff(spk, a) < 0.5f)
				g += cosf(t * PI * 0.5f);
			if (AzDiff(spk, b) < 0.5f)
				g += sinf(t * PI * 0.5f);
			// below about -20 dB counts as off
			return g > 0.1f ? g : 0.0f;
		}
	}
	return 0.0f;
}

static void DrawRoom(float cx, float cy, float rad, const struct AudioState* a, int frame)
{
	Rect(cx - rad - 22, cy - rad - 22, cx + rad + 22, cy + rad + 22, COL_ROOM);
	Ring(cx, cy, rad, COL_EDGE);
	Ring(cx, cy, rad * 0.5f, COL_EDGE);

	const struct TestDef* t = a->test;
	const int active = a->playing && t;
	const int dts = active && t->kind == KIND_DTS;
	const int sounding = active && (dts || a->phase == PHASE_BURST || a->phase == PHASE_VOICE);
	const int spread = active && t->kind == KIND_AMBIENCE;
	const float az = active ? Audio_ThetaToAzimuth(t->kind == KIND_DTS ? t->theta : a->current_theta) : 0.0f;

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
				on = SpeakerGain(dts ? Audio_ThetaToAzimuth(t->theta) : az, s->az);
		}
		// Two-source test: the dialogue keeps the centre lit under the bursts
		if ((active && t->kind == KIND_DUAL && (a->phase == PHASE_BURST || a->phase == PHASE_PAUSE)) || (dts && t->clip == DTS_CLIP_DUAL))
			on = fmaxf(on, SpeakerGain(0.0f, s->az));
		if (on > 0.0f)
			Disc(sx, sy, 22.0f + 6.0f * on, COL_GLOW);
		Rect(sx - 13, sy - 13, sx + 13, sy + 13, on > 0.3f ? COL_SPK_ON : COL_SPK);
		Text(sx - TextWidth(0.6f, s->label) * 0.5f, sy - g_font_cell_h * 0.6f * 0.5f, 0.6f, COL_TEXT, s->label);
	}

	// Subwoofer, in the room's front-right corner
	const float swx = cx + rad + 8.0f, swy = cy - rad - 8.0f;
	const int sub_on = sounding && (t->kind == KIND_LFE || (dts && t->clip == DTS_CLIP_SUB));
	if (sub_on)
		Disc(swx, swy, 18.0f, COL_GLOW);
	Rect(swx - 11, swy - 11, swx + 11, swy + 11,
		sub_on ? COL_SPK_ON : COL_SPK);
	Text(swx - TextWidth(0.5f, "SW") * 0.5f, swy - g_font_cell_h * 0.5f * 0.5f, 0.5f, COL_TEXT, "SW");

	// Listener
	Person(cx, cy, COL_DIM);

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
	SifExecModuleBuffer(dtsplay_irx, size_dtsplay_irx, 0, NULL, &ret);
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
	gs->PrimAlphaEnable = GS_SETTING_ON;
	FontInit();

	// Audio
	audsrv_init();
	struct audsrv_fmt_t fmt = {AUDIO_RATE, 16, 2};
	audsrv_set_format(&fmt);
	audsrv_set_volume(MAX_VOLUME);

	// SPU2 voice samples for the interpolation tests
	static audsrv_adpcm_t sample_sweep, sample_noise;
	audsrv_adpcm_init();
	audsrv_load_adpcm(&sample_sweep, vag_sweep, vag_sweep_size);
	audsrv_load_adpcm(&sample_noise, vag_noise, vag_noise_size);
	int voice_playing = 0, voice_frames = 0;
	static short chunk[AUDIO_CHUNK * 2] __attribute__((aligned(64)));
	struct AudioState audio;
	Audio_Init(&audio, g_voices);
	Dts_Init();
	int dts_frames = 0;

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

		// Voice-sample tests: play the ADPCM sample on an SPU2 voice during the burst phase, repeat after a pause
		{
			const int is_voice_test = audio.playing && (audio.test->kind == KIND_VOICE_SWEEP || audio.test->kind == KIND_VOICE_NOISE);
			audsrv_adpcm_t* smp = (is_voice_test && audio.test->kind == KIND_VOICE_SWEEP) ? &sample_sweep : &sample_noise;
			if (!is_voice_test)
			{
				if (voice_playing)
				{
					audsrv_adpcm_set_volume_and_pan(TEST_VOICE_CH, 0, 0);
					voice_playing = 0;
				}
			}
			else if (audio.phase == PHASE_BURST)
			{
				if (!voice_playing)
				{
					audsrv_adpcm_set_volume_and_pan(TEST_VOICE_CH, MAX_VOLUME, 0);
					audsrv_ch_play_adpcm(TEST_VOICE_CH, smp);
					voice_playing = 1;
					voice_frames = 0;
				}
				else if (++voice_frames > 10 && !audsrv_is_adpcm_playing(TEST_VOICE_CH, smp))
				{
					voice_playing = 0;
					audio.bursts_done++;
					audio.phase = PHASE_PAUSE;
					audio.pos = 0;
				}
			}
		}

		// DTS tests: the IOP module loops the clip in S/PDIF bitstream mode while the test plays
		{
			const int want = (audio.playing && audio.test->kind == KIND_DTS) ? audio.test->clip : -1;
			Dts_Play(want);
			dts_frames = want >= 0 ? dts_frames + 1 : 0;
		}

		// Auto mode: three bursts (one lap for the sweep), then next test
		if (autoplay && audio.playing)
		{
			const enum TestKind k = audio.test->kind;
			const int done = (k == KIND_SWEEP) ? (audio.laps >= 1) :
							 (k == KIND_FAST_SWEEP) ? (audio.laps >= 3) :
							 (k == KIND_LEVELS) ? (audio.bursts_done >= 6) :
							 (k == KIND_VOICE_SWEEP || k == KIND_VOICE_NOISE) ? (audio.bursts_done >= 1) :
							 (k == KIND_DTS) ? (dts_frames >= 8 * 60) :
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
		// Blending state is (re)queued every frame: in one-shot mode the queue is rebuilt each frame, and the
		// clear changes it. Without this, translucent shapes drew solid and every glyph was an opaque box.
		gs->PrimAlphaEnable = GS_SETTING_ON;
		gsKit_set_test(gs, GS_ATEST_OFF);
		gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
		Rect(0, 0, W, H, COL_BG); // the actual clear (see Quad)

		Rect(0, 0, W, 44, COL_PANEL);
		Text(20, 10, 1.0f, COL_TEXT, "Pro Logic II Surround Test for PS2");

		const float rcx = 168, rcy = 186, rr = 100;
		DrawRoom(rcx, rcy, rr, &audio, frame);

		// Test list (scrolls with the selection)
		const float lx = 330, ly = 54, row = 18;
		const int visible = 19;
		int first = sel - visible / 2;
		if (first > NUM_TESTS - visible)
			first = NUM_TESTS - visible;
		if (first < 0)
			first = 0;
		const int shown = (NUM_TESTS < visible) ? NUM_TESTS : visible;
		Rect(lx - 10, ly - 6, W - 14, ly + shown * row + 4, COL_PANEL);
		for (int k = 0; k < shown; k++)
		{
			const int i = first + k;
			const float y = ly + k * row;
			if (i == sel)
				Rect(lx - 6, y - 1, W - 18, y + row - 1, COL_SEL);
			snprintf(line, sizeof(line), "%d", i + 1);
			Text(lx + 18 - TextWidth(0.62f, line), y, 0.62f, i == sel ? COL_ACCENT : COL_DIM, line);
			Text(lx + 28, y, 0.62f, i == sel ? COL_TEXT : COL_DIM, g_tests[i].name);
			if (i == sel && audio.playing)
				Disc(W - 30, y + row * 0.5f - 1, 4.0f, (frame / 15) & 1 ? COL_GREEN : COL_ACCENT);
		}
		// scroll hints
		if (first > 0)
			gsKit_prim_triangle(gs, W - 30, ly - 4, W - 35, ly + 2, W - 25, ly + 2, 1, COL_DIM);
		if (first + shown < NUM_TESTS)
			gsKit_prim_triangle(gs, W - 30, ly + shown * row + 2, W - 35, ly + shown * row - 4, W - 25, ly + shown * row - 4, 1, COL_DIM);

		// Details for the selected test
		const struct TestDef* t = &g_tests[sel];
		// Details sit under the room diagram (the list fills the right-hand side)
		const float dx = 24, dw = 300;
		float dy = rcy + rr + 32;
		Text(dx, dy, 0.55f, COL_ACCENT, "EXPECTED");
		if (audio.playing && audio.test == t && t->kind == KIND_LEVELS && audio.phase == PHASE_BURST)
		{
			static const char* const lv[3] = {"now 0 dB", "now -20 dB", "now -40 dB"};
			Text(dx + 110, dy, 0.55f, COL_GREEN, lv[audio.bursts_done % 3]);
		}
		dy += 15;
		dy += TextWrap(dx, dy, dw, 0.6f, COL_TEXT, t->expected) + 6;
		Text(dx, dy, 0.55f, COL_ACCENT, "ENCODED AS");
		dy += 15;
		if (t->encoding)
			snprintf(line, sizeof(line), "%s", t->encoding);
		else
			snprintf(line, sizeof(line), "Lt %+.3f    Rt %+.3f", cosf(t->theta * PI / 180.0f), sinf(t->theta * PI / 180.0f));
		TextWrap(dx, dy, dw, 0.6f, COL_TEXT, line);

		// Footer with PS2 button icons
		Rect(0, H - 38, W, H, COL_PANEL);
		{
			const float fy = H - 19, ts = 0.65f, ty = fy - g_font_cell_h * ts * 0.5f;
			float x = 20;
			x += ButtonIcon(x, fy, BTN_CROSS) + 6;
			Text(x, ty, ts, COL_TEXT, "Play");
			x += TextWidth(ts, "Play") + 18;
			x += ButtonIcon(x, fy, BTN_CIRCLE) + 6;
			Text(x, ty, ts, COL_TEXT, "Stop");
			x += TextWidth(ts, "Stop") + 18;
			x += ButtonIcon(x, fy, BTN_START) + 6;
			Text(x, ty, ts, COL_TEXT, "Auto-cycle");
			x += TextWidth(ts, "Auto-cycle") + 18;
			x += ButtonIcon(x, fy, BTN_DPAD) + 6;
			Text(x, ty, ts, COL_TEXT, "Select");
			if (autoplay)
				Text(W - 20 - TextWidth(ts, "AUTO"), ty, ts, COL_GREEN, "AUTO");
		}

		gsKit_queue_exec(gs);
		gsKit_sync_flip(gs);
		frame++;
	}
	return 0;
}
