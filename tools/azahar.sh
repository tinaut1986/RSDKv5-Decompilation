#!/bin/bash
# Runs the current build (bin/3DS/CTR/RSDKv5U.3dsx) in the owner's Azahar (Flatpak) and waits until the game's dev
# tools answer on 127.0.0.1:5000, so tools/site.sh -i 127.0.0.1 and tools/shot.sh -i 127.0.0.1 work as with the console.
#   tools/azahar.sh            copy the build to Azahar's SD card, (re)start the emulator on it
#   tools/azahar.sh --stop     stop the emulator
# Needs Data.rsdk in Azahar's sdmc/3ds/Sonic Mania 3DS/ (copied once from the Steam install).
# Azahar must use Vulkan: OpenGL fails when it is started from a non-interactive shell ("shared OpenGL contexts are
# not supported"). The owner's qt-config.ini was switched to graphics_api=2 (backup: qt-config.ini.claude-backup).
# Timings mean nothing in the emulator, and it does not reproduce the stencil (masked inks): use it for crashes, hangs
# (libctru panics show in its log) and pictures.
cd "$(dirname "$0")/.."
A="$HOME/.var/app/org.azahar_emu.Azahar"
E="$A/data/azahar-emu/sdmc/3ds/Sonic Mania 3DS"
stop() {
    # By PID and process name: `pkill -f azahar` would also match (and kill) this script's own command line.
    for p in $(ps -eo pid,comm | awk '$2=="azahar"{print $1}'); do kill -9 "$p"; done
    sleep 2
    for p in $(ps -eo pid,comm | awk '$2=="zenity"{print $1}'); do kill "$p"; done # the launcher's crash dialog
}
stop
[ "${1:-}" = "--stop" ] && exit 0
mkdir -p "$E"
[ -f "$E/Data.rsdk" ] || echo "warning: no Data.rsdk in $E" >&2
cp bin/3DS/CTR/RSDKv5U.3dsx "$E/"
nohup flatpak run org.azahar_emu.Azahar -w "$E/RSDKv5U.3dsx" >/dev/null 2>&1 &
timeout 90 bash -c 'until tools/site.sh -i 127.0.0.1 STATE 2>/dev/null | grep -q category; do sleep 2; done' && echo "Azahar up: tools/site.sh -i 127.0.0.1 ..." || echo "the game did not answer (see $A/data/azahar-emu/log/azahar_log.txt)"
