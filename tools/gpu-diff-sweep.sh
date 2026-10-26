#!/bin/bash
# Draws the same frozen frame with the GPU and with the CPU renderer on the console and compares the two pictures: a
# difference the rounding of lines and inks does not explain is a bug of the GPU renderer (a missing sprite, tile or
# layer: a flipped DrawTile, a layer that does not wrap...). Needs the game running with the dev tools, on a console
# or in Azahar (`-i 127.0.0.1`), with Data.rsdk: it cannot run in CI.
#   tools/gpu-diff-sweep.sh [-i IP] [-s SAMPLES] [-t SECONDS] [scene ...]   scenes as category:index (default: the Mania Mode's)
# A sample = run right for SECONDS (default 6), freeze, shoot the GPU, shoot the CPU, back. Flagged samples (bad blocks)
# keep a side-by-side picture (GPU | CPU | difference) in debug/gpu-diff/; the table is debug/gpu-diff/report.txt.
cd "$(dirname "$0")/.."
IP=$(cat .3ds_ftp_ip 2>/dev/null); SAMPLES=3; SECS=6
while getopts "i:s:t:" o; do case $o in i) IP=$OPTARG;; s) SAMPLES=$OPTARG;; t) SECS=$OPTARG;; esac; done
shift $((OPTIND - 1))
S="tools/site.sh -i $IP"
LIST="$*"; [ -n "$LIST" ] || LIST=$(for i in $(seq 0 28); do echo -n "1:$i "; done)
LOGURL="ftp://$IP:5000/3ds/Sonic%20Mania%203DS/debug/log.txt"
OUT=debug/gpu-diff; mkdir -p $OUT; REPORT=$OUT/report.txt; : > $REPORT
loads() { curl -s --max-time 20 "$LOGURL" | grep -c "load:"; }
$S "SET frameskip 0" > /dev/null
for cs in $LIST; do
  c=${cs%:*}; i=${cs#*:}
  n0=$(loads); $S "SCENE $c $i" > /dev/null 2>&1
  for t in $(seq 1 30); do sleep 3; [ "$(loads)" -gt "${n0:-0}" ] && break; done
  name=$(curl -s --max-time 20 "$LOGURL" | grep "load:" | tail -1 | sed 's/.*load: \(.*\) in [0-9]* ms.*/\1/')
  [ -n "$($S STATE)" ] || { echo "NO ANSWER at $c:$i" | tee -a $REPORT; break; }
  sleep 4
  for s in $(seq 1 $SAMPLES); do
    $S "INPUT 10" > /dev/null; sleep $SECS
    $S "FREEZE 1" > /dev/null; sleep 1
    tools/shot.sh -i $IP top $OUT/gpu.png > /dev/null
    $S "GPU 0" > /dev/null; sleep 2
    tools/shot.sh -i $IP top $OUT/cpu.png > /dev/null
    $S "GPU 1" > /dev/null; $S "FREEZE 0" > /dev/null; $S "INPUT 0" > /dev/null
    read bad worst pct < <(python3 tools/gpu-diff.py $OUT/gpu.png $OUT/cpu.png $OUT/last.png)
    flag=""; [ "$bad" -gt 0 ] && { flag="  <-- DIFFERENT"; cp $OUT/last.png "$OUT/${c}-${i}-s${s}.png"; }
    echo "$c:$i $name sample $s: bad blocks $bad, worst block $worst%, pixels $pct%$flag" | tee -a $REPORT
  done
done
echo DONE >> $REPORT
