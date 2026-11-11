# PS2 Dolby Pro Logic II surround test
EE_BIN = surround-test.elf
EE_OBJS = src/main.o src/audio.o src/voices.o src/font.o src/vags.o src/dts.o src/dts_clips.o dtsplay_irx.o libsd_irx.o audsrv_irx.o sio2man_irx.o padman_irx.o
EE_INCS += -I$(GSKIT)/include -Isrc
EE_LDFLAGS += -L$(GSKIT)/lib
EE_LIBS = -lgskit -ldmakit -laudsrv -lpad -lpatches -lm
EE_CFLAGS += -O2 -Wall

GSKIT ?= $(PS2DEV)/gsKit
IRX = $(PS2SDK)/iop/irx

all: $(EE_BIN)

src/voices.c: tools/gen_voices.py
	python3 tools/gen_voices.py $@

src/vags.c: tools/gen_vag.py
	python3 tools/gen_vag.py $@

FONT_TTF ?= /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf
src/font.c: tools/gen_font.py
	python3 tools/gen_font.py $@ $(FONT_TTF) 20

src/dts_clips.c: tools/gen_dts.py
	python3 tools/gen_dts.py $@

irx/dtsplay/dtsplay.irx: irx/dtsplay/dtsplay.c irx/dtsplay/imports.lst
	$(MAKE) -C irx/dtsplay

dtsplay_irx.c: irx/dtsplay/dtsplay.irx
	$(PS2SDK)/bin/bin2c $< $@ dtsplay_irx

%_irx.c: $(IRX)/%.irx
	$(PS2SDK)/bin/bin2c $< $@ $(notdir $(basename $<))_irx

clean:
	rm -f $(EE_BIN) $(EE_OBJS) src/voices.c src/font.c src/vags.c src/dts_clips.c *_irx.c
	$(MAKE) -C irx/dtsplay clean

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
