# Ultima VII (Exult 1.12.1, the open-source engine for The Black Gate and
# Serpent Isle) for the m9b SoC: builds exult.bin, which the core firmware's
# loader copies into PSRAM and runs (see README.md). Requires
# riscv64-unknown-elf-gcc/g++, picolibc-riscv64-unknown-elf and
# libstdc++-riscv64-unknown-elf-picolibc (CXXROOT=DIR: an unpacked copy of
# that package instead, DIR/usr/...).
#
# `make host` builds tools/host's test harness instead: the same Exult and
# port code for 32-bit x86 Linux (gcc-multilib, g++-multilib).

CROSS   ?= riscv64-unknown-elf-
CC      := $(CROSS)gcc
CXX     := $(CROSS)g++
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

# The m9b core (m9b-core): its firmware (SD card, FatFs) and the runtime the
# programs share (system calls, trap handler, linker script).
CORE   ?= ../core
CORE_FW   := $(CORE)/firmware
CORE_RT   := $(CORE)/runtime
E := exult

# Memory map (PSRAM, $(CORE)/runtime/m9b.ld): the image (2.09 MB) from
# 0x800000, then data, bss and the heap (5.9 MB) up to a 128 KB stack at
# the top.
B ?= build
comma := ,

ifneq ($(CXXROOT),)
CXXINC := -nostdinc++ -isystem $(CXXROOT)/usr/include/newlib/c++/14.2.0 \
          -isystem $(CXXROOT)/usr/include/newlib/c++/14.2.0/riscv64-unknown-elf/rv32im/ilp32
CXXLIB := -L$(CXXROOT)/usr/lib/picolibc/riscv64-unknown-elf/lib/release/rv32im/ilp32
endif

# Exult (upstream's Makefile.common, less: the cheat screen, touch input,
# Ogg Vorbis, the MIDI synthesizers and drivers, the scalers but point and
# 2x, screenshots, zip files, the usecode debugger, Exult Studio).
MAIN := actions actorio actors args browser cheat combat dir drag effects exult exultmenu game \
        gameclk gamedat gamemap gamerend gamewin istring keys keyactions menulist monsters mouse \
        npcnear npctime palette party paths readnpcs schedule shapeid tqueue txtscroll verify version
EXULT_SRC := $(MAIN:%=%.cc) \
    $(addprefix pathfinder/, Astar.cc path.cc PathFinder.cc Zombie.cc) \
    $(addprefix conf/, Configuration.cc XMLEntity.cc) \
    $(addprefix audio/, Audio.cc conv.cc Midi.cc AudioChannel.cc AudioMixer.cc AudioSample.cc \
      RawAudioSample.cc VocAudioSample.cc WavAudioSample.cc) \
    $(addprefix audio/midi_drivers/, MidiDriver.cpp XMidiEventList.cpp XMidiFile.cpp XMidiSequence.cpp) \
    flic/playfli.cc \
    $(addprefix files/, crc.cc Flat.cc Flex.cc IFF.cc listfiles.cc msgfile.cc Table.cc U7file.cc \
      U7fileman.cc U7obj.cc utils.cc sha1/sha1.cpp sdlrwopsstreambuf.cc sdlrwopsistream.cc sdlrwopsostream.cc) \
    $(addprefix gamemgr/, bggame.cc devgame.cc modmgr.cc sigame.cc) \
    $(addprefix gumps/, Actor_gump.cc AudioOptions_gump.cc Book_gump.cc GameEngineOptions_gump.cc \
      CombatStats_gump.cc Enabled_button.cc Face_button.cc Face_stats.cc File_gump.cc Gamemenu_gump.cc \
      GameDisplayOptions_gump.cc Mixer_gump.cc Modal_gump.cc Gump.cc Gump_button.cc Gump_manager.cc \
      Gump_widget.cc Gump_ToggleButton.cc ShortcutBar_gump.cc ItemMenu_gump.cc Jawbone_gump.cc \
      misc_buttons.cc Newfile_gump.cc Notebook_gump.cc Paperdoll_gump.cc Scroll_gump.cc Sign_gump.cc \
      Slider_gump.cc Slider_widget.cc Spellbook_gump.cc Stats_gump.cc Text_button.cc Text_gump.cc \
      InputOptions_gump.cc VideoOptions_gump.cc Yesno_gump.cc StringList_widget.cc AdvancedOptions_gump.cc \
      ConfigSetting_widget.cc DropDown_widget.cc Scrollable_widget.cc) \
    $(addprefix imagewin/, ibuf8.cc imagebuf.cc iwin8.cc PointScaler.cpp) \
    $(addprefix shapes/, font.cc fontvga.cc items.cc miscinf.cc shapeinf.cc shapevga.cc u7drag.cc vgafile.cc) \
    $(addprefix shapes/shapeinf/, ammoinf.cc aniinf.cc armorinf.cc bodyinf.cc continf.cc effhpinf.cc \
      expinf.cc frnameinf.cc frflags.cc frusefun.cc lightinf.cc monstinf.cc npcdollinf.cc objdollinf.cc \
      sfxinf.cc warminf.cc weaponinf.cc) \
    $(addprefix objs/, animate.cc barge.cc chunks.cc chunkter.cc contain.cc egg.cc iregobjs.cc jawbone.cc \
      mappatch.cc objnames.cc objs.cc spellbook.cc virstone.cc) \
    $(addprefix usecode/, ucserial.cc conversation.cc intrinsics.cc keyring.cc stackframe.cc ucdisasm.cc \
      ucfunction.cc ucinternal.cc ucmachine.cc ucsched.cc ucsymtbl.cc useval.cc)

INCLUDES := -Iport -Iport/picolibc -Iport/SDL2 -I$(E) $(addprefix -I$(E)/,data headers imagewin shapes files gumps objs conf audio \
              audio/midi_drivers pathfinder usecode gamemgr flic server shapes/shapeinf tools) \
            -I$(CORE_FW) -I$(CORE_FW)/fatfs
DEFS     := -DM9B -DHAVE_CONFIG_H -DEXULT_DATADIR=\"EXULT\" -include port/m9b_compat.h

ARCH     := -march=rv32im_zba_zicbop_zicsr_zifencei_zicbom -mabi=ilp32
COMMON   := $(ARCH) -Os -g --picolibc-buildtype=release -ffunction-sections -fdata-sections $(INCLUDES)
CXXFLAGS := $(COMMON) $(DEFS) -specs=picolibcpp.specs -std=gnu++17 $(CXXINC) -Wall -Wno-unused -Wno-deprecated-declarations
CFLAGS   := $(COMMON) -specs=picolibc.specs -std=gnu11 -DFF_FS_READONLY=0 -Wall
LDFLAGS  := $(ARCH) -specs=picolibcpp.specs --picolibc-buildtype=release $(CXXLIB) \
            -Wl,--defsym=__stack_size=0x20000 -Wl,--gc-sections -Wl,-Map=$(B)/exult.map \
            $(addprefix -Wl$(comma)--wrap=,main open unlink rename mkdir) \
            -T$(CORE_RT)/m9b.ld
# (-T here, after the --defsym's: m9b.ld tests DEFINED(__stack_size) as it
# is read, and the specs file's -T would come before them: the stack would
# be 4 KB below a heap that grows up to it.)

PORT_CXX := $(wildcard port/*.cc)
PORT_C   := $(wildcard port/*.c)
C_SRC    := $(PORT_C) $(CORE_RT)/syscalls.c $(CORE_RT)/string.c $(CORE_RT)/trap.c \
            $(CORE_FW)/sd.c $(CORE_FW)/diskio.c $(CORE_FW)/fatfs/ff.c $(CORE_FW)/fatfs/ffunicode.c
CXX_SRC  := $(EXULT_SRC:%=$(E)/%) $(PORT_CXX)
ASM_SRC  := $(CORE_RT)/trap_entry.S

obj = $(B)/$(subst ../,_/,$(basename $(1))).o
OBJ := $(foreach f,$(C_SRC) $(CXX_SRC) $(ASM_SRC),$(call obj,$(f)))

all: $(B)/exult.bin

define c_rule
$(call obj,$(1)): $(1)
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) $$(EXTRA) -MMD -MP -c $$< -o $$@
endef
define cxx_rule
$(call obj,$(1)): $(1)
	@mkdir -p $$(@D)
	$$(CXX) $$(CXXFLAGS) $$(EXTRA) -MMD -MP -c $$< -o $$@
endef
$(foreach f,$(C_SRC) $(ASM_SRC),$(eval $(call c_rule,$(f))))
$(foreach f,$(CXX_SRC),$(eval $(call cxx_rule,$(f))))

$(call obj,$(CORE_RT)/string.c): CFLAGS += -fno-tree-loop-distribute-patterns -fno-builtin
$(call obj,$(CORE_RT)/syscalls.c): CFLAGS += -DMAX_FILES=40

$(B)/exult.elf: $(OBJ) $(CORE_RT)/m9b.ld Makefile
	$(CXX) $(LDFLAGS) $(LDEXTRA) $(OBJ) -lm -o $@
	$(SIZE) $@

$(B)/exult.bin: $(B)/exult.elf
	$(OBJCOPY) -O binary $< $@
	@ls -l $@

# -------------------------------------------------------------------------
# The host test harness (tools/host/host.cc): the same Exult and port code
# for 32-bit x86 Linux (gcc-multilib, g++-multilib), so pointers and
# structures are as on the board. libstdc++ is linked in so that operator
# new's allocations go through the harness's malloc.
# -------------------------------------------------------------------------
HB := build-host
HOST_SRC := $(EXULT_SRC:%=$(E)/%) $(PORT_CXX) tools/host/host.cc
hobj = $(HB)/$(subst ../,_/,$(basename $(1))).o
HOST_OBJ := $(foreach f,$(HOST_SRC),$(call hobj,$(f)))
HOST_CXXFLAGS := -m32 -O1 -g -ffunction-sections -fdata-sections -Iport -Iport/SDL2 $(filter-out -Iport -Iport/picolibc -Iport/SDL2,$(INCLUDES)) \
                 $(DEFS) -std=gnu++17 -Wall -Wno-unused -Wno-deprecated-declarations
HOST_WRAP := main malloc calloc realloc aligned_alloc memalign posix_memalign free

define host_rule
$(call hobj,$(1)): $(1)
	@mkdir -p $$(@D)
	g++ $$(HOST_CXXFLAGS) $$(EXTRA) -MMD -MP -c $$< -o $$@
endef
$(foreach f,$(HOST_SRC),$(eval $(call host_rule,$(f))))
HOST_OBJ += $(HB)/tools/host/host_malloc.o
$(HB)/tools/host/host_malloc.o: tools/host/host_malloc.c port/dlmalloc/malloc.c
	@mkdir -p $(@D)
	gcc -m32 -O2 -g -c $< -o $@

host: $(HB)/exult-host

$(HB)/exult-host: $(HOST_OBJ)
	g++ -m32 -no-pie -static-libstdc++ -static-libgcc -Wl,--gc-sections $(HOST_OBJ) $(addprefix -Wl$(comma)--wrap=,$(HOST_WRAP)) \
	    -lpthread -lm -o $@

-include $(OBJ:.o=.d) $(HOST_OBJ:.o=.d)

clean:
	rm -rf build build-*

.PHONY: all host clean
