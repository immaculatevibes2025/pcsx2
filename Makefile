# PS2 Dolby Pro Logic II surround test
EE_BIN = surround-test.elf
EE_OBJS = src/main.o src/audio.o src/voices.o src/font.o libsd_irx.o audsrv_irx.o sio2man_irx.o padman_irx.o
EE_INCS += -I$(GSKIT)/include -Isrc
EE_LDFLAGS += -L$(GSKIT)/lib
EE_LIBS = -lgskit -ldmakit -laudsrv -lpad -lpatches -lm
EE_CFLAGS += -O2 -Wall

GSKIT ?= $(PS2DEV)/gsKit
IRX = $(PS2SDK)/iop/irx

all: $(EE_BIN)

src/voices.c: tools/gen_voices.py
	python3 tools/gen_voices.py $@

FONT_TTF ?= /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf
src/font.c: tools/gen_font.py
	python3 tools/gen_font.py $@ $(FONT_TTF) 20

%_irx.c: $(IRX)/%.irx
	$(PS2SDK)/bin/bin2c $< $@ $(notdir $(basename $<))_irx

clean:
	rm -f $(EE_BIN) $(EE_OBJS) src/voices.c src/font.c *_irx.c

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
