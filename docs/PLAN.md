# Plan

## Status

- **Priority** (owner): R1 GPU renderer (everything that benefits goes to the GPU) -> graphical bottom screen
  (v1 done) -> stereoscopic 3D. **Technical detail and next steps of R1: [`docs/gpu-renderer.md`](gpu-renderer.md).**
- **Where things stand (end of 2026-10-23, after session 3):** everything below is merged into `release/v0.1.0`.
  The GPU renderer is what the owner plays with (RENDERER button / `SITE GPU 1`, saved). On the 2DS: Green Hill 60 fps,
  title, menu and the Special Stages all on the GPU (Special Stage 1 48-60 fps turning, `gpu3d` on by default and
  saved). Water lines, LINE, PALETTE, scissored rotated sprites on the GPU. Bottom screen with tabs (GAME, OPTIONS,
  DEBUG), top FPS counter, loading box. Stereoscopic 3D works (checked on the owner's New 3DS: Special Stage with depth
  for the models, floor, sky, track sprites and item boxes; elsewhere layer parallax and the HUD). **2026-10-23,
  session 4 (`feat/layer-depth`, merged):** zones get a stereo depth per band of lines from the horizontal parallax
  (backgrounds in several planes), background sprites sit with their layer, the title screen has its own depths (logo
  out of the screen, island and trees by line), `stereofar` scale, DEPTH TINT debug view, `SITE HIDE`, `SITE DEPTHS`.
  **End of session 4:** `release/v0.1.0` holds the rotozoom textures, the per-bank copies and the run tools
  (merged, pushed); `perf/floor-lerp` (floor windows with two-pass fog, hard-reset runs, `ufonofall`, command buffer
  stats) is committed, not merged. Real fps (FRAME SKIP off, FORCE 3D) on the owner's Special Stage 2 run: 18.4 before
  -> 30.0. Next: the right eyes dropped in 3D (the 3D scenes' command buffer use: docs/gpu-renderer.md "Session 4
  hand-off").
  **Later in session 4 (`perf/roto-texture`, `perf/special-floor`, not merged yet):** rotozoom layers up to 1024 px from
  textures (title 55 -> 58.5 fps on the 2DS), one copy per fog bank (Special Stage 2 50 -> 58.7 fps); the floor windows
  for the big Special Stage floor were slower on the 2DS and are off (`SET floorwin 1`). Still CPU: INK TINT
  (the pause menu). **Pending work lives in the GitHub issues** (#1 save states, #2 in-game updates, #3 language,
  #4 bottom-screen themes, #5 INK TINT, #6 water line (done: close after the owner's check), #7 Special Stage 60 fps
  turning / with 3D and the floor's quads, #8 options in production builds). Technical record:
  [`docs/gpu-renderer.md`](gpu-renderer.md).
- **2026-10-23, session 5, merged into the release:** the audio on core 1, the floor levels, the 3D scene runs; the
  Special Stage 2 run 36.9 -> 57.7 fps, 29.0 -> 49.5 with 3D. **Pending (pushed, not merged):** #11, #12, #13 on
  `fix/3d-depth-bands` (see the branch table), the 2DS hang, Special Stage 7 with 3D at ~31 fps, #10 (floor through
  the track, minor). The full record: `fix/3d-depth-bands`'s docs/gpu-renderer.md.
- **2026-10-23, session 5 (`perf/audio-core1`):** the audio took ~20 % of core 0 (Ogg decoding); now decoded on
  core 1. Special Stage 2 run 43.4 fps / 35.2 with 3D (46.1 / 36.5 with `floorwin 1`). Next: the floor (9-13 ms of
  CPU a frame). See gpu-renderer.md "The audio off core 0". Then `perf/floor-lod`: the floor from five window levels,
  **56.0 fps / 45.1 with 3D** on the same run (gpu-renderer.md "The floor in five levels"). Then
  `perf/stereo-submit`: **57.7 / 49.5 with 3D** (gpu-renderer.md "Runs of 3D scenes"). Tour of all the Special
  Stages: 7 and 1+ all on the CPU (DEFORMED SPRITE, #11), 4 / 4+ ~13 fps (animated floor palette, #12), 3D models in
  the wrong depth order everywhere (#13), floor through the track (#10). gpu-renderer.md "Session 5 hand-off".
  Next (owner): #11.
- **Active release line**: `release/v0.1.0`, on `origin`.
- **Consoles**: the owner's Old 2DS at `192.168.1.141` and New 3DS at `192.168.1.133` (DHCP: ping first;
  `tools/site.sh -i IP`). Both run the latest dev build of `release/v0.1.0`; the New 3DS takes `--install` over FTP too
  (it rewrites `.3ds_ftp_ip`: set it back to the 2DS). Ask the owner before installing in a new session (each session's
  "install without asking" ends with it); an install relaunches the game (the player loses the spot). **Without a
  console**: `tools/azahar.sh` runs the build in the owner's Azahar with the dev tools on `127.0.0.1:5000` (pictures,
  crashes, reasons; not timings, not the stencil). Azahar's renderer was set to Vulkan for this (backup
  `qt-config.ini.claude-backup` in `~/.var/app/org.azahar_emu.Azahar/config/azahar-emu/`).
- **Branches**

| Branch | State |
|---|---|
| `main` | SaturnSH2x2 `3ds-main` (dfef28d). |
| `release/v0.1.0` | Everything the owner checked on the console, merged with `--no-ff`: build/docs, dev tools, GPU presentation, bottom UI, pacing, the GPU renderer (layers, sprites, faces, 3D scenes as instances, masked inks, rotozoom textures), bottom tabs, stereo 3D and layer depth, recorded runs; and on 2026-10-23 (session 5) `perf/floor-lerp`, `perf/audio-core1` (music decoded on core 1), `perf/floor-lod` (the floor in five window levels) and `perf/stereo-submit` (runs of 3D scenes keep their state). Merged topic branches deleted. |
| `feat/gpu-deformed-sprite` -> `perf/ss4-palette` -> `fix/3d-depth-bands` | **Stacked on the release's last merge, pushed, not merged**: #11 the plasma sky on the GPU (Special Stages 7 / 1+ ~18 -> ~60 fps), #12 animated tiles counted only where used (4 / 4+ ~13 -> ~58), #13 each 3D scene's depth band over its own Z range. Checked by Claude on the 2DS (frozen frames match the software; a tour of the 14 entries at ~60 fps without 3D), **not yet by the owner**, and the 2DS hung on a frozen Special Stage 7 with FORCE 3D while debug switches were toggled (gpu-renderer.md "Tour after the fixes; a hang"). Merge `fix/3d-depth-bands` (it holds the other two) once the owner has seen them and the hang is understood. |
| `feat/loading-screen` | bottom-screen loading indicator; **not confirmed by the owner, not merged**; conflicts in spirit with `BottomUi` (text console). |
| `feat/render-backend` | `gfx-rewrite` base, unused (superseded by record/replay). Reference or delete. |

## Tasks

### B0 Baseline
- [ ] B0.1 Build works from a clean clone (`./build_3ds.sh --cia`); document the libtheora workaround. *Done on the owner's machine; check on a clean clone.*
- [x] B0.2 (2026-10-22, 2DS) Run on hardware with the owner's `Data.rsdk` (find out if it is the Plus data), record Old 3DS frame times per stage (draw vs game logic) with a debug overlay. **Gate for R1: if logic dominates, the GPU will not fix it.**
- [ ] B0.3 CI (`build-3ds.yml`, `build-release.yml`), version from git, release notes, as in `../zelda-tmc-3ds`.
- [x] B0.4 Branches pushed to `origin` (2026-10-22). [x] Issues enabled (2026-10-23). GitHub's default branch is still `master` (upstream's history).

### Measurements (Old 2DS, 80 MB mode, right+A held; `tools/perf-sweep.sh`)

**Correction (late 2026-10-22):** the "fps" in the dev tools counts logic passes, not drawn frames; with the frame skip on,
the numbers below are passes (the picture ran at about half that). Real per-drawn-frame cost in Green Hill 1: update ~4 ms,
tile layers ~10 ms, sprites < 1 ms, flip 4-7 ms, bottom UI ~2.5 ms: ~22 ms, i.e. ~44 fps at best.

Logic (update) is 3-7 ms everywhere; the cost is drawing. Before the pacing work Green Hill ran at ~37 fps.
After it (passes/s): Green Hill, Chemical Plant, Studiopolis, Stardust Speedway, Hydrocity, Mirage Saloon, Lava Reef,
Metallic Madness ~60 fps; Flying Battery 1 47-59; Press Garden and Titanic Monarch ~60 (draw 7-10 ms).
Oil Ocean 1 is 60 fps (tile layers 5 ms of a 6 ms draw) until Sonic falls into the oil and fire sprites appear
(then 15-28 fps, draw up to 48 ms; not yet reproduced with the split timers). **Special Stage 1: 7-28 fps**,
draw 22-122 ms: `Draw3DScene` 12-84 ms, `DrawFace` 6-42 ms, tile layers 8-22 ms. Sprites cost 0.2-0.4 ms.
So the GPU order is: **tile layers** (largest everywhere), **Special Stage 3D** (faces + 3D scene), sprites last.

### GPU findings (2026-10-22, Old 2DS)

- **GPU presentation works** (`C3DPresent`, part of the RENDERER GPU mode; was menu PRESENT / `SITE GPU 1`): the software picture is copied to a
  512x256 linear staging buffer, tiled into a VRAM texture by the display-transfer engine and drawn as one quad. Needed:
  staging in *linear* memory (the CPU cannot write VRAM from an installed title: crash in memcpy), a 512-wide staging
  buffer (equal transfer dimensions), V from 1 (memory row 0) down to 1 - 240/256. Cost today: copy ~1 ms + frame ~4 ms,
  slower than the CPU transpose (~3 ms), so it stays off: it is infrastructure for the GPU renderer, the bottom screen and stereo.
- **Texture memory**: a stage keeps 31-37 sheets, **6.0-6.7 MB at 8 bit (12-13 MB at 16 bit)**; Special Stage 3.2 MB. VRAM is 6 MB,
  so sheets cannot all be resident: sprites need an on-demand atlas of frames (as the Zelda renderer does), only the tile sheet
  (16x16384, 256 KB at 8 bit / 512 KB at 16 bit) can stay whole.
- **Palettes**: every pixel goes through `fullPalette[lineBank][index]`, the bank chosen per screen line (`gfxLineBuffer`, water), and
  palette entries are mutated immediately by game code (cycles, fades), also while drawing. A deferred (GPU) renderer has to
  record palette versions with the draw commands.
- **Design proposed for R1**: record the draw calls of a frame as a command list (with the palette version each used), then
  execute it: on the GPU when every command is supported, otherwise replayed through the software functions in order (a frame
  fallback, like the Zelda renderer). Supported first: tile layers, ordinary sprites, rectangles/fills; the rest falls back.
- Linear memory is the scarce pool (153 KB free with C3D up): `SCREEN_XMAX` (1280) x 4 screens of static framebuffer can shrink
  to 400 on the 3DS to give the linear heap room.

### R1 GPU renderer (see `docs/gpu-renderer.md`)
- [x] R1.1 GPU presentation of the software picture through citro3d (`C3DPresent`), default on.
- [x] R1.2 Frame command recorder with software replay and hazards (`GpuFrame`, P0), verified on the console.
- [x] R1.3 (2026-10-22, owner checked Green Hill 1 on the 2DS) GPU executor for tile layers (BASIC, HSCROLL), sprites (4 flips; inks NONE/BLEND/ALPHA/ADD/SUB), rectangles,
  `FillScreen` (`GpuRender`, P1): **written, layers emit nothing, not verified**. Needs the readback + GPU CHECK tool first.
- [x] R1.3b (2026-10-22) One RENDERER GPU/CPU switch (CPU = everything in software, as originally) and GPU/CPU frame
  counters with the reason of every CPU frame (bottom screen, `SITE STATE`, CSV, log). Not yet checked on the console.
- [x] R1.3c Rotozoom sprites on the GPU (free quads). Palette tracking per group of 8 colours; animated tiles invalidated.
- [x] R1.4 Readback: `SITE SHOT` reads the GPU's output (checked). A pixel diff GPU CHECK is still to do.
- [ ] R1.4b GPU CHECK: diff the GPU frame against the software replay of the same frame, report and write both images.
- [ ] R1.5 (partly done: rotozoom sprites, masked inks, faces, circles, software layers for rotozoom/vscroll/water-split layers, animated tiles) Water / per-line palette banks, VScroll/rotozoom layers, `INK_TINT/MASKED/UNMASKED`, `DrawAniTile` (cell invalidation),
  rotozoom and deformed sprites, faces, circles, lines.
- [x] R1.6 (2026-10-22, measured by Claude on the 2DS; owner not yet) Special Stage on the GPU: every frame of a whole run
  on the GPU (faces in runs, circles, the rotozoom floor as a software layer). ~9 fps -> 19-23 drawn fps with the frame
  skip; the CPU is the limit: logic ~5 ms, Draw3DScene (recording, lighting) 14-17 ms, face emission 8-9 ms, floor 7 ms.
  Next: build the GPU frame on core 1 while core 0 runs the next frame (the frame is a recorded list already).
- [x] R1.6b (2026-10-22, measured by Claude on the 2DS) Special Stage floor on the GPU as tile runs: 48-60 fps (gpu-renderer.md "Rotozoom layers as tile runs"). 3D scenes as GPU instances done on `perf/special-stage` (`gpu3d`).
- [ ] R1.7 Measure on the 2DS with `tools/perf-sweep.sh` against the software numbers below; keep the software path as fallback.
- [x] R1.8 Stereoscopic 3D: layers by parallax band, sprites, HUD, Special Stage models, title screen (tune `stereofar` with the owner).

### U1 Bottom screen / menu
- [x] U1.1 v1 (`BottomUi`, citro2d): fps, update/draw/flip bar, six touch switches that persist
  (`dev-settings.ini`), modal install dialog with % or restart countdown, FTP address. Dev-tools builds only.
- [ ] U1.2 The real UI in the style of the owner's other ports (tabs, map, touch buttons, options), also in production
  builds. The text console menu is only a fallback.

## Decisions log

- 2026-10-22: a relaunch after `SITE INSTALL` (10 s countdown already) crashed Luma's `loader` (`0xE0E01BF5`, our title
  being loaded); started by hand afterwards the build ran. Other ports saw `0xD86007F3` in the same place (see
  `../legend-of-doom-3ds` PLAN 2026-10-13). It happened on 3 installs of 3 while the game exited with the FTP thread
  still running and citro3d never shut down (`DevTools::Shutdown` was never called, no `C3D_Fini`). With a clean exit
  (FTP thread joined, listen socket closed, `C3D_FrameSync` + `C2D_Fini` + `C3D_Fini` before `gfxExit`, each step in
  the log; the previous run's log is kept as `debug/log-prev.txt`) 5 installs of 5 relaunched with no dump.
- 2026-10-22: `C2D_Init(256)` -> 768: the bottom screen outgrew 256 objects (one per glyph) and came out blank.
- 2026-10-22: bottom screen relaid out at the owner's request: fixed header, buttons half as wide (four to a row) fixed at
  the bottom, the information between them scrolls (drag; the bar on the right jumps). stdout/stderr go nowhere while it
  is up: the engine's prints went to the libctru text console in the same framebuffer and flashed over it on every tap.
- 2026-10-22: the engine ran one logic tick per loop pass and a drawn frame ended in a vblank wait, so any pass over
  16.7 ms slowed the whole game (the 400x240 rotate-copy alone cost ~9 ms). Fixed by scheduling passes on the clock
  (skip draw+flip when a tick late, at most 2 in a row) and a tiled transpose. `SITE INSTALL` compares the CIA's title
  ID with `0x0004000005A23000` (UniqueId 0x5A230 shifted by the variation byte), not `0x5A230`.
- 2026-10-22: `__ctru_heap_size = 0` (a fixed 52.5 MB heap panicked `__system_allocateHeaps` on the Old 3DS) and
  `SystemMode: 80MB`, as legend-of-doom-3ds. The engine mallocs ~46.5 MB of storage at start; heap used ~50 MB.

- 2026-10-22 (late): **option A chosen by the owner**: full GPU renderer (layers + sprites as quads, on-demand atlases, record
  list with a software replay fallback per frame). The hybrid (GPU layers + CPU sprites in overlays) was rejected as throwaway.
- 2026-10-22: the frame skip was removed by the owner (it halved the picture rate while the logic ran at 60); it is a switch,
  off in the owner's `dev-settings.ini`. The loop is paced by the clock (`RenderDevice::UpdateFPSCap`), the vblank wait
  and `C3D_FRAME_SYNCDRAW` are off (SYNCDRAW cost ~10 ms/frame).
- 2026-10-22: `SCREEN_XMAX` 1280 -> 400 (to free linear memory) broke the top screen even after a clean build; reverted,
  reason unknown. Linear heap is 5 MB instead (`main.cpp`), heap size automatic (0).
- 2026-10-22: atlases are in **linear** memory written by the CPU (Morton 8x8 tiles) because a CPU write to VRAM from an
  installed title is a data abort. RGBA5551 (loses the low green bit of the 565 palette; check in GPU CHECK).
- 2026-10-22: base is SaturnSH2x2 `3ds-main` + `gfx-rewrite` merged (clean merge); gfx-rewrite is stale (last commit 2025-07) so treat it as a starting point, not as proven.
- 2026-10-22: game logic as a submodule instead of the guide's copy-into-`Game/` script.
- 2026-10-22: own title ID `0x5A230` and data folder so it coexists with the upstream build.
