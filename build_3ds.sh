#!/bin/bash
# Builds the 3DS port: RSDKv5U.elf/.3dsx and, with --cia, 3ds/out/SonicMania.cia.
# Needs devkitPro (devkitARM, libctru). libtheora for 3DS is taken from
# .deps/ when the portlibs lack it (see docs/PLAN.md, B0.1).
set -euo pipefail
cd "$(dirname "$0")"
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}

L="$PWD/.deps/opt/devkitpro/portlibs/3ds"
LIBDIR="$DEVKITPRO/portlibs/3ds/lib/pkgconfig:$DEVKITPRO/portlibs/armv6k/lib/pkgconfig"
[ -d "$L/lib/pkgconfig" ] && LIBDIR="$L/lib/pkgconfig:$LIBDIR"

# The Makefile does not add these by itself.
export DEFINES="-DRETRO_USE_MOD_LOADER=0 -I./Game -I./Game/Objects ${EXTRA_DEFINES:-}"
make PLATFORM=3DS PKGCONFIG="env PKG_CONFIG_LIBDIR=$LIBDIR pkg-config --static" -j"$(nproc)"

if [ "${1:-}" = "--cia" ]; then
  (cd 3ds && ./build_cia.sh ../bin/3DS/CTR/RSDKv5U.elf)
  echo "CIA: 3ds/out/SonicMania.cia"
fi
