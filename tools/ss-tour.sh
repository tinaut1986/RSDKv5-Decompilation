#!/bin/bash
# Takes the console through Special Stages 4, 4+, 7, 1+, 5, 6, 7+ (~45 s each, the countdown held at 50+ by SET ufotime)
# and prints the fps and why the floor / rotozoom textures are not held every 5 s. Without a player the stage ends after
# ~25 s and the game goes back to the title: the later samples are the title. tools/ss-tour.sh [-i IP]
cd "$(dirname "$0")/.."
IP=$(cat .3ds_ftp_ip 2>/dev/null); [ "${1:-}" = "-i" ] && IP=$2
S="tools/site.sh -i $IP"
L="ftp://$IP:5000/3ds/Sonic%20Mania%203DS/debug/log.txt"
$S "SET ufotime 1" >/dev/null
for p in "3 3 SS4" "3 10 SS4+" "3 6 SS7" "3 7 SS1+" "3 4 SS5" "3 5 SS6" "3 13 SS7+"; do
  set -- $p
  n0=$(curl -s --max-time 8 "$L" | grep -c "storage:")
  $S "SCENE $1 $2" >/dev/null
  timeout 150 bash -c "until [ \$(curl -s --max-time 8 '$L' | grep -c 'storage:') -gt $n0 ]; do sleep 4; done"
  $S "SET ufotime 1" >/dev/null
  for k in 1 2 3 4 5 6 7 8 9; do
    sleep 5
    echo "$3 +$((k*5))s: $($S STATE | grep -o 'fps [0-9.]*\|last second [0-9/]*) why [^|]*' | tr '\n' ' ') | $($S REFUSED | grep -o 'roto texture [^,]*, floor [^,]*' | cut -c1-90)"
  done
done
echo DONE
