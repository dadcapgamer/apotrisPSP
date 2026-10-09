# Apotris PSP

Native PSP port of [Apotris](https://apotris.com) 4.1.0 by Akouzoukos and
contributors, the block-stacking game originally made for the Game Boy Advance.
Tested on real PSP hardware: gameplay, sound, music, saving and exit to XMB.

## Install

Download the zip from [Releases](../../releases) and copy the `APOTRIS` folder
into `PSP/GAME/` on the Memory Stick. Everything is linked into the
`EBOOT.PBP`; there are no other files to copy.

Save data is written to `PSP/SAVEDATA/APOTRIS/`.

## Controls

- D-pad: move / menu navigation
- Cross: confirm / rotate right
- Circle: cancel / rotate left
- L and R: additional gameplay actions (all controls are rebindable in-game)
- Start: pause
- Select: secondary menu action

## Build

Requires the [PSPDEV](https://pspdev.github.io/) toolchain (with SDL2 and
libxmp-lite) and Python 3.

```sh
git clone https://github.com/dadcapgamer/apotrisPSP.git
cd apotrisPSP
git submodule update --init
export PSPDEV=~/pspdev
export PATH="$PSPDEV/bin:$PATH"
make -j4
make package
```

`make package` writes `psp-package/APOTRIS/`, ready to copy to the PSP.

## Port notes

- The game renders its 240x160 playfield in software and presents it at
  408x272 with pillarboxing.
- Sound effects are mixed into `sceAudio` by `source/psp_audio.cpp`; music is
  played by libxmp-lite. Both are packed into the EBOOT at build time by the
  scripts in `tools/`.
- Tilengine is an unmodified upstream submodule. The PSP changes to it live in
  `patches/tilengine-psp.patch`, which the Makefile applies automatically.
- Do not add `--gc-sections` to `LDFLAGS`: it strips the PSP import stubs and
  the game crashes before `main`.

This repository contains only what the PSP build needs. For the full game and
its other platforms, see the [upstream project](https://gitea.com/akouzoukos/apotris).

## License

AGPL-3.0, the same as upstream Apotris. See `LICENSE`.
