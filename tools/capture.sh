#!/bin/bash
# Fetches a capture the owner took on the console (DEBUG tab, CAPTURE) (debug/capture-NNN-*) and converts its pictures:
# tools/capture.sh [-i IP] [N] [outdir]   (N: the capture's number; the latest when left out)
# Writes outdir/capture-NNN-{top,right,prev-top,prev-right}.png, -info.txt and -cmds.txt (default outdir: debug/captures).
cd "$(dirname "$0")/.."
ARGS=(); [ "${1:-}" = "-i" ] && { ARGS=(-i "$2"); shift 2; }
IP=${ARGS[1]:-$(cat .3ds_ftp_ip)}
D="ftp://$IP:5000/3ds/Sonic%20Mania%203DS/debug/"
N=${1:-$(curl -s --max-time 20 "$D" | grep -o 'capture-[0-9]*-info.txt' | sort | tail -1 | grep -o '[0-9][0-9]*')}
[ -z "$N" ] && { echo "no capture on the console"; exit 1; }
N=$(printf %03d "$((10#$N))")
OUT=${2:-debug/captures}
mkdir -p "$OUT"
for part in info.txt cmds.txt top.raw right.raw prev-top.raw prev-right.raw; do
  curl -s --max-time 30 -o "$OUT/capture-$N-$part" "${D}capture-$N-$part" || rm -f "$OUT/capture-$N-$part"
done
python3 - "$OUT" "$N" <<'PY'
import os, struct, sys
from PIL import Image
out, n = sys.argv[1], sys.argv[2]
for part in ('top', 'right', 'prev-top', 'prev-right'):
    raw = f'{out}/capture-{n}-{part}.raw'
    if not os.path.exists(raw) or os.path.getsize(raw) < 12:
        continue
    d = open(raw, 'rb').read()
    w, h, bpp = struct.unpack('<III', d[:12]); px = d[12:]
    img = Image.new('RGB', (h, w)); o = img.load()   # column-major framebuffer, columns counted from the screen's bottom
    for x in range(h):
        for y in range(w):
            i = (x * w + y) * bpp
            if bpp == 2:
                v = px[i] | (px[i + 1] << 8)
                c = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
            else:
                c = (px[i + 2], px[i + 1], px[i])
            o[x, w - 1 - y] = c
    img.save(f'{out}/capture-{n}-{part}.png'); os.remove(raw)
    print(f'{out}/capture-{n}-{part}.png')
PY
cat "$OUT/capture-$N-info.txt" 2>/dev/null
