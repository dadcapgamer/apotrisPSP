.DEFAULT_GOAL := all

TARGET = apotris_psp

GAME_CPP = audio_files blockEngine bot descriptions flashSaves game gbp_logo \
	interrupt liba_pc liba_psp psp_audio main menuSystem menus multiplayer \
	multiplayerClasses music prng saving scene screenAudio screenControls \
	screenCredits screenGraphics screenHandling screenLinks screenPlayOptions \
	screenSkinEditor screenColorEditor screenTitle sprites tetromino text
GAME_C = detectEmulators qrcodegen xoshiro128plusplus

# Tilengine's TMX/XML file loaders are compiled out for PSP (see the PSP
# guards that patches/tilengine-psp.patch adds to ObjectList.c): every Apotris
# asset is linked in from data/*.bin, so nothing is ever loaded from the
# filesystem.
TILENGINE_C = Animation Base64 Bitmap Blitters Draw Layer List Math2D Object \
	ObjectList Palette Sequence SequencePack Sprite Spriteset Tables Tilemap Tilengine \
	Tileset crc32

GAME_OBJS = $(addprefix source/,$(addsuffix .o,$(GAME_CPP))) \
	$(addprefix source/,$(addsuffix .o,$(GAME_C)))
TILENGINE_OBJS = $(addprefix subprojects/Tilengine/src/,$(addsuffix .o,$(TILENGINE_C)))
DATA_BINS = $(wildcard data/*.bin)
DATA_OBJS = $(patsubst data/%.bin,build/psp-data/%.o,$(DATA_BINS))
DATA_HEADERS = $(patsubst data/%.bin,build/psp-data/%_bin.h,$(DATA_BINS))

GEN_DIR = build/psp-data
SFX_OBJ = $(GEN_DIR)/psp_sfx.o
SFX_HEADER = $(GEN_DIR)/psp_sfx_table.h
MUSIC_OBJ = $(GEN_DIR)/psp_music.o
MUSIC_HEADER = $(GEN_DIR)/psp_music_table.h

OBJS = $(GAME_OBJS) $(TILENGINE_OBJS) $(DATA_OBJS) $(SFX_OBJ) $(MUSIC_OBJ)

PSPPREFIX := $(shell psp-config --psp-prefix)

INCDIR = include build/psp-data subprojects/Tilengine/include \
	$(PSPPREFIX)/include/SDL2
# -MMD -MP emits a .d beside each object so that editing a header actually
# rebuilds what includes it. Without this the tree silently keeps stale
# objects: nothing else here declares a dependency on include/*.hpp.
DEPFLAGS = -MMD -MP
CFLAGS = -O2 -G0 -Wall -Wextra -DPSP -DTE -DNDEBUG $(DEPFLAGS)
CXXFLAGS = $(CFLAGS) -std=gnu++17
ASFLAGS = $(CFLAGS)

LIBDIR = $(PSPPREFIX)/lib
LIBS = -lSDL2main -lSDL2 -lxmp-lite -lm -lstdc++ \
	-lGL -lpspvram -lpspaudio -lpspvfpu -lpspdisplay -lpspgu -lpspge \
	-lpsphprm -lpspctrl -lpsppower
# --gc-sections must not be used here: it collects the PSP import stub
# sections, which leaves every sceKernel* stub unpatched at load time.
LDFLAGS =

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Apotris PSP 4.1.0

# XMB art, exported from the "apotris" page of the Art 4 Ports Figma file.
# ICON0 is the 144x80 game-list thumbnail, PIC1 the 480x272 full-screen
# background shown once the title is highlighted. Both are opaque 8-bit RGB.
PSP_EBOOT_ICON = psp-xmb/ICON0.PNG
PSP_EBOOT_PIC1 = psp-xmb/PIC1.PNG

# build.mak's EBOOT.PBP rule lists the art only on the pack-pbp command line,
# never as a prerequisite, so without this an art-only change never repacks.
EBOOT.PBP: $(PSP_EBOOT_ICON) $(PSP_EBOOT_PIC1)

$(GAME_OBJS): $(DATA_HEADERS) $(SFX_HEADER) $(MUSIC_HEADER)

# Tilengine is an upstream submodule, so the PSP guards live in a patch rather
# than a fork. It is applied once into the checkout; the reverse check makes
# this a no-op on a tree that already carries it.
TILENGINE_PATCH = patches/tilengine-psp.patch
TILENGINE_STAMP = build/tilengine-patched.stamp

$(TILENGINE_STAMP): $(TILENGINE_PATCH)
	@mkdir -p build
	@if git -C subprojects/Tilengine apply --reverse --check ../../$(TILENGINE_PATCH) 2>/dev/null; then \
		echo "Tilengine PSP patch already applied"; \
	else \
		git -C subprojects/Tilengine apply ../../$(TILENGINE_PATCH) && echo "Applied Tilengine PSP patch"; \
	fi
	@touch $@

$(TILENGINE_OBJS): $(TILENGINE_STAMP)

# One blob for all 39 effects, downmixed to mono at a single rate. The script
# writes the .s, the .bin it includes and the offset table in one go; the
# header rule just waits on it (make 3.81 has no grouped targets).
$(GEN_DIR)/psp_sfx.s: tools/psp_build_sfx.py source/audio_files.cpp $(wildcard assets/*.wav)
	@mkdir -p $(GEN_DIR)
	@python3 tools/psp_build_sfx.py --outdir $(GEN_DIR)

$(SFX_HEADER): $(GEN_DIR)/psp_sfx.s
	@:

# The nine music modules are linked in whole and handed to libxmp from memory.
$(GEN_DIR)/psp_music.s: tools/psp_build_music.py $(wildcard assets/*.it assets/*.mod)
	@mkdir -p $(GEN_DIR)
	@python3 tools/psp_build_music.py --outdir $(GEN_DIR)

$(MUSIC_HEADER): $(GEN_DIR)/psp_music.s
	@:

build/psp-data/%.s: data/%.bin
	@mkdir -p build/psp-data
	@python3 tools/bin2s.py -a 4 -H build/psp-data/$*_bin.h -o $@ $<

build/psp-data/%_bin.h: build/psp-data/%.s
	@:

build/psp-data/%.o: build/psp-data/%.s
	$(CC) $(ASFLAGS) -c $< -o $@

clean-data:
	rm -rf build/psp-data

# A drag-and-drop folder for the memory stick: copy psp-package/APOTRIS to
# PSP/GAME/ on the stick. Everything is linked into the EBOOT, so the one
# file is the whole game.
PACKAGE_DIR = psp-package/APOTRIS

package: EBOOT.PBP
	@mkdir -p $(PACKAGE_DIR)
	@cp EBOOT.PBP $(PACKAGE_DIR)/
	@echo "Package ready: $(PACKAGE_DIR) -> copy into PSP/GAME/ on the stick"

.PHONY: package

-include $(OBJS:.o=.d)

PSPSDK := $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
