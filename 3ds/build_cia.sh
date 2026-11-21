#!/bin/bash

if [ -z "$1" ]; then
    echo "Missing path to elf file."
    echo "Usage: $0 path/to/file.elf"
    exit 1
fi

APP_NAME="Sonic Mania"
APP_AUTH="tinaut1986, SaturnSH2x2, Rubberduckycooly"

# The committed bannertool and makerom are Linux programs (the release workflow's). On Windows: devkitPro's
# bannertool.exe, and the fixed makerom of ../tools/fetch-makerom.sh (devkitPro's makerom.exe drops the minor version).
BANNERTOOL=./bannertool
MAKEROM=./makerom
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        BANNERTOOL="${DEVKITPRO:-/opt/devkitpro}/tools/bin/bannertool.exe"
        MAKEROM=$(sh ../tools/fetch-makerom.sh) || exit 1
        ;;
esac

mkdir -p out
rm -rf out/*

# make banner
"$BANNERTOOL" makebanner -i ./banner.png -a ./jingle.wav -o out/banner.bnr

# The version (build_3ds.sh exports it): in the SMDH's long description and as the title's version (major.minor.micro,
# 6/6/4 bits), so FBI and the HOME menu's data management show it.
LABEL="${MANIA3DS_VERSION_LABEL:-}"
V="${MANIA3DS_BUILD_VERSION:-0.0.0}"
NUM=$(echo "$V" | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+' || echo 0.0.0)
MAJOR=$(echo "$NUM" | cut -d. -f1) MINOR=$(echo "$NUM" | cut -d. -f2) MICRO=$(echo "$NUM" | cut -d. -f3)

# make SMDH
"$BANNERTOOL" makesmdh -s "$APP_NAME" -l "$APP_NAME${LABEL:+ v$LABEL}" -p "$APP_AUTH" -i ./48x48.png -o out/icon.icn

# build CIA
"$MAKEROM" -f cia -o out/SonicMania.cia -DAPP_ENCRYPTED=false -elf "$1" -rsf ./SonicMania.rsf -exefslogo -target t -icon out/icon.icn -banner out/banner.bnr \
    -major "$MAJOR" -minor "$MINOR" -micro "$MICRO"

echo "Built .cia"
