// dtsplay: IOP helper for the surround test.
// Puts the SPU2 S/PDIF output into bitstream (bypass) mode and loops an IEC 61937 DTS stream through core 0's
// input with block-transfer DMA - the same path PS2 games use for in-game DTS and Dolby Digital FMVs.
// The EE side asks for a buffer, DMAs the stream into it, then starts/stops playback over SIF RPC.

#include <irx.h>
#include <loadcore.h>
#include <sysmem.h>
#include <thbase.h>
#include <sifcmd.h>
#include <libsd.h>

IRX_ID("dtsplay", 1, 0);

#define DTSPLAY_RPC_ID 0x5D750001
enum
{
	CMD_ALLOC = 1, // arg: size in bytes -> IOP address (0 on failure)
	CMD_PLAY = 2, // arg: size in bytes of the data at that address -> 0
	CMD_STOP = 3,
};

// SPU2 S/PDIF registers
#define SPDIF_OUT ((volatile u16*)0xBF9007C0)
#define SPDIF_MODE ((volatile u16*)0xBF9007C6)
#define SPDIF_MEDIA ((volatile u16*)0xBF9007C8)
#define SPDIF_OUT_BYPASS 0x0100
#define SPDIF_MEDIA_DVD 0x0200

static SifRpcDataQueue_t s_queue;
static SifRpcServerData_t s_server;
static u32 s_rpc_buffer[16];

static void* s_buf = NULL;
static int s_playing = 0;
static u16 s_saved_out, s_saved_mode, s_saved_media;

static void stop(void)
{
	if (!s_playing)
		return;
	sceSdBlockTrans(SD_CORE_0, SD_TRANS_STOP, NULL, 0);
	*SPDIF_OUT = s_saved_out;
	*SPDIF_MODE = s_saved_mode;
	*SPDIF_MEDIA = s_saved_media;
	s_playing = 0;
}

static void* rpc_handler(int cmd, void* data, int size)
{
	u32* arg = (u32*)data;
	(void)size;
	switch (cmd)
	{
		case CMD_ALLOC:
			stop();
			if (s_buf)
				FreeSysMemory(s_buf);
			s_buf = AllocSysMemory(ALLOC_FIRST, arg[0], NULL);
			arg[0] = (u32)s_buf;
			break;

		case CMD_PLAY:
			stop();
			if (!s_buf)
			{
				arg[0] = (u32)-1;
				break;
			}
			s_saved_out = *SPDIF_OUT;
			s_saved_mode = *SPDIF_MODE;
			s_saved_media = *SPDIF_MEDIA;
			// Bypass the SPU2 mixer: core 0's input goes out of the optical port untouched (bitstream data)
			*SPDIF_MEDIA = SPDIF_MEDIA_DVD;
			*SPDIF_MODE = 0x0000;
			*SPDIF_OUT = SPDIF_OUT_BYPASS;
			// Loop the whole stream. Block layout: 512 bytes left, 512 bytes right, repeated.
			sceSdBlockTrans(SD_CORE_0, SD_TRANS_LOOP, (u8*)s_buf, arg[0]);
			s_playing = 1;
			arg[0] = 0;
			break;

		case CMD_STOP:
			stop();
			arg[0] = 0;
			break;
	}
	return data;
}

static void rpc_thread(void* unused)
{
	(void)unused;
	SifInitRpc(0);
	SifSetRpcQueue(&s_queue, GetThreadId());
	SifRegisterRpc(&s_server, DTSPLAY_RPC_ID, rpc_handler, s_rpc_buffer, NULL, NULL, &s_queue);
	SifRpcLoop(&s_queue);
}

int _start(int argc, char* argv[])
{
	iop_thread_t t;
	int id;
	(void)argc;
	(void)argv;

	t.attr = TH_C;
	t.option = 0;
	t.thread = rpc_thread;
	t.stacksize = 0x800;
	t.priority = 0x40;
	id = CreateThread(&t);
	if (id <= 0)
		return MODULE_NO_RESIDENT_END;
	StartThread(id, NULL);
	return MODULE_RESIDENT_END;
}
