#!/bin/bash
# Loads the game's scenes one after another on the console (over the game's FTP) and keeps each load's storage line
# (pool use, defragmentations, refused allocations) and the game's state. Stops when the game stops answering.
#   tools/scene-sweep.sh [-i IP] [name] [N]   -> debug/pool-sweep/<name>.txt (N: only the first N Mania scenes)
# Mania and Encore, the Special Stages, a few Blue Spheres, Pinball, Extras, Presentation; ~45 min on the 2DS.
# Turn on SET heapcheck 1 first to catch a heap overflow at the scene that causes it.
cd "$(dirname "$0")/.."
IP=$(cat .3ds_ftp_ip 2>/dev/null); [ "${1:-}" = "-i" ] && { IP=$2; shift 2; }
S="tools/site.sh -i $IP"
LOGURL="ftp://$IP:5000/3ds/Sonic%20Mania%203DS/debug/log.txt"
mkdir -p debug/pool-sweep
OUT=debug/pool-sweep/${1:-pools}.txt
: > "$OUT"
LIST=""
for i in $(seq 0 28); do LIST="$LIST 1:$i"; done
[ -n "${2:-}" ] && LIST=$(echo $LIST | cut -d' ' -f1-$2) # the first N scenes only
for i in $(seq 0 26); do [ -z "${2:-}" ] && LIST="$LIST 2:$i"; done
for i in $(seq 0 13); do LIST="$LIST 3:$i"; done
for i in 0 10 20 33; do LIST="$LIST 4:$i"; done
LIST="$LIST 5:0 6:0 6:1 6:2"
for i in $(seq 1 8); do LIST="$LIST 0:$i"; done
count() { curl -s --max-time 20 "$LOGURL" | grep -c "storage:"; }
for cs in $LIST; do
  c=${cs%:*}; i=${cs#*:}
  n0=$(count)
  $S "SCENE $c $i" > /dev/null 2>&1
  ok=0
  for t in $(seq 1 30); do
    sleep 5
    n=$(count)
    [ -n "$n" ] && [ "$n" -gt "${n0:-0}" ] && { ok=1; break; }
  done
  sleep 8
  line=$(curl -s --max-time 20 "$LOGURL" | grep "load:\|storage:" | tail -2 | tr '\n' ' ')
  state=$($S STATE 2>/dev/null | grep -o "category [0-9]* scene [0-9]*\|heap used [0-9]* KB")
  echo "$c $i ok=$ok | $(echo $state) | $line" >> "$OUT"
  [ -z "$state" ] && { echo "NO ANSWER after $c $i" >> "$OUT"; break; }
done
echo DONE >> "$OUT"
