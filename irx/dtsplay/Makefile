IOP_BIN = dtsplay.irx
IOP_OBJS = dtsplay.o imports.o
IOP_CFLAGS += -Wall

all: $(IOP_BIN)

clean:
	rm -f $(IOP_BIN) $(IOP_OBJS)

include $(PS2SDK)/Defs.make
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
