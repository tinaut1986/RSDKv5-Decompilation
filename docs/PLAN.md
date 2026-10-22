# Plan

## Status

- **Priority**: B0 (baseline) -> R1 (GPU renderer) -> U1 (bottom screen).
- **Active release line**: `release/v0.1.0` (local only, not pushed yet).
- **Branches**

| Branch | State |
|---|---|
| `main`, `release/v0.1.0` | cut from SaturnSH2x2 `3ds-main` (dfef28d) |
| `chore/build-and-docs` | build script, own title ID/data dir, CLAUDE.md, PLAN. Builds a CIA. Not tested on console. |
| `feat/render-backend` | `gfx-rewrite` merged into the base; builds, not tested on console |

## Tasks

### B0 Baseline
- [ ] B0.1 Build works from a clean clone (`./build_3ds.sh --cia`); document the libtheora workaround. *Done on the owner's machine; check on a clean clone.*
- [ ] B0.2 Run on hardware with the owner's `Data.rsdk` (find out if it is the Plus data), record Old 3DS frame times per stage (draw vs game logic) with a debug overlay. **Gate for R1: if logic dominates, the GPU will not fix it.**
- [ ] B0.3 CI (`build-3ds.yml`, `build-release.yml`), version from git, release notes, as in `../zelda-tmc-3ds`.
- [ ] B0.4 Push branches to `origin`, enable issues.

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

- 2026-10-22: base is SaturnSH2x2 `3ds-main` + `gfx-rewrite` merged (clean merge); gfx-rewrite is stale (last commit 2025-07) so treat it as a starting point, not as proven.
- 2026-10-22: game logic as a submodule instead of the guide's copy-into-`Game/` script.
- 2026-10-22: own title ID `0x5A230` and data folder so it coexists with the upstream build.
