# Plan

## Status

- **Priority**: B0 (baseline, done) -> R1 (GPU renderer: tile layers first, then the Special Stage 3D) -> U1 (graphical bottom screen) -> stereoscopic 3D.
- **Goal fixed by the owner (2026-10-22)**: the GPU renderer is wanted regardless of the current numbers, because the graphical bottom screen and stereoscopic 3D need it.
- **Active release line**: `release/v0.1.0` (local only, not pushed yet).
- **Branches**

| Branch | State |
|---|---|
| `main`, `release/v0.1.0` | cut from SaturnSH2x2 `3ds-main` (dfef28d) |
| `chore/build-and-docs` | merged into the release (2026-10-22) |
| `feat/dev-tools` | merged into the release: FTP + SITE INSTALL/SCENE/INPUT/STATE, perf CSV, touch menu. Checked on the 2DS. |
| `perf/frame-pacing` | clock-scheduled frame skip + tiled flip transpose, draw split by type. On the 2DS: 37 -> 58.6 fps in Green Hill. Not merged yet. |
| `feat/render-backend` | `gfx-rewrite` merged into the base (RenderBackend + SoftwareBackend); builds, not tested on console |

## Tasks

### B0 Baseline
- [ ] B0.1 Build works from a clean clone (`./build_3ds.sh --cia`); document the libtheora workaround. *Done on the owner's machine; check on a clean clone.*
- [x] B0.2 (2026-10-22, 2DS) Run on hardware with the owner's `Data.rsdk` (find out if it is the Plus data), record Old 3DS frame times per stage (draw vs game logic) with a debug overlay. **Gate for R1: if logic dominates, the GPU will not fix it.**
- [ ] B0.3 CI (`build-3ds.yml`, `build-release.yml`), version from git, release notes, as in `../zelda-tmc-3ds`.
- [ ] B0.4 Push branches to `origin`, enable issues.

### Measurements (Old 2DS, 80 MB mode, right+A held; `tools/perf-sweep.sh`)

Logic (update) is 3-7 ms everywhere; the cost is drawing. Before the pacing work Green Hill ran at ~37 fps.
After it: Green Hill, Chemical Plant, Studiopolis, Stardust Speedway, Hydrocity, Mirage Saloon, Lava Reef,
Metallic Madness ~60 fps; Flying Battery 1 47-59; Press Garden and Titanic Monarch ~60 (draw 7-10 ms).
Oil Ocean 1 is 60 fps (tile layers 5 ms of a 6 ms draw) until Sonic falls into the oil and fire sprites appear
(then 15-28 fps, draw up to 48 ms; not yet reproduced with the split timers). **Special Stage 1: 7-28 fps**,
draw 22-122 ms: `Draw3DScene` 12-84 ms, `DrawFace` 6-42 ms, tile layers 8-22 ms. Sprites cost 0.2-0.4 ms.
So the GPU order is: **tile layers** (largest everywhere), **Special Stage 3D** (faces + 3D scene), sprites last.

### GPU findings (2026-10-22, Old 2DS)

- **GPU presentation works** (`C3DPresent`, menu PRESENT / `SITE GPU 1`, off at boot): the software picture is copied to a
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

### R1 GPU renderer
- [ ] R1.1 Merge `feat/render-backend`; confirm identical output to `3ds-main`.
- [ ] R1.2 `C3DBackend` skeleton on `RenderBackend`: sprite sheets as RGBA5551 textures, per-palette cache with dirty tracking (see `../zelda-tmc-3ds` `port_ppu_gpu_3ds.c`), everything else falls back to `SoftwareBackend`.
- [ ] R1.3 Sprites + tiles (`INK_NONE/ALPHA/ADD/SUB`), per-line palette bank (`gfxLineBuffer`, water).
- [ ] R1.4 Rotozoom, deformed sprites, faces; `INK_TINT/MASKED/UNMASKED`.
- [ ] R1.5 Presentation without the CPU rotate copy in `CTRRenderDevice::FlipScreen`; frame pacer for Old 3DS.
- [ ] R1.6 CPU/GPU parity check (GPU CHECK) using `SoftwareBackend` as reference.

### U1 Bottom screen / menu
- [ ] U1.1 Decide contents (map, HUD, touch buttons, options) once R1 is stable.

## Decisions log

- 2026-10-22: the engine ran one logic tick per loop pass and a drawn frame ended in a vblank wait, so any pass over
  16.7 ms slowed the whole game (the 400x240 rotate-copy alone cost ~9 ms). Fixed by scheduling passes on the clock
  (skip draw+flip when a tick late, at most 2 in a row) and a tiled transpose. `SITE INSTALL` compares the CIA's title
  ID with `0x0004000005A23000` (UniqueId 0x5A230 shifted by the variation byte), not `0x5A230`.
- 2026-10-22: `__ctru_heap_size = 0` (a fixed 52.5 MB heap panicked `__system_allocateHeaps` on the Old 3DS) and
  `SystemMode: 80MB`, as legend-of-doom-3ds. The engine mallocs ~46.5 MB of storage at start; heap used ~50 MB.

- 2026-10-22: base is SaturnSH2x2 `3ds-main` + `gfx-rewrite` merged (clean merge); gfx-rewrite is stale (last commit 2025-07) so treat it as a starting point, not as proven.
- 2026-10-22: game logic as a submodule instead of the guide's copy-into-`Game/` script.
- 2026-10-22: own title ID `0x5A230` and data folder so it coexists with the upstream build.
