# Videos and full-screen images

The game shows nine Theora videos and one still image. The original 3DS port had video and image playback stubbed out
(`LoadVideo` returned false at once), so the game skipped all of them without a word.

| File | Where | Size |
|---|---|---|
| `Data/Video/Mania.ogv` | The intro (TitleSetup, after the title screen waits; music `IntroHP.ogg` 77.0 s, or `IntroTee.ogg` 77.7 s with the video 1.8 s in) | 1024x512, 23.98 fps, 1781 frames: 74.3 s, 30 MB |
| `BadEnd.ogv`, `MREnd.ogv` | An ending's first clip, from the start of its music (`BadEnd.ogg`, 25.0 s) | 1024x512, 30 fps, 419 frames: 14.0 s |
| `BadSonic`, `BadSonic2`, `BadTails`, `BadKnux`, `BadMighty`, `BadRay.ogv` | An ending's second clip, the character's, 14 s into the music | 1024x512, 30 fps, 316-319 frames: 10.5-10.6 s |
| `Data/Images/TrueEnd.png` | The true ending (UIVideo, `LoadImage`) | 1024x512 |

An ending's UIVideo plays `videoFile1` with the music and then `videoFile2` with a start delay of 14 s on the music's
clock. The names (and their order) were found in the scene files (UTF-16 strings of the UIVideo attributes), not
guessed.

## How it plays (Graphics/Video.cpp, Graphics/CTR/VideoFrame.cpp)

- **The converted copy first.** `LoadVideo` opens `sdmc:/3ds/Sonic Mania 3DS/Video/<name>` and falls back to the
  pack's. A video read from the pack uses a 64 KB stdio buffer (`OpenDataFile`): the engine's buffer the size of the
  file would be 30 MB for the intro.
- **A decoder thread** on core 0, one priority below the main thread, decodes ahead into a queue (VideoFrame has 4
  slots of 400x240 RGB565: the one on the screen and up to 3 queued). It runs while the main thread sleeps until its
  next tick (the main thread used ~2 ms of each 16.7 ms pass during a video). Decoding on the main thread, a frame
  when its time came, made every slow frame late the next ones and the pacing threw passes away (only 56 % of the
  intro's frames were shown).
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

```sh
python3 tools/convert-videos.py path/to/Data.rsdk out/Video   # then copy out/Video/* to sdmc:/3ds/Sonic Mania 3DS/Video/
```

The tool reads the videos out of the player's own `Data.rsdk` (decrypting them as `Reader.cpp` does) and re-encodes them
with ffmpeg (libtheora) at 400x200, quality 4, a keyframe every 240 frames: 48.8 MB -> 4.7 MB. Each packet becomes one
frame at the stream's own rate (`setpts=N/rate`, `-fps_mode cfr`): ffmpeg would otherwise follow the granule jumps and
add the 7 s hold and repeated frames. Each converted video keeps the original's packet count (checked with ffprobe).
The game's data is never committed or shipped.

## Measurements (Old 2DS, dev build, `debug/log.txt` has a line per video and a timeline every 5 s)

| Video | Decode a frame (avg / max) | Shown |
|---|---|---|
| Intro, pack 1024x512 | 59 ms / 224 ms | 167 frames (keyframes and a few after), in sync |
| Intro, 400x200 quality 6 (granule timing, 81.8 s) | 14.4 ms / 44 ms | 1540 of 1817 (85 %) |
| Intro, 400x200 quality 4 (granule timing, 81.8 s) | 15.2 ms / 43 ms (thread, wall time) | 1727 of 1776 (97 %) |
| **Intro, 400x200 quality 4, a frame a packet** | 15.2 ms / 42 ms | **1733 of 1769 (98 %), ends with the music at 76.11 s** |
| BadEnd + BadSonic, quality 4, a frame a packet | 15 ms | 410 of 419, 197 of 206; end with the music at 14.01 and 24.61 s (of 25.0) |
| MREnd + BadSonic2, quality 4 | 15.3 / 13.5 ms | 410 of 420, 195 of 205 |

On the PC (ffmpeg, one thread) quality 4 decodes in 75 % of quality 6's time and quality 3 in 62 %; a keyframe every
240 frames saves another ~6 %.

## Open

- Converting on the console (the owner's question): decoding the pack's intro there takes ~2 min on an Old 3DS and
  encoding Theora again several times more (an estimate: 10-20 min). Not done.
- Integrating the converted videos in `Data.rsdk` instead of `Video/` (a rebuilt pack, ~44 MB smaller). Not done.
- Faster decoding: devkitPro's libtheora is built without its ARM assembly.
