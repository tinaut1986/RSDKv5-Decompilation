#!/bin/bash
# Builds the 3DS port: RSDKv5U.elf/.3dsx and, with --cia, 3ds/out/SonicMania.cia.
# With --install [IP]: builds the CIA, uploads it to the running game's FTP server (port 5000) and
# installs it over the running build (SITE INSTALL); the game relaunches into it 10 s later.
# IP defaults to the last one used (.3ds_ftp_ip). The running game must be a dev-tools build.
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
# DEV_TOOLS=0 leaves the FTP server and the perf timing out (production builds).
export DEFINES="-DRETRO_USE_MOD_LOADER=0 -DMANIA_DEV_TOOLS=${DEV_TOOLS:-1} -I./Game -I./Game/Objects ${EXTRA_DEFINES:-}"
# make does not rebuild objects when only a -D changes, and some of them (SCREEN_XMAX) change the layout of
# structures the engine and the game logic share: a stamp of the defines wipes the objects when they differ.
STAMP="bin/.defines"
if [ "$(cat "$STAMP" 2>/dev/null)" != "$DEFINES" ]; then
  rm -rf bin
  mkdir -p bin
  printf '%s' "$DEFINES" > "$STAMP"
fi
make PLATFORM=3DS PKGCONFIG="env PKG_CONFIG_LIBDIR=$LIBDIR pkg-config --static" -j"$(nproc)"

if [ "${1:-}" = "--install" ]; then
  IP="${2:-$(cat .3ds_ftp_ip 2>/dev/null || true)}"
  [ -n "$IP" ] || { echo "usage: $0 --install <console IP>" >&2; exit 1; }
  (cd 3ds && ./build_cia.sh ../bin/3DS/CTR/RSDKv5U.elf)
  CIA=3ds/out/SonicMania.cia
  DIR="/3ds/Sonic Mania 3DS/update"
  URL="ftp://$IP:5000${DIR// /%20}/dev.cia"
  # ALLO announces the size (the bottom screen's progress bar); "-" runs SITE INSTALL after the upload.
  curl --fail --ftp-create-dirs -T "$CIA" "$URL" \
       -Q "+ALLO $(stat -c %s "$CIA")" -Q "-SITE INSTALL $DIR/dev.cia"
  echo "$IP" > .3ds_ftp_ip
  echo "Installed on $IP; the game relaunches in about 10 s."
elif [ "${1:-}" = "--cia" ]; then
  (cd 3ds && ./build_cia.sh ../bin/3DS/CTR/RSDKv5U.elf)
  echo "CIA: 3ds/out/SonicMania.cia"
fi
