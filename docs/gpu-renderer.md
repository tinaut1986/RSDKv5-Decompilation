# GPU renderer: design, state and how to continue

Written 2026-10-22, updated at the end of the second GPU session (same day), for whoever picks it up. Read
`docs/PLAN.md` first (status, priorities, decisions); this file is the technical detail of the renderer. The work is
on `release/v0.1.0` (merged) and `perf/special-stage` (3D scenes on the GPU, masked inks: not merged yet).

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
  **Hazards** (`GPUFRAME_HAZARD/PALETTE_HAZARD`): `DrawDeformedSprite`, `DrawDevString`, `DrawAniTile` (only when called
  while drawing; outside it marks tiles changed, see below) and every palette mutator (`LoadPalette`, `BlendColors`, `SetPaletteFade`, the inline `SetPaletteEntry/Mask`,
  `CopyPalette`, `RotatePalette`) flush what is pending (replay with the pre-change state) and the rest of the frame is
  drawn immediately. `paletteVersion` is bumped by every palette change; `contentVersion` by every scene load
  (`GpuFrame::InvalidateContent` in `RetroEngine.cpp`).
- `GpuRender.cpp/.hpp` + `gpu.v.pica`/`gpu_shbin.h`: the executor. Walks the stream and builds, on the CPU, quads
  (int16 x,y in quarter pixels, int16 u,v texels, RGBA bytes: 12 B/vertex; the shader's `uvscale` per texture
  normalises; 8192 quads, two vertex buffers + a static index buffer in linear memory) for tile layers (BASIC, HSCROLL),
  sprites (4 flips; inks NONE/BLEND/ALPHA/ADD/SUB/MASKED/UNMASKED; big ones in chunks cut to the frame), rotozoom sprites
  (free quads), rectangles, `FillScreen` (per-channel alpha via the constant-colour blend factor), faces and blended
  faces (fans; runs of faces are one command), circles and rings (polygons), 3D scenes (`GpuScene3D`, below) and
  "software layers": a layer the GPU cannot draw (rotozoom, vscroll, split by the water line) is drawn by the engine
  into `screens[1]` and uploaded as a texture by the transfer engine.
  Anything else, or an ink/size/atlas problem, returns false with a `Reason` (`SITE STATE` shows `reason`/`cmd`).
  Textures: a **tile atlas** (512x512 RGBA5551, 1024 cells of 16x16, keyed by (bank, tile id incl. flip bits)) and a
  **sprite atlas** (1024x512 RGBA5551, shelf packer in 8-pixel units, keyed by (sheet, bank, sprX, sprY, w, h); a solid
  white 8x8 block at the origin serves rectangles/fills). Both live in **linear memory** and are written by the CPU in
  the PICA 8x8 Morton layout, then cache-flushed (the CPU cannot write VRAM from an installed title). Conversion is lazy,
  queued during the build and done after `C3D_FrameBegin(0)` (it waits for the previous GPU frame; no SYNCDRAW). Palette
  changes are tracked per group of 8 colours, animated tiles by hash (sections below). A full sprite atlas is emptied and
  the frame rebuilt once. TEV = texture * vertex colour; blending per batch. Texture V: the PICA samples memory row 0 at
  v = 1 (the shader's `uvscale` = (1/W, -1/H) plus 1).
- `GpuScene3D.cpp/.hpp` + `scene3d.v.pica`: the engine's 3D scenes as model instances (section below).
- `BottomUi.cpp` (in `Dev/CTR/`): the citro2d bottom screen (fps, stage bar, buttons, modal install dialog).
- `DevTools.cpp`: FTP server, perf CSV, remote control (see CLAUDE.md), `StageScope` timers, `SITE ORDER`.

Build wiring: `Makefile` source list + `CMakeLists.txt` + `makefiles/3DS.cfg` (`-lcitro2d -lcitro3d`). Shaders are
assembled with `tools/gen-shader.sh x.v.pica` (picasso) into a committed `x_shbin.h`.

## State at the end of the second session (2026-10-22)

- The renderer works and is the default way to look at the game: `SITE GPU 1` / the RENDERER button (saved). Checked by
  the owner on the 2DS: Green Hill 1 (60 fps, flip ~4-5 ms, ~6.7 ms idle; the waterfall area 60 since the conversion
  speed-up), the title screen, the main menu (diorama with masked inks, 50-55 fps, all GPU), the Special Stage.
- `SITE SHOT` works in GPU mode (it reads the framebuffer the GPU wrote); the old "blind" note no longer applies.
- **Special Stage**: with `SITE SET gpu3d 1` (off at boot until merged) the 3D scenes are GPU instances and the floor
  (a rotozoom layer) is drawn as tile runs (see "Rotozoom layers as tile runs"): ~17 -> ~40 -> **48-60 fps** on the 2DS
  (60 looking down the track, 48-55 turning over the big green field). Every frame is a GPU frame.
- Known CPU-frame reasons still possible: `INK TINT`, `LINE` (the Special Stage's wireframe item boxes),
  `DEFORMED SPRITE`, `ANI TILE` during the draw lists, `DEV TEXT`, a clip smaller than the screen under a rotated sprite,
  a face or a 3D scene. Water zones (Hydrocity, Chemical Plant...) have not been visited in GPU mode yet: a sprite
  crossing the water line still refuses the frame (`WATER LINE`); layers crossing it go through the software layer path.

## Palette tracking in the atlases (2026-10-22)

- Before: every cached tile/sprite was stamped with the global `paletteVersion`, so any palette write (Green Hill's water
  cycle, every frame) reconverted everything on screen and, because a cell in use was rewritten, started the frame with
  `C3D_FRAME_SYNCDRAW` (a vblank wait): 25 fps, flip 30 ms.
- Now `TrackPalette` compares `fullPalette` with a copy at every GPU frame (catches every writer) and stamps the changed
  groups of 8 colours with the frame's generation; a tile/sprite keeps the groups its pixels use and its conversion's
  generation, and is reconverted only when one of its groups changed since. No `SYNCDRAW` (FrameBegin already waits for
  the previous GPU frame). Green Hill 1: 60 fps, flip 5.6-7.7 ms, all frames on the GPU after the title card.
- A refused frame drops its queued conversions: their stamps go back to 0 (unconverted), or the cells keep stale content
  (seen: white/blue blocks where the title card's `FACE` frames had been refused).
- Still ~80 tiles a frame are reconverted in Green Hill (a group changing every frame): per-colour tracking would cut it.
- Animated tiles: `DrawAniTile` copies new frames into `tilesetPixels` (Green Hill's flowers) outside the draw lists, so no
  hazard sees it: it now marks the tiles (`GpuFrame::TilesChanged`) and the executor drops their copies at the next frame.
  Before, the flowers showed a stale animation frame next to fresh tiles (pieces missing).
- Sprite cache key: sprX and sprY overlapped in `SprKey` (sprites drawn from the wrong part of the sheet). Fixed.
- `SITE CMDS` writes the next recorded frame's command list to `debug/cmds.txt` (type, clip, screen position, params).

## Special Stage (2026-10-22)

- `Draw3DScene` is not a hazard: its faces are recorded through DrawFace/DrawBlendedFace, appended to run commands
  (`CMD_FACES`, `CMD_BLENDED_FACES`: arena = per face n, colour(s), vertices) so ~4000 faces take a handful of commands.
  Its insertion sort became a stable merge sort.
- Vertices are 12 bytes (int16 quarter pixels / texels; `uvscale` uniform per texture): 8192 quads fit in the linear heap.
- The floor is a rotozoom layer drawn by the software into a texture (soft layer), uploaded by the transfer engine.
- Circles and rings are polygons. A whole run, exit included, is GPU-only.
- Cost per drawn frame (2DS): logic ~5, Draw3DScene ~15, face emission ~8, floor ~7 ms: the CPU is the limit.

## Rotozoom layers as tile runs (perf/special-stage, 2026-10-22)

- The Special Stage floor and the title's island are LAYER_ROTOZOOM: per line the software walks the layer from
  `scanlines[y].position` by `.deform` a pixel (16.16), with the line's palette bank (the Special Stage fogs the floor
  with banks 1-7 by distance). `EmitLayerRotozoom` cuts each line into the runs of pixels that stay in one tile and
  draws each as a 1-pixel-high quad of that tile from the tile atlas with affine texel coordinates in 1/32 texel
  (`TEX_TILE_FINE`: the tile atlas with uvscale / 32). A run's ends are its first and last samples kept 2/32 inside
  the tile, pushed half a pixel step out, so the interpolation never reads the next atlas cell.
- The run ends come from an exact integer DDA per axis (`RotoAxis`: 4 divisions a line, none a run; checked against
  the brute force walk). Empty tiles (0xFFFF, 81 % of the Special Stage map) are walked but not drawn.
- Costs on the 2DS, frozen Special Stage frame with 2678 quads: walk 1.6 ms, + cells and coordinates 3.0 ms, + vertex
  writes 3.5 ms. Lessons: the vertex buffer must be written in whole words (the `Vtx` initialisers compiled to 32 byte
  and halfword stores a quad: 10-18 ms), and counters must be locals (stores through the vertex pointer alias the
  globals). A small per-layer memo (bank, tile) -> cell origin avoids EnsureTile's 200 KB of tables.
- More than 6000 quads (ROTO_MAX_QUADS) or 16384 runs: the layer goes back to the software (soft layer).
- The tile atlas (1024 cells) no longer refuses the frame when full: `EvictTiles` gives back every (bank, tile) not
  used in the current frame (`tile evictions` in `SITE STATE`; about one a second while turning in the Special Stage).
- Debug: `SITE SET depthmode 256` draws rotozoom layers in software again (compare); `SITE FREEZE 1` stops the objects
  and keeps drawing, so both renderers can be compared on one picture; `SITE LAYERS` writes `debug/layers.bin` (layer
  layouts, palettes, the first rotozoom layer's scanlines, tileset) for offline analysis; `SITE STATE` shows
  `roto N us M quads` (last frame).
- Still costing: the walk's cache misses in the 512 KB layout (1.6 ms), the GPU (pframe 1-3 ms turning: 4000+ thin
  quads), the logic (4-6 ms). Ideas: a coarse bitmap of empty 8x8 tile blocks to skip the far lines' empties; or a
  window texture of the floor rendered by the GPU and sampled with one quad a line (needs a window per fog bank or a
  combiner lerp between banks 0 and 7, and VRAM).

## Session 3 (2026-10-22, evening): stereo, lines, palettes, atlas, bottom screen

- **Stereoscopic 3D** (slider, or DEBUG FORCE 3D to measure it on a 2DS): every batch carries a depth (`Batch::depth`,
  pixels of disparity at the full slider); `CmdDepth`: tile layers by parallax (factor 1.0 on the screen plane, far
  backgrounds up to 6 px behind), draw groups 12+ (HUD) 2 px in front. The frame is drawn per eye into the left and
  right targets (`C3DPresent::TargetRight`, `SetStereo` -> `gfxSet3D`), the projection shifted by +-depth/2 per batch.
  2DS: Green Hill 60 with FORCE 3D, Special Stage 39-43 (GPU-bound: models and the floor twice).
- **GPU command buffer 256 KB**: one eye of the Special Stage takes ~26-46 %; with two eyes 128 KB overflowed and froze
  the 2DS (libctru panic, reproduced in Azahar). `SITE STATE` shows `cmdbuf N%` (peak); the right eye is skipped
  (`eyes dropped`) rather than overflow.
- **LINE** (`EmitLine`): the software's clipping and Bresenham steps, pixels gathered into horizontal / vertical runs.
- **PALETTE**: a palette write while recording (a boss flashing, Super Sonic, `Decoration` additive) no longer sends
  the frame to the CPU. `PaletteWillChange` (the hazard macro, called before the write) keeps the palette the frame
  started with; the next recorded command is preceded by `CMD_PALETTE` (a 4 KB snapshot). The executor tracks the
  real banks from the base palette and maps a bank that differs in a snapshot to a virtual bank 8..15
  (`BANKS = 16`, `sBankMap`, `ApplySnapshot`), handed out in order of appearance so frames reuse their atlas copies.
  The software replay applies the snapshots too. `SetPaletteMask` keeps the old hazard (`GPUFRAME_MASK_HAZARD`).
  Sprite key: bank bits 53-56, sheet 57-62 (ConvertSprite reads the sheet from the key: keep both in step).
- **Sprite atlas packing**: a sprite only went to a shelf of its own height (+8); the title's many heights filled the
  atlas with half-empty shelves (ATLAS FULL several times a second). With no room for a new shelf, any taller shelf
  with room is used. Title retries 85 -> 9 in Azahar, no CPU frames.
- **Out of quads**: a frame that runs out of quads with rotozoom tile runs is retried once with those layers in software
  (`quad retries`). `gpu3d` is on by default and saved (off, the Special Stage's faces overflowed: 6 fps).
- **Exit from HOME hung** in "closing" (C3D_Fini waiting for a frame HOME never acknowledged). As in sm-3ds: an APT hook
  drains the queue with an empty frame on suspend/sleep, and the teardown runs on a thread with a 1 s timeout.
- **CIA install "out of memory"** in big stages (the heap is ~51.8 MB used and a 128 KB malloc failed): the installer
  uses a static 32 KB buffer. `SITE STATE` shows heap used and linear free.
- **Bottom screen** (`BottomUi.cpp`): tabs GAME (run info via `GameInfo.c`, compiled as game code), OPTIONS (FRAMES
  AUTO / LOCK 30 / NO SKIP, SHOW FPS corner with a mini-screen icon, TOP HUD shown / auto / hidden, RENDERER), DEBUG
  (the old view + 3D SCENES, FORCE 3D). FPS counter on the top screen in the 5x7 font of the other ports (`UiFont.hpp`),
  drawn by citro2d in GPU frames and into the software picture in CPU frames. TOP HUD AUTO skips the HUD class's draw
  while the GAME tab is open (`DevTools::HideEntityClass`). The engine's debug-mode touch toggled the libctru console
  (consoleClear wiped the bottom screen on every tap): off while the graphical screen is up.
- **Loading box**: boot and every stage load draw a box on the bottom screen from inside the loaders (step, file, bar,
  seconds; `BottomUi::LoadBegin/Phase/Item/Detail/End`). The render device now starts before `InitEngine` (as the mod
  loader's path does) so the boot shows it.
- Tools: `SITE FREEZE 0|1`, `SITE LAYERS`, `SITE GOTO <Object> [dx]` (moves the player; the camera bounds kill him far
  away: not useful yet), `SITE SET tab|fps|hud|lock30|force3d`, `tools/shot.sh right`.
- Still CPU: INK TINT (the pause menu's grey: needs a copy of the frame into a texture), WATER LINE / HSCROLL LAYER split
  by the water line (seen by the owner in a later stage).

## Session 3, later (2026-10-23): water line, scissor, stereo depth in the Special Stage

- **Water line**: a sprite, a big (chunked) sprite or an HScroll layer whose lines use more than one palette bank is
  emitted once per run of lines with one bank (`PerBankRun`, the command's clip cut to the run). A rotated sprite
  across it is scissored per run.
- **Scissor**: batches carry a scissor rectangle (`Batch::scissor`, table `sScissors`, `ScissorFor`); rotated
  sprites and 3D scenes under a clip smaller than the screen are scissored instead of refused. The target is turned:
  screen (x0, y0, x1, y1) -> `C3D_SetScissor(240 - y1, 400 - x1, 240 - y0, 400 - x0)` (both axes backwards; checked
  with `SITE SET depthmode 1024`, which scissors everything to the top-left quarter).
- **3D scenes**: MAX_DRAW3D 64 -> 256 (the second Special Stage draws up to ~60 scenes a frame; over 64 the frame
  went to the CPU as "3D SCENE"); MAX_BATCHES 512. `SITE REFUSED` prints the last refused command (the STATE line is
  cut by the FTP line limit) and how often rotozoom layers went to the software.
- **Stereo depth in the Special Stage** (scene folder UFO*): 3D models shift per vertex in `scene3d.v.pica`
  (uniform `eye`: shift * clamp(1 - Zc / Z, -0.5, 1), Zc = 41000, the player's Z); the floor gets a depth band per line
  from the walk's step (the lines at y 200 on the screen plane, the horizon the farthest; a batch per band); the sky
  layers are the farthest; the track's scaled sprites (rings, spheres) take Z = 0x1000000 / scale (same projection as
  the models); an item box's wireframe cage (lines) takes the depth of its contents sprite, which is scaled
  0x2000000 / Z (`UfoDepths`, a pass over the frame's commands). 2D batches shift by whole pixels per eye: a fraction
  moved the 1-pixel floor runs' samples onto the next atlas cell (stray texels only in 3D).
- **Soft-layer texture in VRAM**: in linear memory (240 KB free once the command buffer is 256 KB) it failed to
  allocate and every rotozoom layer sent to the software was refused (ROTOZOOM LAYER, New 3DS). The second Special
  Stage's floor goes over 6000 quads often and takes that path (issue #7).
- Installs: the New 3DS (192.168.1.133) runs the dev build too; `./build_3ds.sh --install 192.168.1.133` works there
  (it rewrites `.3ds_ftp_ip`: set it back to the 2DS).

## Session 4 hand-off (2026-10-23, end): reliable runs, the command buffer

- **Recorded runs are reliable now.** A run starts from a full load of its scene (`forceHardReset`) in the recording
  and in the playback, with the RNG seeded: a reload of the scene being played keeps the objects' statics, a load from
  another scene makes them anew. The earlier fix (copying the statics) also copied what the load assigns (sprite
  sheet and sound IDs, storage pointers): garbage and a crash on the 2DS; removed. Recorded keys include the circle
  pad (Azahar's arrow keys). `tools/runs/ss2-owner-30s.bin` (see its README) replays on the 2DS to the recording's
  picture, also the first playback after boot.
- `SITE SET ufonofall 1`: the Special Stage's holes read as ground (`GetTile` returns tile 0 for an empty playfield
  tile, Special Stages only); with `ufotime 1`, for blind test routes. Keep both the same when recording and playing.
- **The owner's run on the 2DS with FORCE 3D: 29.5-30.0 fps** (18.4 before the rotozoom textures); floor windows on
  31.3-31.4.
- **Right eyes dropped**: the left eye may use at most ~47 % of the 256 KB command buffer, or the right eye is skipped
  (16 per run of the owner's route). At the run's last frame the left eye used 90 KB, 76 KB of them in the 3D scenes.
  Caching the vertex buffer / attribute set-up between model instances changed nothing (reverted; it may also have
  caused glitches on the chased UFO the owner saw). In Azahar a frame with 10 models in 9 scenes took 12 KB: ~1.3 KB
  a scene, so the cost is per scene, not per model. Suspect: `C3D_BindProgram` switching between the 2D and the 3D
  shader for every scene (citro3d uploads the shader code on a program change), plus the scene's uniforms and state.
  **Next:** keep the 3D program bound across consecutive 3D batches / put both shaders in one DVLB (same code, two
  entry points) so a switch uploads nothing; or a larger command buffer (`C3D_Init(0x40000)` in C3DPresent.cpp; linear
  free in play ~496 KB). Check with `SITE TIMES` (`eye N KB 3d N KB (M models in S scenes) dropped D`).
- Debug added: `SITE TIMES` (GPU drawing time, command buffer use), `SITE EMIT` (pcopy per command type),
  `SITE SET gpuskip` (the 2DS hung once with it), `SITE SET floorwin`, `ufotime`, `ufonofall`; the DEBUG tab shows
  update / draw / flip / idle in their bar colours.
- Open: Special Stage loads took ~30 s late in the session (11 s in the morning); the chased UFO's glitches (check
  with the caching reverted); lines as one quad not measured on their own.

## Measuring with recorded runs; the floor windows again (2026-10-23, late)

- **FRAME SKIP is on by default** and makes `SITE STATE`'s fps count logic passes: the "59 fps" figures of the
  sections below were measured with it on and are wrong. Real figures come from `RUNPLAY` (drawn frames a second).
- Recorded runs (`RUNREC N` / `RUNPLAY N F`): the keys of each logic pass (buttons and circle pad) from a scene's
  reload, the RNG seeded, and the objects' statics copied at the start of the recording and put back at the start of
  the playback (a reload resets only what StageLoad sets), except words that point into the engine's data storage
  (restoring those drifted the 2DS's replay and crashed the next scene load in DefragmentAndGarbageCollectStorage).
  A 30 s run of the owner's, recorded in Azahar, replays on the 2DS to the same picture. The first playback after
  the game starts ended on a black screen once; Special Stage loads took ~30 s in the late session (11 s earlier): to
  look at.
- The owner's Special Stage 2 run on the 2DS with FORCE 3D: 18.4 fps as tile runs (before the rotozoom textures),
  29.7-30.0 fps now; with the floor windows 31.3-31.4.
- Floor windows rewritten: two 512x512 levels (full and half resolution, steps up to 1 and 2), each kept in bank 0
  and in bank 7; a line is two quads (bank 0 times 1 - f, then bank 7 times f added) instead of a pass per fog group.
  The copies are synchronous again: queued copies (GX_TextureCopy from a ring) corrupted textures on the 2DS when a
  frame was refused or the scene changed before the queue ran them (garbage on the floor until a restart), and
  `C3D_TexBind(1, NULL)` (to use a second texture unit) crashes in citro3d 1.7.1.
- Same frozen frame, windows off / on: pcopy 13.5 / 9.7 ms, GPU drawing 12.0 / 12.8 ms, frame 28 / 25.5 ms,
  quads 6062 / 3487. The GPU (two eyes) is the limit now, and the command buffer peaks at 93 %: 213 right eyes
  dropped in a few runs (judder in 3D).
- Lines (`EmitLine`) are one parallelogram a pixel thick instead of a quad per run of pixels.

## Rotozoom textures per palette bank; Special Stage 2 at 59 fps (perf/special-floor, 2026-10-23)

- What Special Stage 2 spent on the 2DS was its small 3D floor (a 16x16-tile grid, 256x256 px, `UFO_Setup_Scanline_3DFloor`
  picks a fog bank per line): `RotoTex` needed one bank, so it was tile runs, often over the quad limit and drawn in
  software (CPU frames "ROTOZOOM LAYER").
- `RotoTex` now keeps a copy per palette bank its lines use (`RotoBank`, 128 KB each here; up to ROTO_BANK_BUDGET, 1 MB,
  for several banks), each line drawn from its bank's copy: no extra passes. Hot colours stay shared (the mask), their
  pass colour taken from the line's bank. Far lines of a small repeating layer span thousands of texels: their texel
  unit grows (`Batch::uvk`, 1/16 << k) instead of refusing the layer. Bugs found on the way: a non-zero bank's batch
  was rebound to the sprite atlas by the generic bind (`curTex` holds a key for roto textures now); the "too big"
  status of the big floor hid the small layer's in `SITE REFUSED`.
- 2DS, same frozen frame (both all-GPU): pcopy 13.7 -> 6.7 ms and 13.7 -> 4.7 ms at two spots. Playing (turning):
  **50.0 -> 58.7 fps**, 59.3 with FORCE 3D. Sweep of the Special Stages with textures: 1, 3, 5, 6 at 58-60 fps with
  and without 3D (4 and 7 ended or loaded during the sample). With 3D, stages 5 and 6 peaked the command buffer at
  93 % and dropped 4-8 right eyes: the next thing to look at for 3D.
- `VRAM_RESERVE` (800 KB for the 3D models) applies to every Special Stage texture.

## Special Stage floor from two map windows (perf/special-floor, 2026-10-23)

- The floor (512x512 tiles, 8192 px) cannot be one texture. From a `SITE LAYERS` dump of Special Stage 1: ~2330 tile
  runs a frame, 909 on lines with a step under 1 texel a pixel, 987 between 1 and 2, 431 between 2 and 4, none beyond;
  the lines up to a step of 2 fit a 1024 px box of the map, those up to 4 a 2048 px one.
- `FloorWin`: two windows in repeating VRAM textures where a map pixel lives at its position modulo the window, so a
  move writes only the cells entering it (a column with one strided `C3D_SyncTextureCopy`, a row with a plain one):
  level 0 1024x1024 texels at full resolution (cells of a tile) for steps up to 2; level 1 512x512 at a quarter (each
  texel the map pixel in the middle of its 4x4; cells of one 8x8 block, 2x2 tiles) for steps 1.5-4. A window moves
  when its lines' box leaves it. Each line is one quad from the finest window holding it whole; the rest stay tile
  runs (`sLineDone`). VRAM: 3.75 MB (both windows and masks); ~185 KB left in the Special Stage.
- **Fog**: UFO_Setup makes banks 1-6 as `SetPaletteFade(k, 0, 7, 36k)` over indices 160-255 and picks the bank per
  line by distance; bank 7 holds a fog colour per ramp of 8 (green, blue, grey, red...: 6 distinct in stage 1). The
  windows hold bank 0, an A4 mask holds each pixel's fog group (g + 1, the distinct bank 7 colours of the indices
  it uses), and per line and group a pass blends the group colour in by f_k (`BLEND_FOG`: alpha test EQUAL, constant
  colour, blend constant alpha). `FloorFog` fits f_k per bank from the palette and refuses (tile runs) when a bank is
  not such a fade within a step and a half.
- Rebuilt when the colours the floor uses (any bank), the layout or its tiles change, once held two frames.
  `sTileEpoch` now moves only when an animated tile's picture really changes (DrawAniTile rewrites tiles every frame,
  mostly the same).
- Results in Azahar (frozen frames, turning; quad counts only): stage 1 floor 2780-5020 quads as runs -> 720-1580; stage 2 frame
  ~6950 -> 2500-3300 quads (it no longer goes over the 6000-quad limit). Against the CPU renderer the windows differ as
  much as the runs did (stage 2: 466 vs 507 pixels); the level-1 lines (rows ~140-160) differ a few hundred pixels more
  from the reduced resolution, hardly visible (fogged). With FORCE 3D cmdbuf peaked at 37 %.
- **Measured on the 2DS (2026-10-23): slower than the tile runs, so off by default** (`SITE SET floorwin 1`, not
  saved). Turning in stage 1: windows 55.1 fps (pcopy 6.5 ms, pframe 3.1) vs runs 59.9 (5.4, 1.5); with FORCE 3D 45.5
  (22 right eyes dropped) vs 58.6. Stage 2: 43.9 vs 48.6. Frozen (no window moves) both cost the same (pcopy 12.7 vs
  13.2 in stage 2). Causes: a window move's copies are `C3D_SyncTextureCopy`, which waits for the GPU's queue; the fog
  passes (one per group, full lines, twice in 3D) add GPU fill. The first try also took the VRAM the 3D models
  needed (every 3D frame to the CPU): masks are A4 now and each window keeps FLOOR_RESERVE (800 KB) free.
- What stage 2 really spends (~12 ms of pcopy) is its small 3D floor drawn in software every frame: the next target.
  To revive the windows: queue the copies (GX_TextureCopy into the frame's queue, a staging area per copy) and fog
  without per-group passes.
- Still: stage 2's small 3D floor (16x16 tiles, fog banks per line) is tile runs and sometimes a software layer
  (`RotoTex` needs one bank: the same fog passes would do); `SITE SET depthmode 256` (rotozoom layers in software,
  debug) draws that layer black, the CPU renderer draws it: a bug of the debug mode.
- Debug: `SITE REFUSED` `floor A+B/C+D lines` (lines drawn from level 0 + level 1 / candidates), or why not (`few
  lines`, `changing`, `fog`, `no vram`); `depthmode 512` turns the windows off too. `SITE SET ufotime 1` keeps the
  Special Stage's countdown (UFO_Setup->rings) at 50 or more, not saved.

## Rotozoom layers from a texture of their own (perf/roto-texture, 2026-10-23)

- The title's island (64x64 tiles, 1024x1024 px) and clouds (16x16) were ~3500 one-pixel tile-run quads of the ~4000
  a frame: the same cost as the Special Stage floor. A rotozoom layer up to 1024x1024 px is now drawn whole into a
  VRAM texture (`RotoTex`, RGBA5551, GPU_REPEAT as the layer wraps; per tile four Morton blocks through the staging
  buffer and the copy engine) and each screen line is one quad with affine texel coordinates in 1/16 texel (from the
  line's start reduced into the texture; the ends half a step outside the first and last samples). Title: ~4060 ->
  ~950 quads (480 of them the layers), checked against the software on frozen frames in Azahar (215 pixels differ,
  the tile runs 294). Stereo: a depth per line as before.
- Rebuilt when what it depends on changes and has then held for two frames (the tile runs draw it meanwhile): the
  colours its pixels use (`used`, from its distinct tiles), the layout, the tileset epoch (animated tiles).
- **Hot colours**: the title rotates indices 140-143 (the water) every 6 frames, which rebuilt it ~10 times a second.
  When only a few used colours change (at most 4), they become hot: the texture stays, an A8 mask (1 MB for the island)
  holds (k + 1) * 32 where a pixel uses hot colour k, and each frame every hot colour is painted over from the mask
  (`BLEND_HOT`: alpha test EQUAL, constant colour, opaque) with the same line quads. Two builds per visit, none after.
- Slots: two; freed on a scene change (with the 3D models, which held VRAM until the next 3D scene: after a Special
  Stage the island found no VRAM), or when not drawn for a second and a slot is needed. A layer whose texture cannot
  be allocated is drawn with tile runs until the next scene. VRAM free in the title: ~1.4 MB.
- Debug: `SITE SET depthmode 512` draws them as tile runs again; `SITE REFUSED` ends with `roto texture <why>` (`-`
  drawn from it; `too big`, `no vram`, `banks` lines in several palette banks, `changing`, `quads`, `range`) and the
  VRAM free; `SITE STATE` `roto N us M quads tex B` (B: builds since boot).
- Measured on the 2DS (2026-10-23): roto build 3.9 -> 0.9 ms, pcopy 5.3 -> 1.3 ms, 55.0 -> 58.5 fps; with FORCE 3D
  56.2 -> 57.2 fps, the GPU slower (pframe 4.0 -> 7.7 ms: the hot passes, and sampling a big texture at steps of up to
  2.5 texels).
- Next: the Special Stage floor (~512x512 tiles) does not fit: a window texture around the player for near lines plus
  a reduced copy of the whole map for far ones, fog by distance in the combiner (issue #7).

## Stereo depth by layer band (feat/layer-depth, 2026-10-23)

- `layer->parallaxFactor` is the **vertical** factor of an HSCROLL / BASIC layer (horizontal only for VSCROLL and
  rotozoom); the horizontal parallax lives in `scrollInfo[i].parallaxFactor`, one per band of lines
  (`layer->lineScroll[row]` -> index). The old depth used the vertical factor, so most backgrounds came out flat.
- `ParallaxDepth(f)`: disparity = (1 - f/0x100) * `stereoFar` (0x100 on the screen plane, 0 the farthest); factors
  over 0x100 (foregrounds faster than the camera) in front, at most half the scale. `EmitLayerHScroll` splits bands
  where the depth changes and gives each its own; other layer kinds take their nearest scroll info.
- Sprites drawn after a background layer and before the frame's last playfield layer (depth 0) take that background
  layer's nearest band (`sBehind`, `sPlayfieldEnd` from a pass over the commands): background objects sit in the
  background, not on Sonic's plane. Only behind depths propagate (a foreground layer does not pull sprites out).
- The title screen (scene folder `Title`) has its own table (`TitleDepth`, the camera never moves): clouds far,
  island and its 3D trees halfway, mountains / reflection (group 1) far, logo + Sonic + menu text (group 4) out at
  -2/3 of the scale.
- Title island and clouds (rotozoom layers 3 and 2): a depth per line as the Special Stage floor, against the
  island's nearest line (screen line 239 on the screen plane, the horizon ~5/6 of the scale). The island's trees
  (`Title3DSprite`, group 2, unscaled frames picked by distance) take the depth of the island line at their foot
  (line 152 + i, depth 1 - i / 87; scaled ones from scale / 9.6).
- **DEPTH TINT** (DEBUG tab button, `SITE SET depthtint 1`, not saved): every 2D batch one flat blue by its depth, as in
  `../zelda-tmc-3ds` (dark far behind, bright in front; the playfield mid blue), with the slider down too (depths are
  worked out whenever it is on). Fills and rectangles keep their colours; 3D models are not tinted.
- `SITE HIDE <ObjectName>` (e.g. `HIDE TitleLogo`): one class not drawn, to see what it covers; `SITE HIDE` undoes it.
- `SITE SET stereofar N` (0-16, default 6, saved; also scales the Special Stage). `SITE DEPTHS`: the last 3D frame's
  layer depth ranges and sprites put behind; also writes `debug/depths.txt` with every layer's type, group and factors.
- Green Hill on the New 3DS: clouds 6 px of disparity, mountains and water 4, the playfield 0; 60 fps, cmdbuf 14-17 %.
- A crash in `_free_r` while loading Mania Mode scene 12 during a sweep (scene requests over FTP back to back, New 3DS);
  not reproduced in Azahar, no sign it involves this change (it only reads layer data). Watch for it.

## 3D scenes as GPU model instances (perf/special-stage, 2026-10-22: works, off by default until merged)

- Why: in the Special Stage the CPU spends ~30 ms a frame on the engine's 3D (vertex transforms in AddModelToScene,
  lighting and the face sort in Draw3DScene, face emission). legend-of-doom and sm-3ds got their Old 3DS speed by
  cutting CPU work, not with core 1 (core 1 only does audio there; 80 % of core 1 slowed Doom and froze the 2DS).
- Design (`GpuScene3D.cpp`, `GpuRender.cpp` "3D scenes", `scene3d.v.pica`): AddModelToScene / AddMeshFrameToScene in
  S3D_SOLIDCOLOR_SCREEN and S3D_SOLIDCOLOR_SHADED_BLENDED_SCREEN store an instance (model, frames A/B and timer,
  matrices, colour); Draw3DScene records one CMD_SCENE3D. The executor keeps each model in VRAM (all frames, int16
  position + normal / 4, triangle indices, colours; ~850 KB for the Special Stage, which does not fit the linear heap)
  and the shader interpolates, transforms, projects (X * 2^projX / Z + centre, homogeneous w = Z) and lights as the
  software does. Depth buffer (GPU_RB_DEPTH16 on the top target) instead of the sort; each scene its own 1/64 of the
  depth range. A refused frame replays the same engine calls (GpuScene3D::Replay).
- First try on the 2DS froze the console. Reproduced in Azahar (see below): libctru panicked in `GPUCMD_AddInternal`,
  the GPU command buffer (`C3D_Init(0x8000)`, 32 KB) overflowed with the per-instance uniforms. Now 128 KB.
- Depth: 1/Z inside a scene's band left a model's near and far sides a few 16-bit steps apart (Sonic seen from the
  front). Now z/w linear in min(Z, 65536) inside the band, and a 24-bit depth buffer. `depthmode 0` (default) is right
  (checked in Azahar: Sonic's back, as the software draws him); `SITE SET depthmode 0..7` stays for experiments.
- Checked in Azahar: the Special Stage draws its 3D scenes from instances (`faces 0`, `3d 0.0` ms), every frame on the
  GPU, matching the software picture. Still **off by default** until seen on the 2DS: `SITE SET gpu3d 1`.

## Masked inks (INK_MASKED / INK_UNMASKED) with the stencil (2026-10-22)

- A masked pixel is drawn only where the screen already holds `maskColor`. In a frame that uses masked inks the depth
  test is on (GPU_ALWAYS) and the alpha test drops transparent texels; every batch clears the stencil under it, then a
  colourless second pass sets it to 1 where the fragment equals the mask colour. The comparison runs in the combiner:
  frag (0), frag again with its alpha kept in the combiner buffer (1), ADD_SIGNED colour + (1 - mask) - 0.5 (2),
  DOT3_RGBA x4 (3), alpha = **red** of the result + (1 - frag alpha) (4); alpha test LESS 3.
- Learned on the 2DS: DOT3_RGBA does **not** put its result in the alpha (take it from red with GPU_TEVOP_A_SRC_R).
  Azahar does not reproduce the stencil here (nothing is ever marked): check masked inks on the console only.
- Masked batches are drawn quad by quad (draw where 1, raise to 2, second pass lowers to 1 where the colour drawn is the
  mask colour): within one draw call a later quad saw the stencil an earlier one had raised and drew nothing (the menu's
  diorama: a masked rectangle first, the scene's sprites after it). A 2 means "drawn over".
- Debug: `SITE SET depthmode` bit 3 paints the comparison (unblended), bit 7 its alpha as grey, bit 4 draws masked inks
  as plain sprites, bit 5 adds a red masked rectangle over the screen (shows where the stencil ends at 1), bit 6 tries
  the reverse alpha test.
- Big sprites are chunked on a 128 grid but each chunk is cut to the sprite's frame (a 512x32 strip takes 128x32 chunks):
  the menu's diorama no longer fills the sprite atlas (ATLAS FULL, 7 CPU frames a second -> none).

## Testing in Azahar (when the console is not at hand)

- `tools/azahar.sh` copies the build and (re)starts the emulator, waiting for the dev tools; `--stop` stops it.
- The build runs in the owner's Azahar (Flatpak): `Data.rsdk` and `RSDKv5U.3dsx` in the emulator's
  `sdmc/3ds/Sonic Mania 3DS/`; the dev tools' FTP server listens on the host's port 5000, so `tools/site.sh -i 127.0.0.1`
  and `tools/shot.sh -i 127.0.0.1` work as with the console. Timings mean nothing there; crashes, hangs and pictures do.
- OpenGL fails when Azahar is started from a non-interactive shell ("shared contexts not supported"): its renderer was
  switched to Vulkan in `qt-config.ini` (backup `qt-config.ini.claude-backup` next to it; restore it afterwards).
- A panic shows in Azahar's log (`~/.var/app/org.azahar_emu.Azahar/data/azahar-emu/log/azahar_log.txt`) with PC/LR:
  resolve them with addr2line against `bin/3DS/CTR/RSDKv5U.elf`.
- `SITE MODELS` lists the loaded models (vertices x frames, flags, position and normal ranges).

## Renderer mode and frame accounting (2026-10-22)

- One switch, **RENDERER GPU/CPU** (bottom screen, text console cell, `SITE GPU 0|1`), saved as `gpumode` in
  `dev-settings.ini` (the old `gpu` key is ignored). GPU = `gpu` + `record` + `gpurender` on; CPU = all three off, i.e.
  the software renderer and the CPU copy to the screen, as the port started. CPU is the default. `SET gpu|record|gpurender`
  still flip the parts one by one (the button then shows CPU unless all three are on).
- Every frame shown is counted as GPU or CPU (`DevTools::NoteFrame` from `FlipScreen`). A CPU frame carries a reason
  (`GpuFrame::LastWhy`): `CPU MODE`; a hazard (`PALETTE`, `3D SCENE`, `DEFORMED SPRITE`, `ANI TILE`, `DEV TEXT`,
  `COMMAND LIMIT`); `NOT RECORDED`, `EMPTY FRAME`; or the executor's refusal (`GpuRender::ReasonName`: the unsupported
  command such as `FACE`, `ROTOZOOM LAYER`, an ink such as `INK TINT`, `WATER LINE`, `ATLAS FULL`...).
- Where to read it: the bottom screen (mode, who drew the last frame and why, GPU/CPU frames in the last second and since
  the switch, the three most frequent reasons); `SITE STATE` (`renderer ... frames gpu N cpu N (last second g/c) why X`);
  the perf CSV (`gpu_frames,cpu_frames` per second); `log.txt` every 5 s in GPU mode when a CPU frame happened
  (`render (5 s, scene N): gpu N cpu N | why: FACE 120, PALETTE 3`).
- The fps number counts loop passes and is capped at 60 by the clock pacing: compare renderers by the ms, not the fps.

## The audio off core 0; the hidden costs (perf/audio-core1, 2026-10-23)

- **The audio thread took ~20-24 % of core 0** (SITE C3D: `mix N% ogg N%`), most of it stb_vorbis decoding the Ogg
  music. It runs at a higher priority than the game, so its time hid inside every stage (pframe looked 2-5 ms
  longer than its parts). Mixing alone is ~4 %: a lower mixing rate would gain little.
- **The music is decoded on core 1** by a thread of its own into a ring (~0.37 s) at the end of the stream's
  stb_vorbis buffer (music storage); the mixer stays on core 0 and copies from it (CTRAudioDevice.cpp). The first
  half of the ring is decoded by LoadStream itself. Gaps are counted (`music gaps` in the RUN result, `starved` in
  SITE C3D). GetChannelPos subtracts what the ring holds.
- **Core 1 share: 30 %, no more.** At 80 % (MaxCpu 0xD0) the system services there starved: a Special Stage load took
  70-87 s instead of 28 and the GPU's replies (FrameBegin/End, cache flushes) slowed, fewer fps than at 30 %. The
  whole audio thread on core 1 at 30 % gave the most fps (47.0 / 37.8 with 3D) but crackled: it did not keep up.
  The exheader keeps MaxCpu 0x9E (30 %) and AffinityMask 3 (threads on core 1).
- **The heap has no room for statics:** a 64 KB static ring left zlib no heap to inflate the scene layouts, and the
  Special Stage loaded with every layout tile 0 (CPU renderer too). Uncompress failures are logged now; every load
  logs the storage pools (`storage: STG used/limit KB e<entries> d<defrags> f<refused>`).
- The owner's Special Stage 2 run on the 2DS (quiet: no FTP polling during the run, which costs ~2 fps):
  before 36.9 fps / 29.0 with FORCE 3D; now **43.4 / 35.2**, no music gaps, the owner heard it clean. With the floor
  windows (`SET floorwin 1`) **46.1 / 36.5**: with the CPU freed they now win (they were off as slower).
- Where a 3D frame goes now (ms): logic ~3.5, recording ~2.5-3, floor geometry (pcopy, ~5000 quads as tile runs)
  9-13, citro3d submit ~3 an eye (`SITE C3D`: begin, eye, overlay, end), GPU 10-12 in parallel.
- Debug added: `SITE C3D` (pframe split, audio shares), `SET audioprio`, the **CAPTURE** button (DEBUG tab: freezes
  and saves debug/capture-NNN-* with both eyes, the frame before, the command list and the renderer state; tapped
  again, carries on; `tools/capture.sh [N]` fetches and converts them), `load: <scene> in N ms` in the log.

## The floor in five levels, faded in one pass (perf/floor-lod, 2026-10-23)

- The floor windows are **on by default** (`SET floorwin 0` for the tile runs) and the floor is drawn from five levels:
  512-texel windows at full and half resolution (steps up to 1 and 2), 256-texel windows at an eighth and a sixteenth
  (2048 and 4096 px, steps up to 4 and 8) and the whole map at a thirty-second (it never moves and takes every line
  left, up to the horizon). No floor line is left to the tile runs: the last ones kept the game's exact fog and showed
  as an unfaded strip at the end of the track (seen by the owner).
- **One texture a level, one quad a line:** the window holds the map in bank 0 only and the combiner fades each line
  towards a colour by the line's fade (vertex alpha = 1 - f, `GPU_INTERPOLATE`). The exact fog (a bank 7 copy and a
  second additive quad a line) took twice the VRAM and the fill. The colour is black at 60 % of the game's fade (the
  owner: the far floor just darker, not bluer, and not hidden); `SET floorfog N` (0-100) and `SET floorfogavg 1`
  (bank 7's average over the floor's pixels: navy in stage 2) to compare; neither saved.
- VramReserve() is now what the scene's models not yet in VRAM will take plus 32 KB (it was a fixed 800 KB, which left
  no room for a third level); a level that does not fit is tried again on later frames. VRAM free in stage 2 with the
  five levels: ~1 MB (374 KB with the old two levels and their fog copies).
- The owner's Special Stage 2 run on the 2DS: **56.0 fps** (tile runs 43.6), **45.1 with FORCE 3D** (35.3). This
  morning, before the audio work: 36.9 / 29.0.

## Runs of 3D scenes without the 2D state between them (perf/stereo-submit, 2026-10-23)

- Special Stage 2 records up to ~75 3D scenes a frame, one model each, mostly **in a row** (57 in a row in a capture).
  After every scene the draw loop put the whole 2D state back (program, attributes, buffers, combiner, depth,
  projection) and the next scene put the 3D one back: ~1.4 KB of commands and ~50 us of CPU a scene, twice in 3D.
- Now the 2D state comes back only before a 2D batch, and a scene that follows another sets only its own uniforms
  (screen centre, depth band, lighting). Both shaders are one binary (`renderer_shbin.h` from gpu.v.pica and
  scene3d.v.pica, DVLE 0 and 1; `tools/gen-shader.sh renderer a.v.pica b.v.pica`): citro3d uploads no shader code
  when the two programs share it.
- 2DS, the same frozen frame: command buffer 84-93 % -> 42 %, no eyes dropped. The owner's Special Stage 2 run:
  **49.5 fps with FORCE 3D** (45.1), **57.7** without (56.0).

## Session 5 hand-off (2026-10-23, evening): the Special Stage tour

Branches, stacked, each committed and pushed, none merged (the owner played Special Stage 2 on the 2DS and found it
good): `perf/floor-lerp` -> `perf/audio-core1` -> `perf/floor-lod` -> `perf/stereo-submit`. Merging the last brings
them all. Special Stage 2 run on the 2DS: 36.9 / 29.0 fps (3D) at the start of the session, **57.7 / 49.5** now.

A tour of the 14 Special Stage list entries on the 2DS (category 3: 0-6 the stages, 7-13 the Special Stage+ ones; fps
without FORCE 3D, sampled 15 s after the load, `tools/site.sh STATE`):

| Entry | fps | Seen by the owner | Issue |
|---|---|---|---|
| 1 (3 0) | fine | | |
| 2 (3 1) | ~58 | blue floor lines through the track now and then | #10 |
| 3 (3 2) | 42 | fine | |
| 4 (3 3) | 10-13 | very slow | #12 |
| 5 (3 4) | 37 | 3D models in the wrong depth order (looked like broken polygons) | #13 |
| 6 (3 5) | 37 | ramps: the floor through the arrow, the arrow seen from behind | #13 |
| 7 (3 6) | ~20 | every frame on the CPU: DEFORMED SPRITE | #11 |
| 1+ (3 7) | ~18 | every frame on the CPU: DEFORMED SPRITE | #11 |
| 2+ (3 8) | ~60 | | |
| 3+ (3 9) | ~58 | fine | |
| 4+ (3 10) | slow | as 4 | #12 |
| 5+, 6+ (3 11, 3 12) | 54 | 3D depth order; floor through the track | #13, #10 |
| 7+ (3 13) | 54 | palm trees in the wrong depth order | #13 |

- **#11** `DrawDeformedSprite` has no GPU command: the frame hazards to the CPU whole (layers, sprites, 3D, faces in
  software). The owner's chosen next step.
- **#12** Special Stage 4's floor colours change every frame (`floor changing`, `roto texture changing` in
  `SITE REFUSED`): the windows and the rotozoom textures never hold, the atlas reconverts each frame, ~36 ms of
  geometry.
- **#13** every 3D scene has its own slice of the depth range, later nearer (`Draw3DSceneGpu`, uniform `band`), so a
  model drawn later wins whatever its depth; coplanar faces (a ramp's arrow) fight in the depth test.
- **#10** the floor levels' one-pixel-per-texel sampling (or a line changing level) lets a floor line win over the
  track at some camera angles. Minor (the owner).
- Tools of this session: the DEBUG tab's CAPTURE button (`tools/capture.sh`), `SITE C3D` (pframe split, audio
  shares, music gaps), `load:` and `storage:` lines in the log, `SET floorwin/floorfog/floorfogavg/audioprio`.
- Measuring: `RUNPLAY` with no FTP polling during the run (each poll costs fps); `SITE RUN` gives the result with
  `music gaps`. The 30 s tour script lives in the session's scratchpad: per entry `SITE SCENE 3 N`, wait for the
  load (STATE answers with the old scene while the main thread loads: wait for the `load:` line or the new scene
  number), play, `tools/shot.sh`.
- Debug switches left on the console: `frameskip 0`, `force3d` as last set, `ufotime 1` (not saved); `ufonofall` off.

## Next steps (suggested order)

Pending work is tracked in the GitHub issues (tinaut1986/RSDKv5-Decompilation); in short:

0. Special Stages: #11 DEFORMED SPRITE on the GPU (7 and 1+), #12 Special Stage 4's animated floor palette, #13 the 3D
   scenes' depth order, #10 the floor through the track; the title with the floor levels and the shader binary
   (not checked yet). What is left of a 3D frame (~20 ms on the 2DS: logic ~3.5, recording ~2.5, geometry ~5, the two
   eyes' submit ~5).
1. #1 Save states tab (the owner wants it to reach spots quickly; `SITE GOTO` kills the player far away: the camera
   bounds do not follow).
2. #5 INK_TINT on the GPU (the pause menu's grey: copy the frame into a texture mid-frame, combiner grey).
3. #7 Special Stage: 60 fps turning and with 3D; the second stage's floor over 6000 quads goes to the software
   (~9 ms); 1/32-texel floor coordinates leave a few wrong pixels (1/64 with an offset).
4. #2 in-game updates by channel (needs releases first: PLAN B0.3), #3 language, #4 bottom-screen themes, #8 the
   bottom screen and options in production builds.
5. Sweep every zone in GPU mode (Azahar for pictures and `SITE REFUSED`, the 2DS for timings) and fix by frequency.
6. Sprite atlas: evict least used entries instead of emptying it (retries are rare since the shelf fallback).
7. Stereo elsewhere: per-band depths done (feat/layer-depth); tune `stereofar` with the owner on the New 3DS
   (6 px) with the owner on the New 3DS.

## Lessons (read before touching this)

- **Linear memory is the scarcest thing** (240 KB free with the 256 KB GPU command buffer): new GPU buffers go to VRAM
  when only the GPU and the transfer engine touch them (the soft-layer texture moved there). The heap is ~51.8 MB used
  in a stage: avoid big mallocs at run time (the CIA installer now uses a static buffer).
- **`make` misses included .cpp files** (CTRRenderDevice.cpp is included by Drawing.cpp, CTRInputDevice.cpp by Input.cpp):
  after changing them, `rm -rf bin`.
- **Stereo**: 2D batches must shift by whole pixels (fractions move 1-pixel quads' samples onto the next atlas cell).
- **The GPU command buffer** overflowing panics libctru (a frozen console): watch `cmdbuf N%` in `SITE STATE` after
  anything that adds per-draw uniforms (the second Special Stage with 3D reaches ~90 %; the right eye is dropped rather
  than overflow).
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
- `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` waits for a vblank (up to 16 ms): never use it. `C3D_FrameBegin(0)` already waits
  for the previous GPU frame, so atlas cells can be rewritten after it.
- The GPU command buffer (`C3D_Init`) overflowing makes libctru panic in `GPUCMD_AddInternal` (svcBreak): on the console
  a frozen 2DS. 128 KB now; per-instance uniforms of the 3D scenes filled 32 KB.
- On the 2DS, `GPU_DOT3_RGBA` writes its result to RGB only, not to the alpha (read it with `GPU_TEVOP_A_SRC_R`).
- Azahar does not reproduce the stencil here: masked inks can only be checked on the console. Everything else (pictures,
  crashes, panics with PC/LR in its log) can be checked in Azahar (`tools/azahar.sh`).
- A refused frame must undo what it queued (atlas stamps), or cells keep stale content; a cache keyed by packed fields
  must give each field enough bits (sprY needed 16: sheet 0 is the whole tileset as one column).
- The bottom screen is the citro2d UI now; the libctru text console is only a fallback and must never print past row 29
  or the 40th column (it scrolls).
- Linear memory is the scarce pool. `__ctru_linear_heap_size` is 5 MB now (`main.cpp`), heap size 0 (automatic: the
  Old 3DS cannot give a fixed 52 MB). Atlases (1.5 MB) + two vertex buffers (0.65 MB) + staging 0.25 MB + C3D buffers.
- The engine's fps counter in the dev tools counts passes of the loop, not drawn frames; `skipped` says how many passes
  skipped drawing.
- Old 3DS vs "2DS": the owner's `.141` console is an Old 2DS (Old 3DS CPU). New 3DS numbers would differ a lot.
