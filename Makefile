# KallistiOS build. Source your KOS environ.sh first, then use ./build.sh (it prepares smw/ and romdisk/).
TARGET = smw_dc.elf
SMW = smw

SMW_SRC = \
	src/common_cpu_infra src/common_rtl src/smw_00 src/smw_01 src/smw_02 src/smw_03 \
	src/smw_04 src/smw_05 src/smw_07 src/smw_0c src/smw_0d src/smw_cpu_infra \
	src/smw_rtl src/smw_spc_player src/util src/lm src/tracing \
	src/snes/apu src/snes/cart src/snes/cpu src/snes/dma src/snes/dsp src/snes/ppu \
	src/snes/ppu_old src/snes/snes src/snes/snes_other src/snes/spc

OBJS = $(addsuffix .o,$(addprefix $(SMW)/,$(SMW_SRC))) dc/dc_game.o dc/dc_platform.o romdisk.o

KOS_ROMDISK_DIR = romdisk
# Upstream is written for x86 compilers: keep old-style C warnings from becoming errors on modern GCC.
KOS_CFLAGS += -O3 -fomit-frame-pointer -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-strict-aliasing -include stddef.h \
	-Idc -Idc/compat -I$(SMW) -I$(SMW)/src -DSYSTEM_VOLUME_MIXER_AVAILABLE=0 \
	-Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration \
	-Wno-error=int-conversion -Wno-error=return-mismatch -w

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS) -lm

clean:
	-rm -f $(TARGET) $(OBJS) romdisk.o romdisk.img

run: $(TARGET)
	$(KOS_LOADER) $(TARGET)

dist: $(TARGET)
	-rm -f $(OBJS) romdisk.o romdisk.img
	$(KOS_STRIP) $(TARGET)
