# GPU renderer: design, state and how to continue

Written 2026-10-22 at the end of the first GPU session, for whoever (a new session, another model) picks it up.
Read `docs/PLAN.md` first (status, priorities, decisions); this file is the technical detail of the renderer work
on branch `feat/gpu-renderer`.

## Goal

Everything that makes sense on the GPU goes to the GPU (owner, 2026-10-22), because the owner also wants a graphical
bottom screen (done: `BottomUi`) and stereoscopic 3D (needs the layers as separate GPU draws). Target hardware: the
owner's Old 2DS (`192.168.1.141` at the time of writing; the IP is DHCP, ping first). The game logic is not the problem
(3-7 ms); drawing is.

## What the numbers say (Old 2DS, 80 MB mode)

Per **drawn** frame in Green Hill 1 (no frame skip): update ~4 ms, tile layers ~10 ms (the cost is overdraw: 4-7
full-screen parallax layers), sprites < 1 ms, volcado/flip ~4-7 ms. ~22 ms total = ~44 fps at best, 25-35 in practice
with the bottom screen UI (~2.5 ms). **The earlier "58 fps" figures counted logic passes, not presented frames** (with
frame skip the logic ran at 60 and the picture at ~30). Special Stage: `Draw3DScene` 12-84 ms + `DrawFace` 6-42 ms, 7-28 fps.

Draw order of a frame (`SITE ORDER`; L layer, S sprite, F face, R rotozoom, P shape, 3 = 3D scene), very regular:

| Zone | Order |
|---|---|
| Green Hill 1 | `L4 S6 L1 S22` |
| Chemical Plant 1 | `L6 S2 L1 S9 L1 S22 F6 S3 F2 S18` |
| Studiopolis 1 / Hydrocity 1 | `L7 P1 F4 S1 F2 S15 R2` |
| Press Garden 1 | `L6 S17 L1 S14 F6 S3 F2 S16` |
| Oil Ocean 1 | `L7 S6 L1 S14` |
| Special Stage | `L8` then ~800 faces and the 3D scene |

In `ProcessObjectDrawLists` each draw group draws its **entities first, then its layers**.

## Architecture (what exists)

`RSDKv5/RSDK/Graphics/CTR/`:

- `C3DPresent.cpp/.hpp`: citro3d + citro2d core (`InitCore`), the top-screen render target (RGB565, 240x400 tilted),
  and the *present path*: the software picture is copied into a 512x256 **linear** staging buffer, tiled into a VRAM
  texture by `C3D_SyncDisplayTransfer`, drawn as one quad (`present.v.pica`). Used when the frame is not drawn by the GPU
  renderer. Menu/`SET gpu 1` (default on).
- `GpuFrame.cpp/.hpp` + `GpuFrameCmd.hpp` + `GpuFrameHazard.hpp`: **record-and-execute**. Inside
  `ProcessObjectDrawLists` (`GpuFrame::Begin/End` in `Scene/Object.cpp`) every framebuffer-writing function
  (`FillScreen, DrawLine, DrawRectangle, DrawCircle, DrawCircleOutline, DrawFace, DrawBlendedFace, DrawSpriteFlipped,
  DrawSpriteRotozoom` in `Graphics/Drawing.cpp`; `DrawLayerBasic/HScroll/VScroll/Rotozoom` in `Scene/Scene.cpp`) calls
  `GPUFRAME_REC(Rec...)` first and returns if it was recorded. At `FlipScreen` (`CTRRenderDevice.cpp`) `GpuFrame::Present`
  tries the GPU executor; if it refuses (or is off) the list is **replayed through the same software functions in order**
  (`Flush`), so an unsupported frame looks exactly as before. Each command stores the clip bounds and screen position of
  its call; `gfxLineBuffer` changes are recorded as `CMD_LINEBUF`; layer commands carry a copy of the `scanlines` table.
  **Hazards** (`GPUFRAME_HAZARD/PALETTE_HAZARD`): `DrawDeformedSprite`, `DrawDevString`, `DrawAniTile`, `Draw3DScene` and
  every palette mutator (`LoadPalette`, `BlendColors`, `SetPaletteFade`, the inline `SetPaletteEntry/Mask`,
  `CopyPalette`, `RotatePalette`) flush what is pending (replay with the pre-change state) and the rest of the frame is
  drawn immediately. `paletteVersion` is bumped by every palette change; `contentVersion` by every scene load
  (`GpuFrame::InvalidateContent` in `RetroEngine.cpp`).
- `GpuRender.cpp/.hpp` + `gpu.v.pica`/`gpu_shbin.h`: the executor (P1). Walks the stream and builds, on the CPU, quads
  (screen x,y,u,v + RGBA bytes, 20 B/vertex, two vertex buffers + a static index buffer in linear memory) for
  tile layers (BASIC and HSCROLL, one palette bank per layer), sprites (`DrawSpriteFlipped`, 4 flips, inks NONE/BLEND/
  ALPHA/ADD/SUB), rectangles (same inks) and `FillScreen` (per-channel alpha via the constant-colour blend factor).
  Anything else, or an ink/size/atlas problem, returns false with a `Reason` (`SITE STATE` shows `reason`/`cmd`).
  Textures: a **tile atlas** (512x512 RGBA5551, 1024 cells of 16x16, keyed by (bank, tile id incl. flip bits)) and a
  **sprite atlas** (1024x512 RGBA5551, shelf packer in 8-pixel units, keyed by (sheet, bank, sprX, sprY, w, h); a solid
  white 8x8 block at the origin serves rectangles/fills). Both live in **linear memory** and are written by the CPU in
  the PICA 8x8 Morton layout, then cache-flushed (the CPU cannot write VRAM from an installed title). Conversion is lazy,
  queued during the build and done after `C3D_FrameBegin`; a stale (palette changed) or reused cell makes the frame begin
  with `C3D_FRAME_SYNCDRAW` so the GPU is not reading it. TEV = texture * vertex colour; blending per batch.
  Texture V: PICA samples memory row 0 at v = 1 (`AddQuad` does `1 - y/H`).
- `BottomUi.cpp` (in `Dev/CTR/`): the citro2d bottom screen (fps, stage bar, buttons, modal install dialog).
- `DevTools.cpp`: FTP server, perf CSV, remote control (see CLAUDE.md), `StageScope` timers, `SITE ORDER`.

Build wiring: `Makefile` source list + `CMakeLists.txt` + `makefiles/3DS.cfg` (`-lcitro2d -lcitro3d`). Shaders are
assembled with `tools/gen-shader.sh x.v.pica` (picasso) into a committed `x_shbin.h`.

## State at the end of the session

- **P0 (record + software replay) works and is correct** (verified by screenshots while running right in Green Hill:
  scrolling, Sonic, enemies, HUD). Off by default: `SITE SET record 1`.
- **P1 (GPU executor) runs without crashing but does not work yet.** With `SET record 1`, `SET gpu 1`,
  `SET gpurender 1` in Green Hill the stats say frames are accepted (`gpu` count rising, `reason 0`) but **`quads` is only
  ~22 (the sprites) and `tileconv` is 0: the layers emit no quads**. Diagnostics were just added (`layers`, `tileq`,
  `empty`, `rowsout` in `SITE STATE`: layer commands seen, tile quads, empty tiles, rows outside the layout) and the
  build that has them was being installed when the session ended, so **those numbers have not been read yet**: read
  them first. The owner looked at the screen and said it does not work (not described further; a layer-less frame would
  show only sprites on black). After a few seconds frames also start falling back (`reason 2`, `cmd 5` = `CMD_FACE`,
  `cmd 0` = `CMD_FILL` before FILL support was added).
- Everything is off by default and resets at boot (`record`, `gpurender` are not saved in `dev-settings.ini`).

## Suspects for "layers emit nothing"

1. `EmitLayerBasic/EmitLayerHScroll` use `layer->layout`, `xsize/ysize`, `widthShift` and the `ScanlineInfo` copy: check
   with the new counters whether `layerCmds` > 0 and where the quads go (`emptyTiles`, `rowsOut`). If `rowsOut` is high the
   row index maths is wrong (BASIC uses the first scanline's y and 16-line blocks; HSCROLL groups lines whose
   `position.y` advances by exactly 1 px).
2. `c.kind` vs `LAYER_BASIC/HSCROLL/...` enum values, and that the layer command's `p[0]` pointer survives (32-bit).
3. `UniformBank` is fed by the `sLineBuf` of the stream; check it is not refusing (it would show `reason 4`).
4. The tile index: `layout & 0xFFF` includes the flip bits (index into the 4-variant `tilesetPixels`); `EnsureTile`
   refuses ids >= 4096 (`TILE_RANGE`).
5. Verify the emitted geometry against the software result with a readback tool (below) rather than by eye.

## Tooling gaps to close first

- **There is no reliable way to look at the GPU output.** `SITE SHOT` (top) reads `gfxGetFramebuffer`, which in GPU
  presentation mode is a stale buffer. Add a readback: `C3D_SyncDisplayTransfer` from the render target's colour buffer
  (VRAM, tiled) to a temporary linear buffer, then write it like `SHOT` does. Then add a **GPU CHECK**: render the same
  frame with the software replay and the GPU, diff them, report the differing pixel count and write both images (the
  Zelda port has this: `../zelda-tmc-3ds` `PortPpuGpu3DS` parity checks and `docs/gpu-rasterizer-parity-notes.md`).
- Verify with a *moving* scene. A static scene hides a renderer that draws nothing: the software framebuffer keeps the
  last frame. (This misled the session once: the first P0 replay wrote into `screens[1]`, the screenshot looked right
  and "60 fps" was fake. See the lessons below.)

## Next steps (suggested order)

1. Read the new counters, fix the layer emission, add the readback + GPU CHECK, get Green Hill (`L4 S6 L1 S22`)
   pixel-close to the software frame.
2. Measure on the 2DS (`SITE STATE`, perf CSV, `tools/perf-sweep.sh`) before claiming any gain.
3. Cover the rest of the common frames: `DrawLayerVScroll`, per-line palette banks (water: draw twice with scissor, or one
   texture version per bank), `INK_TINT/MASKED/UNMASKED`, `DrawAniTile` (invalidate tile cells instead of a hazard),
   `DrawSpriteRotozoom`/`DrawDeformedSprite`, faces (`DrawFace`, `DrawBlendedFace`) and circles/lines.
4. Special Stage (`Draw3DScene`): ~800 faces per frame; textured/vertex-coloured triangles are native on the PICA.
5. Cut the cost of the GPU path itself: the present path costs ~2 ms more than the CPU transpose (copy ~1 ms + frame ~4 ms
   with `SYNCDRAW` off it is ~1.3 ms); the executor writes the atlas with the CPU, keep conversions lazy and per-entry.
6. Then stereoscopic 3D (layers as separate draws with depth) and the rest of the bottom UI.

## Lessons (read before touching this)

- **`currentScreen` has already moved on** when `FlipScreen` runs (the draw loop does `currentScreen++`). Replays must
  set `currentScreen = &screens[0]` (they do now); anything reading screen state after the draw loop must use `screens[0]`.
- **Do not change `SCREEN_XMAX`** (tried 1280 -> 400 to free linear memory): the top screen came out garbled/blank even
  after a clean build. Reason not found. It is reverted. Free memory another way.
- `make` does not rebuild objects when a `-D` changes; `build_3ds.sh` wipes `bin/` when `DEFINES` change (stamp
  `bin/.defines`). After switching branches also run `rm -rf bin`.
- **CPU writes to VRAM crash an installed title** (data abort in `memcpy`). VRAM is for render targets/textures filled by
  the GPU or the transfer engine only. Atlases are therefore in linear memory.
- citro3d state is sticky across frames: the present path turns blending off and sets its own program/attributes each
  frame; citro2d (`BottomUi`) calls `C2D_Prepare()` and sets alpha blending itself. Re-apply state after anyone else's frame.
- citro2d text needs a higher z than the rectangles it sits on (modal dialog bug).
- `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` cost ~10 ms per frame (46 fps instead of 60): off by default; use it only when
  rewriting texture cells the previous GPU frame may still read.
- The bottom screen is the citro2d UI now; the libctru text console is only a fallback and must never print past row 29
  or the 40th column (it scrolls).
- Linear memory is the scarce pool. `__ctru_linear_heap_size` is 5 MB now (`main.cpp`), heap size 0 (automatic: the
  Old 3DS cannot give a fixed 52 MB). Atlases (1.5 MB) + two vertex buffers (0.65 MB) + staging 0.25 MB + C3D buffers.
- The engine's fps counter in the dev tools counts passes of the loop, not drawn frames; `skipped` says how many passes
  skipped drawing.
- Old 3DS vs "2DS": the owner's `.141` console is an Old 2DS (Old 3DS CPU). New 3DS numbers would differ a lot.
