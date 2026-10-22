# Project notes for Claude

Native Nintendo 3DS port of **Sonic Mania**. This is the owner's fork
(`tinaut1986/RSDKv5-Decompilation`) of SaturnSH2x2's New 3DS port, which is built on
the RSDKv5 decompilation (RSDKModding / Rubberduckycooly). Goal: make it run well on
the **Old 3DS** by moving the renderer from CPU to GPU (citro3d).

The owner already shipped ports of the same kind: Zelda Minish Cap
(`../zelda-tmc-3ds`), Metroid Zero Mission (`../mzm`), Super Metroid (`../sm-3ds`).
They are the reference for workflow, release process, debug tools, GPU renderers
with palette handling, bottom-screen UI and Old 3DS pacing.

**Start every session by reading [`docs/PLAN.md`](docs/PLAN.md).**

## Language

- Chat with the owner in **Spanish (castellano)**.
- Code, comments, identifiers, commit messages and docs: **English**.
- Inherited engine code keeps its own style; do not churn it.

## Layout

| Path | What |
|---|---|
| `RSDKv5/` | The engine (renderer, audio, input, storage). Where the port work happens. |
| `RSDKv5/RSDK/Graphics/Backend/` | `RenderBackend` interface + `SoftwareBackend` (from the `gfx-rewrite` branch). The GPU backend plugs in here. |
| `RSDKv5/RSDK/Graphics/CTR/` | 3DS render device (presentation, frame pacing). |
| `dependencies/Sonic-Mania-Decompilation` | Game logic (submodule). `Game` is a symlink to its `SonicMania/` folder. |
| `3ds/` | RSF, banner, icon, `makerom`/`bannertool`, `build_cia.sh`. |

Game data is **never committed**: `Data.rsdk` from the owner's Steam copy
(Flatpak Steam: `~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Sonic Mania/`)
goes to `sdmc:/3ds/Sonic Mania 3DS/` on the console (`UserStorage.cpp`).

## Build

```sh
./build_3ds.sh            # RSDKv5U.elf / .3dsx in bin/3DS/CTR/
./build_3ds.sh --cia      # also 3ds/out/SonicMania.cia
```

- Needs devkitPro. `libtheora` for 3DS is not in the owner's portlibs and installing it
  needs sudo, so it is unpacked into `.deps/` (gitignored) from
  `pkg.devkitpro.org/packages/3ds-libtheora-1.2.0alpha1-2-any.pkg.tar.xz` (needs a
  pacman user agent). The script picks it up by itself.
- After switching branches run `rm -rf bin` first: make does not notice header changes
  across branch switches and links stale objects.
- The Makefile does not add `-DRETRO_USE_MOD_LOADER=0` or the `Game/` include paths;
  `build_3ds.sh` does.
- Title ID `0x5A230`, name "Sonic Mania 3DS", data folder `sdmc:/3ds/Sonic Mania 3DS/`.
  The SaturnSH2x2 build (`0x3082`, `/3ds/SonicMania/`) can stay installed next to it.

## Git and GitHub

- Everything goes to `origin` = `tinaut1986/RSDKv5-Decompilation`. `upstream` =
  SaturnSH2x2, push URL set to `DISABLED`. Never push or open PRs upstream.
- Same workflow as the owner's other ports:
  - `main` = stable; only `--no-ff` merges of a finished release branch.
  - `release/vX.Y.Z` = working line. Topic branches (`feat/`, `fix/`, `perf/`, `chore/`,
    `docs/`) are cut from the release branch, one topic each, merged back with
    `--no-ff` once the owner has checked them on the console.
  - Beta = tag on the release branch; stable = tag on `main` (push `main` before the tag).
    After every tag rename the release branch to the next patch.
  - Never commit directly on `main` or `release/*`. Commit only when the owner asks.
- `master` is the fork's GitHub default branch (SaturnSH2x2's history); we do not use it.
- Releases/CI/version script are not set up yet: see PLAN (B0.3). `main`, `release/v0.1.0` and the open feature branches are on `origin` (pushed 2026-10-22).
- Commit only when the owner asks; the owner asked for commits at the end of each long task.

## The console in the loop (dev-tools builds)

The game runs an FTP server on port 5000 (address on the bottom screen) with a remote control. Everything is
`curl`-based; see `RSDKv5/RSDK/Dev/CTR/DevTools.cpp` (commands) and `docs/gpu-renderer.md`.

```sh
./build_3ds.sh --install 192.168.1.141      # build, upload, SITE INSTALL over the running game, relaunch (10 s)
tools/site.sh STATE                         # category/scene, fps, stage times, GPU stats
tools/site.sh SCENES | "SCENE 1 0"          # list / jump (category, index): 1 0 = Green Hill 1, 3 0 = Special Stage 1
tools/site.sh "INPUT 11"                    # hold keys (hex 3DS mask: 10 = right, 1 = A); "INPUT 0" releases
tools/site.sh "FREEZE 1"                   # stop the objects, keep drawing (compare GPU and CPU on one picture); "FREEZE 0"
tools/site.sh "GPU 1"                       # renderer mode (the RENDERER button): 1 GPU (gpu+record+gpurender), 0 CPU (all software)
tools/site.sh "SET record 1"                # single switches: frameskip maxskip draw flipcopy gpu gpurender record syncdraw vblank bottomui
tools/site.sh ORDER                         # draw order of the last frame (L layer, S sprite, F face, R roto, P shape)
tools/site.sh SURFACES                      # sprite sheets in memory (texture-memory budget)
tools/site.sh CMDS                          # next recorded frame's command list -> debug/cmds.txt (with layer sizes)
tools/site.sh MODELS                        # loaded 3D models (vertices x frames, flags, ranges)
tools/site.sh "SET gpu3d 1"                 # 3D scenes as GPU model instances (perf/special-stage; off at boot)
tools/site.sh "SET depthmode N"             # renderer debug bits (docs/gpu-renderer.md: depth convention, mask views)
tools/shot.sh top|bottom|right out.png      # screenshot (reads what the GPU drew too; right = right eye in 3D)
tools/perf-sweep.sh "1 0 GHZ1" ...          # visit scenes, run right, log SITE STATE
```

Files on the SD (`sdmc:/3ds/Sonic Mania 3DS/`): `Data.rsdk`, `debug/log.txt` (boot steps), `debug/perf-NNNN.csv` (one row a
second), `dev-settings.ini` (the bottom-screen switches). Luma crash dumps are in `/luma/dumps/arm11/`; resolve the PC/LR with
`arm-none-eabi-addr2line -f -C -e bin/3DS/CTR/RSDKv5U.elf` (see the dump format in `docs/gpu-renderer.md` lessons:
`crash_dump` words 10.. are r0-r12, sp, lr, pc, cpsr).

- The owner's console is an **Old 2DS** (Old 3DS CPU). Its IP changes (DHCP): ping first.
- Ask before installing in a session unless the owner has said otherwise for that session. A relaunch loses unsaved progress.
- After the game freezes the console needs a manual power cycle; the first install of a build that cannot be
  installed over FTP (a bug in the installer) is manual: upload to `/cias/` and install with FBI.
- `DEV_TOOLS=0 ./build_3ds.sh` builds without the dev tools (production).
- **No console at hand**: `tools/azahar.sh` runs the build in the owner's Azahar (Flatpak) and the same tools work with
  `-i 127.0.0.1` (`tools/site.sh -i 127.0.0.1 STATE`, `tools/shot.sh -i 127.0.0.1 top x.png`). Good for pictures, crashes
  and libctru panics (PC/LR in Azahar's log, resolve with addr2line); useless for timings; it does not reproduce the
  stencil (masked inks). Never `pkill -f azahar` from a script: the pattern matches the script's own command line.

## Tests

Test on hardware whenever performance is involved; emulators say nothing about Old 3DS timings. Verify rendering changes with
a *moving* scene and screenshots (`tools/shot.sh`), never with a static one.
