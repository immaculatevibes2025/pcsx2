// PS2 surround test - EE side of the DTS player (talks to irx/dtsplay over SIF RPC)
#include "dts.h"

#include <kernel.h>
#include <sifrpc.h>
#include <string.h>

#define DTSPLAY_RPC_ID 0x5D750001
enum
{
	CMD_ALLOC = 1,
	CMD_PLAY = 2,
	CMD_STOP = 3,
};

#define CLIP_FRAMES (512 * 375)
#define CLIP_BYTES (CLIP_FRAMES * 4)

static SifRpcClientData_t s_client;
static unsigned int s_rpc[16] __attribute__((aligned(64)));
static unsigned int s_iop_buf;
static int s_ok;
static int s_current = -1;

// Block-transfer layout: for every 256 frames, 512 bytes of left words then 512 bytes of right words
static short s_blocks[CLIP_FRAMES * 2] __attribute__((aligned(64)));

static unsigned int Call(int cmd, unsigned int arg)
{
	s_rpc[0] = arg;
	SifCallRpc(&s_client, cmd, 0, s_rpc, sizeof(s_rpc), s_rpc, sizeof(s_rpc), NULL, NULL);
	return s_rpc[0];
}

int Dts_Init(void)
{
	for (int tries = 0; tries < 100; tries++)
	{
		if (SifBindRpc(&s_client, DTSPLAY_RPC_ID, 0) < 0)
			return -1;
		if (s_client.server)
			break;
		for (volatile int i = 0; i < 100000; i++)
			;
	}
	if (!s_client.server)
		return -1;
	s_iop_buf = Call(CMD_ALLOC, CLIP_BYTES);
	s_ok = (s_iop_buf != 0);
	return s_ok ? 0 : -1;
}

int Dts_Available(void)
{
	return s_ok;
}

void Dts_Play(int index)
{
	if (!s_ok || index == s_current)
		return;
	Call(CMD_STOP, 0);
	s_current = -1;
	if (index < 0 || index >= g_num_dts_clips)
		return;

	const short* w = g_dts_clips[index].words;
	for (unsigned int b = 0; b < CLIP_FRAMES / 256; b++)
	{
		short* dst = &s_blocks[b * 512];
		const short* src = &w[b * 512];
		for (int i = 0; i < 256; i++)
		{
			dst[i] = src[i * 2 + 0];
			dst[256 + i] = src[i * 2 + 1];
		}
	}

	// Copy to IOP memory in 64 KB DMA pieces
	FlushCache(0);
	for (unsigned int off = 0; off < CLIP_BYTES; off += 0x10000)
	{
		SifDmaTransfer_t t;
		t.src = (unsigned char*)s_blocks + off;
		t.dest = (void*)(s_iop_buf + off);
		t.size = (CLIP_BYTES - off) < 0x10000 ? (CLIP_BYTES - off) : 0x10000;
		t.attr = 0;
		const unsigned int id = SifSetDma(&t, 1);
		while (SifDmaStat(id) >= 0)
			;
	}

	Call(CMD_PLAY, CLIP_BYTES);
	s_current = index;
}
