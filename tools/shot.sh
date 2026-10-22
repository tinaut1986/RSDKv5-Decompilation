#!/bin/bash
# Takes a snapshot of a console screen: tools/shot.sh [-i IP] [top|bottom] [out.png]
cd "$(dirname "$0")/.."
ARGS=(); [ "${1:-}" = "-i" ] && { ARGS=(-i "$2"); shift 2; }
IP=${ARGS[1]:-$(cat .3ds_ftp_ip)}
WHICH=top; case "${1:-}" in top|bottom) WHICH=$1; shift;; esac
OUT=${1:-/tmp/mania-shot-$WHICH.png}
tools/site.sh "${ARGS[@]}" SHOT $WHICH >/dev/null; sleep 1.5
D="ftp://$IP:5000/3ds/Sonic%20Mania%203DS/debug/"
LAST=$(curl -s --max-time 8 "$D" | awk '{print $NF}' | grep '^shot-' | sort | tail -1)
curl -s --max-time 20 -o /tmp/mania-shot.raw "$D$LAST"
python3 - "$OUT" <<'PY'
import struct, sys
from PIL import Image
d=open('/tmp/mania-shot.raw','rb').read()
w,h,bpp=struct.unpack('<III',d[:12]); px=d[12:]
img=Image.new('RGB',(h,w))   # the framebuffer is column-major: w=240 rows per column, h=400 columns
o=img.load()
for x in range(h):            # screen column
    for y in range(w):        # fb index within the column counts up from the screen's bottom
        i=(x*w+y)*bpp
        if bpp==2:
            v=px[i]|(px[i+1]<<8); r=(v>>11)&31; g=(v>>5)&63; b=v&31
            c=(r*255//31,g*255//63,b*255//31)
        else:
            c=(px[i+2],px[i+1],px[i])
        o[x,w-1-y]=c
img.save(sys.argv[1]); print(sys.argv[1], img.size)
PY
