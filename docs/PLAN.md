# Plan

## Status

- **Priority** (owner): R1 GPU renderer (everything that benefits goes to the GPU) -> graphical bottom screen
  (v1 done) -> stereoscopic 3D. **Technical detail and next steps of R1: [`docs/gpu-renderer.md`](gpu-renderer.md).**
- **Where R1 stands (2026-10-22):** P0 (record + software replay) works; P1 (GPU executor for layers/sprites/rects) runs
  but **does not draw the layers correctly yet** (quads = sprites only; diagnostics added, not read). Both are off at boot.
- **Active release line**: `release/v0.1.0`. Pushed to `origin` (the owner's fork) on 2026-10-22 together with `main`, `feat/gpu-renderer`, `feat/loading-screen` and `feat/render-backend`; the merged topic branches were not pushed (their commits are in the release).
- **Console**: the owner's Old 2DS at `192.168.1.141` (DHCP: ping first; `tools/site.sh -i IP`). The build on it is the
  latest of `feat/gpu-renderer` (with the layer diagnostics, if the last install finished). Ask the owner before
  installing in a new session (the "install without asking" permission was for the previous session only).
- **Branches**

| Branch | State |
|---|---|
| `main`, `release/v0.1.0` | `main` = SaturnSH2x2 `3ds-main` (dfef28d). The release holds, merged: build/docs, dev tools, GPU presentation, bottom UI, clock pacing + frame skip, persistent switches, draw-order tooling. All checked on the 2DS by the owner. |
| `feat/gpu-renderer` | **active**: `GpuFrame` recorder (P0), `GpuRender` executor (P1, broken), diagnostics. Cut from the release. |
| `feat/loading-screen` | bottom-screen loading indicator for boot and stage loads (all builds). Written and installed once; **the owner has not confirmed it, not merged**. It conflicts in spirit with `BottomUi` (it uses the text console): review before merging. |
| `feat/render-backend` | `gfx-rewrite` merged into the base (`RenderBackend` + `SoftwareBackend`). Not used: the record/replay design superseded it. Keep as reference or delete. |
| `chore/build-and-docs`, `feat/dev-tools`, `feat/gpu-present`, `feat/bottom-ui`, `perf/frame-pacing` | merged into the release; may be deleted. |

## Tasks

### B0 Baseline
- [ ] B0.1 Build works from a clean clone (`./build_3ds.sh --cia`); document the libtheora workaround. *Done on the owner's machine; check on a clean clone.*
- [x] B0.2 (2026-10-22, 2DS) Run on hardware with the owner's `Data.rsdk` (find out if it is the Plus data), record Old 3DS frame times per stage (draw vs game logic) with a debug overlay. **Gate for R1: if logic dominates, the GPU will not fix it.**
- [ ] B0.3 CI (`build-3ds.yml`, `build-release.yml`), version from git, release notes, as in `../zelda-tmc-3ds`.
- [x] B0.4 Branches pushed to `origin` (2026-10-22). [ ] Enable issues in the repository settings; GitHub's default branch is still `master` (upstream's history).

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
- [ ] R1.4 Readback of the render target (`SITE SHOT` is blind in GPU mode) and a GPU CHECK (diff against the software replay).
- [ ] R1.5 Water / per-line palette banks, VScroll/rotozoom layers, `INK_TINT/MASKED/UNMASKED`, `DrawAniTile` (cell invalidation),
  rotozoom and deformed sprites, faces, circles, lines.
- [ ] R1.6 Special Stage: `Draw3DScene` and faces on the GPU.
- [ ] R1.7 Measure on the 2DS with `tools/perf-sweep.sh` against the software numbers below; keep the software path as fallback.
- [ ] R1.8 Stereoscopic 3D (needs layers as separate draws).

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
