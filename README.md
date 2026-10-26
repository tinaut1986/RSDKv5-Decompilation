# Sonic Mania 3DS

Nintendo 3DS port of *Sonic Mania*, built on the RSDKv5 decompilation, aimed at running well on the **Old 3DS and
2DS** as well as the New 3DS: the game is drawn by the console's GPU instead of the CPU.

It installs as its own application, **Sonic Mania 3DS**, and keeps its data and saves in
`sdmc:/3ds/Sonic Mania 3DS/`. Downloads are on this repository's
[releases](https://github.com/tinaut1986/RSDKv5-Decompilation/releases). The project is in beta.

No game data is distributed with this project. You need your own copy of Sonic Mania: its `Data.rsdk` file.

**Do not use this project to pirate Sonic Mania or any other Retro Engine game.** The Retro Engine and Sonic Mania
belong to their owners (the engine to [Evening Star](https://eveningstar.studio/), the game to SEGA); please support
their work.

## Origin

* [RSDKModding/RSDKv5-Decompilation](https://github.com/RSDKModding/RSDKv5-Decompilation): the decompilation of the
  Retro Engine v5 / v5U this repository is a fork of.
* [RSDKModding/Sonic-Mania-Decompilation](https://github.com/RSDKModding/Sonic-Mania-Decompilation): the game's code.
* [SaturnSH2x2/RSDKv5-Decompilation](https://github.com/SaturnSH2x2/RSDKv5-Decompilation/tree/3ds-main): the New 3DS
  port this one started from. It has since diverged: its own application, a GPU renderer, bottom screen and tools.

## Features

* **GPU renderer** (citro3d): tile layers, sprites, the Special Stages' 3D scenes, rotozoom layers and the masked and
  tinted inks are drawn by the GPU; the original software renderer stays as reference and fallback (DEBUG >
  RENDERER in debug builds).
* **Stereoscopic 3D**: the layers are set in depth by their parallax, with the HUD in front; the Special Stages' models
  too. Lower the 3D slider for the flat picture.
* **Frame pacing** for the Old 3DS: AUTO skips a late picture, 30 FPS draws every other one, OFF draws them all (the
  game slows down instead).
* **Bottom screen**:
  * GAME: the stage, the mode, the characters; time, score, rings and lives with the game's own HUD art (the time at
    the game's rate); the save's chaos emeralds; a row with the item boxes picked up (a shield or a power stays as long
    as it lasts).
  * OPTIONS: frames (AUTO, 30 FPS, OFF), an FPS counter on the top screen, the game's HUD hidden while the GAME tab
    shows it, Sonic Mania Plus, and colour themes (MANIA, which mixes the game's four colours on filled,
    cut-corner panels, MANIA OUTLINE, and five more).
  * ACHIEVEMENTS: the PC game's 18 achievements, kept on the SD card: a list with each one's description and unlock
    date; the unlock notice on the top screen (the game's own) or the bottom one, with a sound.
  * UPDATES: in-app updates with a STABLE and a + BETAS channel.
  * A loading box with the step, the file and a bar while the game loads.
* The bottom screen's text follows the language chosen in the game (English, French, Italian, German, Spanish,
  Japanese).
* Debug builds (the beta releases) add a DEBUG tab: frame timings, renderer statistics, switches and an FTP server
  with a remote control (scene jumps, screenshots, recorded runs).

## Installation

1. Install the CIA from the latest release with FBI (or scan the QR code in the release). There is no 3DSX release:
   from the Homebrew Launcher the game does not get enough memory.
2. Copy `Data.rsdk` from your copy of Sonic Mania (on PC, the game's install folder) to:

```text
sdmc:/3ds/Sonic Mania 3DS/
```

If that folder has no `Data.rsdk`, the game reads the one of the original 3DS port's folder (`sdmc:/3ds/SonicMania/`)
in place, and at the first start it offers once to copy that folder's saves. Both applications can be installed side
by side.

The sound needs your console's DSP firmware (`sdmc:/3ds/dspfirm.cdc`, the file other homebrew uses too). If it is
missing or damaged the game says so and offers to extract it from the console itself (EXTRACT); without it the game
runs, silent. It is Nintendo's, so it is never part of a download.

**Sonic Mania Plus** (Mighty, Ray, Encore mode) is off by default, as the decompilation's license asks of every build
given to anyone. When the game data has the Plus content, the first start asks whether to turn it on; do so only if
you own Sonic Mania Plus. It can be changed later in OPTIONS > PLUS (from the next start).

## Updates

Open **UPDATES**, pick the **STABLE** or **+ BETAS** channel and install. The notes show every release between the
installed version and the one offered. Save your game first: the application restarts after an update. The CIA is
checked against a SHA-256 digest before it replaces the installed one; Wi-Fi is required.

## Building

Requirements: devkitPro with devkitARM, libctru, citro2d, citro3d and the 3DS portlibs the updater needs (curl,
mbedTLS, jansson, zlib), plus `3ds-libtheora` (the build script can use an unpacked copy in `.deps/`).

```sh
./build_3ds.sh                      # bin/3DS/CTR/RSDKv5U.3dsx (with the dev tools)
./build_3ds.sh --cia                # also 3ds/out/SonicMania.cia
DEV_TOOLS=0 ./build_3ds.sh --cia    # without the dev tools
./build_3ds.sh --install <3ds-ip>   # build, upload and install over the running game (dev builds)
```

A tag `vX.Y.Z` builds and publishes a release (`.github/workflows/build-release.yml`). Contributor and workflow notes
are in [CLAUDE.md](CLAUDE.md) and [docs/PLAN.md](docs/PLAN.md), the renderer's in
[docs/gpu-renderer.md](docs/gpu-renderer.md), release notes in [docs/release-notes](docs/release-notes).

## Credits

* [Evening Star](https://eveningstar.studio/): the Retro Engine (RSDK).
* Rubberduckycooly and chuliRMG ([RSDKModding](https://github.com/RSDKModding)): the decompilation of RSDKv5(U) and of
  Sonic Mania.
* [SaturnSH2x2](https://github.com/SaturnSH2x2): the original Nintendo 3DS port.
* [tinaut1986](https://github.com/tinaut1986): this port (GPU renderer, Old 3DS work, bottom screen, updater).

## License

The code is covered by the **RSDKv5(U) Decompilation Source Code License**: see [LICENSE.md](LICENSE.md). In short:
no commercial use, the original developers and the decompilation's authors must be credited, no game assets may be
distributed, and every fork follows the same license.
