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
| `RSDKv5/RSDK/Dev/CTR/` | Dev tools (FTP remote control, bottom-screen UI); the loading box is in every build. |
| `RSDKv5/RSDK/Graphics/CTR/` | 3DS render device (presentation, frame pacing). |
| `dependencies/Sonic-Mania-Decompilation` | Game logic (submodule). `Game` is a symlink to its `SonicMania/` folder. |
| `3ds/` | RSF, banner, icon, `makerom`/`bannertool`, `build_cia.sh`. |

Game data is **never committed**: `Data.rsdk` from the owner's Steam copy
(Flatpak Steam: `~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Sonic Mania/`)
goes to `sdmc:/3ds/Sonic Mania 3DS/` on the console (`UserStorage.cpp`). Without it there, the engine reads the original port's
`/3ds/SonicMania/Data.rsdk` in place, and at the first boot it offers to copy that folder's saves (`*.bin`) into ours
(`Dev/CTR/Upstream.cpp`; the answer is kept in `save-import.txt`). In the emulator the question is answered from a file:
`debug/ask.txt` with `s` (bottom screen to `debug/shot-bottom.raw`), `1` or `0` (`DevTools::AskFromFile`).

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
- Releases: `tools/version.sh` gives the version from git; a tag `vX.Y.Z` runs `.github/workflows/build-release.yml`, which
  builds the CIA and publishes it (beta = pre-release, built with the dev tools, off a `release/*` branch; stable on
  `main`). Notes for the updater: `docs/release-notes/vX.Y.Z.md` (see its README). The in-game updater reads them. `main`, the release branch (`release/v0.1.3` since 2026-10-24, after the `v0.1.2` beta) and the open feature branches are on `origin`.
- Commit only when the owner asks; the owner asked for commits at the end of each long task.
- **Issues** are enabled on `origin` (2026-10-22): pending work, bugs and the owner's requests go there (English, label
  `bug`/`enhancement`, sections Seen / Likely cause / To try / To check, as in sm-3ds); docs keep the technical record.

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
tools/site.sh "SET gpu3d 0|1"               # 3D scenes as GPU model instances (on by default, saved)
tools/site.sh REFUSED                       # the last command the GPU refused, and why (STATE's line is cut by the FTP limit); also why a rotozoom layer is not drawn from its texture, VRAM free
tools/site.sh "GOTO DDWrecker"              # move the player next to an object (the camera does not follow: he may die)
tools/site.sh "SET force3d 1"               # draw both eyes as if the 3D slider were up (cost on a 2DS); also tab/fps/hud/lock30
tools/site.sh "SET depthmode N"             # renderer debug bits (docs/gpu-renderer.md: depth convention, mask views)
tools/site.sh "SET stereofar N"             # stereo depth scale (px of disparity for the farthest layers, default 6, saved)
tools/site.sh DEPTHS                        # last 3D frame's layer depths; debug/depths.txt has every layer's parallax factors
tools/site.sh "SET depthtint 1"             # debug: paint every 2D batch by its stereo depth (DEPTH TINT button), slider down too
tools/site.sh "HIDE TitleLogo"              # debug: stop drawing one object class ("HIDE" alone undoes it)
tools/site.sh "SET ufotime 1"               # debug: the Special Stage countdown stays at 50+ (test it at leisure); not saved
tools/site.sh TIMES                         # one short line: frame, upd, draw, flip (pcopy, pframe), wait, GPU drawing time, quads, batches
tools/site.sh "RUNREC 1800" | RUNSTOP | "RUNPLAY [N] [F]" | RUN  # record keys from a full load of the current scene
                                            # (N passes, then frozen), save debug/run.bin, replay it (F: freeze at the end);
                                            # RUN: the result (real drawn fps). Saved runs and tools/runs/play.sh: tools/runs/
tools/site.sh "SET ufonofall 1"             # debug: the Special Stage's holes are ground (blind test routes); not saved
tools/site.sh EMIT                          # pcopy split by command type (layers, sprites, 3D, lines...)
tools/shot.sh top|bottom|right out.png      # screenshot (reads what the GPU drew too; right = right eye in 3D)
tools/perf-sweep.sh "1 0 GHZ1" ...          # visit scenes, run right, log SITE STATE
tools/site.sh "SET heapcheck 1"             # debug: walk the heap every frame and loading step, log the first broken chunk
tools/site.sh "SET music 0"                 # debug: no music streams (tell a music fault from others); not saved
tools/site.sh "TOUCH 230 125"               # one tap on the bottom screen (320x240): drive its tabs, options, dropdowns
tools/site.sh "SET theme N"                 # the bottom screen's colour theme (OPTIONS > THEME: 0 Mania, 1 classic .. 5 Green Hill)
tools/scene-sweep.sh [-i IP] name           # load every scene, log each load's storage pools; stops if the game dies
tools/ss-tour.sh [-i IP]                    # Special Stages 4, 4+, 7, 1+, 5, 6, 7+: fps and the floor's reason every 5 s
tools/fbi-install.py IP [file.cia]          # send a CIA to FBI's Remote Install when the game's FTP is down
```

Files on the SD (`sdmc:/3ds/Sonic Mania 3DS/`): `Data.rsdk`, `debug/log.txt` (boot steps), `debug/perf-NNNN.csv` (one row a
second), `port-settings.ini` (the bottom-screen options and, in dev builds, the switches; `dev-settings.ini` is the old
name, read once; `Settings.ini` is the game's own). Luma crash dumps are in `/luma/dumps/arm11/`; resolve the PC/LR with
`arm-none-eabi-addr2line -f -C -e bin/3DS/CTR/RSDKv5U.elf` (see the dump format in `docs/gpu-renderer.md` lessons:
`crash_dump` words 10.. are r0-r12, sp, lr, pc, cpsr).

- The owner's consoles: an **Old 2DS** (Old 3DS CPU; 192.168.1.141, on 2026-10-24 it had .133) and a **New 3DS**
  (it had .133 before; 3D screen: check stereo there): the bottom screen's DEBUG tab shows the IP. IPs change (DHCP): ping first. `--install IP` rewrites `.3ds_ftp_ip`.
- **FRAME SKIP is on by default** (`Port::frameSkip = true` in the code; `port-settings.ini` overrides it): with it on, the
  `fps` of `SITE STATE` counts logic passes, not drawn frames. Turn it off (`SET frameskip 0`) before any measurement,
  or use a recorded run (`RUNPLAY`), whose result counts drawn frames.
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
