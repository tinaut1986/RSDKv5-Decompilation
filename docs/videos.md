# Videos and full-screen images

The game shows ten Theora videos and one still image. The original 3DS port had video and image playback stubbed out
(`LoadVideo` returned false at once), so the game skipped all of them without a word.

| File | Where | Size |
|---|---|---|
| `Data/Video/Mania.ogv` | The intro (TitleSetup, after the title screen waits; music `IntroHP.ogg` 77.0 s, or `IntroTee.ogg` 77.7 s with the video 1.8 s in) | 1024x512, 23.98 fps, 1781 frames: 74.3 s, 30 MB |
| `BadEnd.ogv`, `MREnd.ogv` | An ending's first clip, from the start of its music (`BadEnd.ogg`, 25.0 s) | 1024x512, 30 fps, 419 frames: 14.0 s |
| `BadSonic`, `BadSonic2`, `BadTails`, `BadKnux`, `BadMighty`, `BadRay.ogv` | An ending's second clip, the character's, 14 s into the music | 1024x512, 30 fps, 316-319 frames: 10.5-10.6 s |
| `GoodEnd.ogv` | The good ending (scene `Ending/SceneG.bin`, music `GoodEnd.ogg` 42.0 s) | 1024x512, 30 fps, 1230 frames: 41.0 s, 7.4 MB |
| `Data/Images/TrueEnd.png` | The true ending, "True End?" (UIVideo, `LoadImage`: a still picture 32 s long with its music, fading in and out) | 1024x512 |

A bad ending's UIVideo plays `videoFile1` with the music and then `videoFile2` with a start delay of 14 s on the music's
clock. The names (and their order) were found in the scene files (UTF-16 strings of the UIVideo attributes), not
guessed. (The good ending's scene id is a single letter, `G`, which the first search, of two-letter ids, missed: it
played from the pack, a few frames a second, until the owner noticed.)

## How it plays (Graphics/Video.cpp, Graphics/CTR/VideoFrame.cpp)

- **The converted copy first.** `LoadVideo` opens `sdmc:/3ds/Sonic Mania 3DS/Video/<name>` and falls back to the
  pack's. A video read from the pack uses a 64 KB stdio buffer (`OpenDataFile`): the engine's buffer the size of the
  file would be 30 MB for the intro.
- **A decoder thread** on core 0, one priority below the main thread, decodes ahead into a queue (VideoFrame has 12
  slots of 400x240 RGB565, 2.3 MB: the one on the screen and up to 11 queued, half a second at 24 fps). It runs while
  the main thread sleeps until its next tick (the main thread used ~2 ms of each 16.7 ms pass during a video).
  Decoding on the main thread, a frame when its time came, made every slow frame late the next ones and the pacing
  threw passes away (only 56 % of the intro's frames were shown). With 4 slots the intro still stopped and jumped in
  its heaviest stretches, always the same ones (four times in a row around 21-23 s, at 53 s): frames that cost more
  to decode (25-31 ms) than they last, for about a second. Half a second ahead carries it through them.
- **The start.** The clock takes the music's position on its first pass with music (moved on by the real time, it ran
  ahead by the time the music took to load, 240 ms for an ending's). `LoadVideo` decodes a few frames before the
  thread starts (up to 6, or 120 ms): the thread gets little of the core in the next 0.1-0.2 s, while the main thread
  finishes the scene's frame. Y2R is opened there too (7 ms) and stays open between videos.
- **Reads of 64 KB.** The engine read the video 4 KB at a time; a read from the SD card costs ~10 ms however small, and
  ffmpeg's Ogg pages hold a second of video each, so the second frame waited for ~15 reads (120-185 ms) and an
  ending's second clip lost a dozen frames at its start. Nothing is written to the log while a video plays either.
- **Y2R** (the camera's YUV -> RGB hardware, service `y2r:u`) converts a 4:2:0 frame at the screen's width (the
  converted videos): the planes are copied without the decoder's borders (~0.5 ms) and the conversion runs while the
  thread decodes the next frame; a frame is queued once it is done. Anything else (the pack's 1024x512 videos) is
  scaled to fit on the CPU (~19 ms a frame). Y2R needs ~6 ms for 400x200 on a 2DS; the CPU path took 10-12 ms.
- **The clock** is the real time, pulled gently (5 % a pass) towards the music's position (`GetVideoStreamPos`, which on
  the 3DS is what the mixer plays, `CtrMusicPos`). The music's position moves in steps of a mixer buffer (23 ms, at
  times several at once): followed as it is, two 41.7 ms frames came due in one pass and one was lost (340 of the
  intro's). It jumps only when the two are more than 250 ms apart.
- **Frame times** are a count of packets, one frame each, as on the PC: libtheora counts packets and ignores the
  pages' granule positions (it needs `TH_DECCTL_SET_GRANPOS` after a seek), and the engine never sets them. The pack's
  videos have jumps in those granule positions (the intro's first frame "held" 7.2 s, five more of 1-4 frames): they
  are not in the game. Following them (and ffmpeg follows them when it converts) made the intro 81.8 s long instead of
  74.3, and the music ended first; with the count the intro ends 74.3 s into its music (76.1 s with `IntroTee`, which
  starts 1.8 s before the video), as on the PC. Repeated frames (empty packets, how a still stretch is coded) count too:
  `BadSonic` has 113 of its 319.
- **Far behind** (more than 0.5 s, the pack's 1024x512 videos on an Old 3DS): packets are dropped undecoded up to the
  next keyframe still ahead of the clock, so the picture keeps to the music in jumps. Before that the intro lasted
  132 s instead of 74.
- The screen is presented only when the picture changes (24-30 times a second, not 60), the still image again while it
  fades (`dimMax`).
- `audioState` was never set by the 3DS audio device, so `LoadVideo` ignored every start delay (the second ending clip
  started at once with its first 7 s skipped). The device sets it once its mixer runs.

## Converting

The pack's own videos still play when nothing is converted, a few frames a second on an Old 3DS: a line under the
picture says so (BottomUi::SetVideoNotice, on while a video wider than the screen plays), so the game does not look
broken. Converted videos are 400 wide, wherever they are read from, and play without it.

```sh
python3 tools/convert-videos.py path/to/Data.rsdk out/Video   # then copy out/Video/* to sdmc:/3ds/Sonic Mania 3DS/Video/
```

The tool reads the videos out of the player's own `Data.rsdk` (decrypting them as `Reader.cpp` does) and re-encodes them
with ffmpeg (libtheora) at 400x200, 500 kbit/s, a keyframe every 240 frames: 56.2 MB -> 11.7 MB. A constant bit rate,
not a constant quality: quality 4 (the first choice, 3.0 MB for the intro) looked blocky, and quality 6 or 7 made the
intro stop and jump on the Old 2DS in its heavy stretches (21-24 s, 53-55 s, 70 s). At 500 kbit/s the quiet stretches
(most of the videos) get ~3.5 times the bits quality 4 gave them, the heaviest half second about the same (79-92 KB at
450-520 kbit/s against 80 at quality 4). libtheora's two-pass mode and `-bufsize` did not flatten the heavy stretches. Each packet becomes one
frame at the stream's own rate (`setpts=N/rate`, `-fps_mode cfr`): ffmpeg would otherwise follow the granule jumps and
add the 7 s hold and repeated frames. Each converted video keeps the original's packet count (checked with ffprobe).
The game's data is never committed or shipped.

`--pack NEW.rsdk` writes a copy of the pack with the converted videos inside it instead of (or besides) the folder:
the videos stored unencrypted (the engine reads unencrypted entries as they are: the PC game's pack has 66 of them),
every other file copied as it was, still encrypted (the key comes from a file's path and size, not its place). For
the owner's pack: 208.4 MB -> 163.9 MB, the 1879 other entries identical (checked byte for byte). It goes on the SD
card as `Data.rsdk` in place of the original; with it, no `Video/` folder is needed.

## Measurements (Old 2DS, dev build, `debug/log.txt` has the lines of each video once it ends)

| Video | Decode a frame (avg / max) | Shown |
|---|---|---|
| Intro, pack 1024x512 | 59 ms / 224 ms | 167 frames (keyframes and a few after), in sync |
| Intro, 400x200 quality 6 (granule timing, 81.8 s) | 14.4 ms / 44 ms | 1540 of 1817 (85 %) |
| Intro, 400x200 quality 4 (granule timing, 81.8 s) | 15.2 ms / 43 ms (thread, wall time) | 1727 of 1776 (97 %) |
| **Intro, 400x200 quality 4, a frame a packet** | 15.2 ms / 42 ms | **1733 of 1769 (98 %), ends with the music at 76.11 s** |
| BadEnd + BadSonic, quality 4, a frame a packet | 15 ms | 410 of 419, 197 of 206; end with the music at 14.01 and 24.61 s (of 25.0) |
| MREnd + BadSonic2, quality 4 | 15.3 / 13.5 ms | 410 of 420, 195 of 205 |
| **Intro, 12 slots, prefill** | 15 ms | **1765 of 1769, none skipped or late; ends with the music at 74.30 s** |
| **BadEnd / MREnd, 12 slots, prefill** | 15 ms | **419 of 419** |
| BadKnux / BadMighty, 12 slots, prefill | 13 ms | 195 of 203, 197 of 205: 6-7 frames lost in the first 0.3-0.45 s |
| Intro, quality 6 / 7 | 17 / 18 ms | 1676 / 1463 of ~1780: stops at 22-23 s, 53-55 s, 70-71 s |
| **Intro, 500 kbit/s, 64 KB reads** | 16.7 ms / 55 ms | **1777 of 1780, none skipped or late; ends with the music at 74.30 s** |
| **BadEnd, 500 kbit/s** | 15 ms | **419 of 419**; 6 frames ready in 130 ms before the thread starts |
| **BadSonic / BadKnux, 500 kbit/s** | 13 ms | **none skipped**; 6 shown a little late in 0.28-0.41 s, 5 dropped |
| **GoodEnd, 500 kbit/s** | | **1213 of 1213**; ends with the music at 40.99 s (of 42.0) |

`debug/log.txt` also lists the stretches where a video fell behind ("behind:", with the slowest decode before each)
and how the start went (Y2R and the slots, the first frame, the frames ready before the thread, the first step's
read / decode / convert times, the thread's first steps).

On the PC (ffmpeg, one thread) quality 4 decodes in 75 % of quality 6's time and quality 3 in 62 %; a keyframe every
240 frames saves another ~6 %.

## Open

- Converting on the console was built (OPTIONS > VIDEOS: a question with the time, a progress screen with CANCEL) and
  taken out again (2026-11-26, the owner: PC only for now). devkitPro's `3ds-libtheora` has no encoder:
  `libtheoraenc.a` is a 2.8 KB stub (`encoder_disabled.o`) and `th_encode_alloc` always returns NULL. It would need
  libtheora built from Xiph's source with the encoder (and the release workflow building it too). Memory is tight as
  well: at the title screen the heap is 62.5 MB with 52.9 MB in use, and decoding the pack's 1024x512 intro takes
  4.8 MB of the 9.6 left. OPTIONS > VIDEOS now shows how many are converted (VideoFiles.cpp) and, tapped, says to
  convert the rest on a PC.
- Integrating the converted videos in `Data.rsdk` instead of `Video/` (a rebuilt pack, ~44 MB smaller). Not done.
- Faster decoding: devkitPro's libtheora is built without its ARM assembly.
- An ending's second clip (the character's) shows a few frames a little late at its start: it is due 14 s into the
  music, which is where the first clip ends. Decoding it ahead would need to know the next clip while the first plays
  (UIVideo's `videoFile2`).
