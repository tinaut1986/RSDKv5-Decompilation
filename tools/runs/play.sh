#!/bin/bash
# tools/runs/play.sh IP label [passes]: replays the console's debug/run.bin (RUNPLAY), samples SITE STATE every 3 s,
# prints the averages and the run's result (real drawn fps). Set frameskip 0, force3d, ufotime, ufonofall first.
I=$1; L=$2
tools/site.sh -i $I "RUNPLAY ${3:-0}" >/dev/null
timeout 60 bash -c "until tools/site.sh -i $I RUN | grep -q '^200 playing'; do sleep 0.5; done"
sleep 2
while tools/site.sh -i $I RUN | grep -q '^200 playing'; do
  tools/site.sh -i $I STATE | python3 -c "
import re,sys
s=sys.stdin.read()
def g(p):
    m=re.search(p,s); return m.group(1) if m else 'nan'
print(g(r' frame ([0-9.]+)'),g(r'upd ([0-9.]+)'),g(r'draw ([0-9.]+)'),g(r'flip ([0-9.]+)'),g(r'pcopy ([0-9.]+)'),g(r'pxfer ([0-9.]+)'),g(r'pframe ([0-9.]+)'),g(r'roto ([0-9]+) us'),g(r'gpu [0-9]+ quads ([0-9]+)'),g(r'layers ([0-9.]+) sprites'),g(r'3d ([0-9.]+)'),g(r'faces ([0-9.]+) 3d'))"
  sleep 3
done > /tmp/play-samples.txt
awk -v L="$L" '{for(i=1;i<=NF;i++){s[i]+=$i}; n++} END{printf "%-18s frame %5.1f upd %4.1f draw %4.1f flip %4.1f pcopy %4.1f pxfer %4.1f pframe %4.1f roto %5.0f us quads %5.0f | rec: layers %4.1f 3d %4.1f faces %4.1f (%d samples)\n",L,s[1]/n,s[2]/n,s[3]/n,s[4]/n,s[5]/n,s[6]/n,s[7]/n,s[8]/n,s[9]/n,s[10]/n,s[11]/n,s[12]/n,n}' /tmp/play-samples.txt
tools/site.sh -i $I RUN
