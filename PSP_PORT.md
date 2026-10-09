# Apotris PSP

Native PSP adaptation of Apotris 4.1.0. This port reuses the upstream game's
software-rendered 240x180 backend and presents its 240x160 playfield at 408x272
with pillarboxing.

## Build

Requires the [PSPDEV](https://pspdev.github.io/) toolchain (with SDL2 and
libxmp-lite) and Python 3. Only the Tilengine submodule is needed:

```sh
git clone https://github.com/dadcapgamer/apotrisPSP.git
cd apotrisPSP
git submodule update --init subprojects/Tilengine
export PSPDEV=~/pspdev
export PATH="$PSPDEV/bin:$PATH"
make -j4
```

### Tilengine patch

Tilengine stays an unmodified upstream submodule. The PSP changes to it live
in `patches/tilengine-psp.patch`, which the Makefile applies to the checkout
before compiling Tilengine, and skips if the checkout already carries it.

### Link flags

Do not add `--gc-sections` to `LDFLAGS`. It collects the PSP import stub
sections, which leaves every `sceKernel*` stub unpatched; the crt0's first
`sceKernelCreateThread` then jumps into unrelocated data and the title dies
before `main`. Tilengine's TMX/XML loaders were previously being dropped by
that same flag, so they are now compiled out explicitly with `#ifndef PSP`
guards in `subprojects/Tilengine/src/ObjectList.c` -- every Apotris asset is
linked in from `data/*.bin` and nothing is loaded from the filesystem.

## Install

```sh
make package
```

That writes `psp-package/APOTRIS/`, a folder to drag straight into `PSP/GAME/`
on the Memory Stick. Everything -- graphics and sound -- is linked into the
executable, so the single `EBOOT.PBP` is the whole game; there is no companion
assets directory to keep beside it.

Save data is written to `PSP/SAVEDATA/APOTRIS/Apotris.sav` on first save.

## XMB art

`psp-xmb/ICON0.PNG` (144x80) is the thumbnail in the game list and
`psp-xmb/PIC1.PNG` (480x272) is the full-screen background the XMB fades in
once the title is highlighted. Both are exported from the `apotris` page of the
Art 4 Ports Figma file and stored as opaque 8-bit RGB -- the source frames have
no transparency, and dropping the alpha channel saves about a quarter of the
bytes.

The Makefile points `PSP_EBOOT_ICON`/`PSP_EBOOT_PIC1` at them and also names
them as prerequisites of `EBOOT.PBP`. That second part is not redundant:
pspsdk's `build.mak` passes the art to `pack-pbp` on the command line but never
declares it as a dependency, so replacing the art without the extra rule leaves
the old PBP in place.

## Controls

- D-pad: move/menu navigation
- Cross: confirm / rotate right
- Circle: cancel / rotate left
- L and R: additional gameplay actions (all controls are rebindable)
- Start: pause
- Select: secondary menu action

## Audio

Sound effects and music are both implemented.

`tools/psp_build_sfx.py` packs all 39 effects into one blob at build time,
downmixed to mono and resampled to a single rate (22050 Hz). Storing at half
the output rate halves the resident size and costs nothing, because
`sfxRate()` needs arbitrary playback rates anyway. The blob is linked in via
the same `build/psp-data` pipeline as the graphics data, so the EBOOT stays
self-contained. It adds about 1.6 MB.

`source/psp_audio.cpp` mixes up to eight voices into the single 44100 Hz
stereo output that `sceAudio` provides, interpolating each voice from the
stored rate. Voices are allocated free-first, then by stealing the one that
started longest ago. Output was verified against the emulator's audio dump:
both `sfx()` and `sfxRate()` correlate at 1.0000 with the expected signal.

Music is played by libxmp-lite, which is in the PSPDEV toolchain and whose
four loaders (MOD/S3M/XM/IT) cover exactly the formats Apotris ships; the full
libxmp would add roughly half a megabyte more for loaders nothing uses.
`tools/psp_build_music.py` links the nine modules in whole and numbers them the
way the desktop backend does, so the song ids in save data stay comparable.
libxmp decodes on its own thread into a ring buffer of about a third of a
second, so a slow pattern change cannot starve the audio device.

`setMusicTempo()` maps to `xmp_set_tempo_factor()`, matching the GBA original's
`mmSetModuleTempo()`: musical tempo changes, pitch does not. The desktop
SoLoud backend instead resamples, which also drops pitch -- if the Zone effect
should sound like the desktop build rather than the GBA one, that is the knob.

Audio was verified against the emulator's dump: `sfx()` and `sfxRate()`
correlate at 1.0000 with the expected signal, and music ran 35 seconds
continuously with no ring-buffer underruns.
