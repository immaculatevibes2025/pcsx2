// PS2 surround test - DTS bitstream clips and the IOP player (irx/dtsplay)
#pragma once

struct DtsClip
{
	const short* words; // interleaved left/right S/PDIF words, 48 kHz
	unsigned int frames; // 4 s, a whole number of DTS frames, loops seamlessly
};

extern const struct DtsClip g_dts_clips[];
extern const int g_num_dts_clips;

// Returns 0 when the IOP module is up and its buffer is allocated.
int Dts_Init(void);
// Uploads clip `index` to the IOP and starts looping it in S/PDIF bitstream mode. -1 stops playback.
void Dts_Play(int index);
int Dts_Available(void);
