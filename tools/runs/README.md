# Recorded runs

Keys of every logic pass from a scene's full load (`SITE RUNREC N` / `RUNSTOP`), replayed with `SITE RUNPLAY [N] [F]`
(F: freeze the objects on the last pass, to compare pictures). The result line (`SITE RUN`) gives the real drawn fps.
A run recorded in Azahar replays on the 2DS to the same picture.

- `ss2-owner-30s.bin`: the owner's 30 s of Special Stage 2 (category 3, list 66), recorded 2026-10-23 in Azahar with
  `ufotime 1` and `ufonofall 1`. Replay with the same switches: it ends with 56 rings, Mach 2.

Upload to the console and measure:

```sh
curl -T tools/runs/ss2-owner-30s.bin "ftp://IP:5000/3ds/Sonic%20Mania%203DS/debug/run.bin"
tools/site.sh "SET frameskip 0"; tools/site.sh "SET force3d 1"; tools/site.sh "SET ufotime 1"; tools/site.sh "SET ufonofall 1"
tools/runs/play.sh IP "label"
```
