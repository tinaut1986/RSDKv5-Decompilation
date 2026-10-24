#!/bin/bash
# Sends one SITE command to the game's FTP server and prints its reply.
#   tools/site.sh [-i IP] STATE | SCENES | "SCENE 1 3" | "INPUT 10" | "INSTALL /path.cia"
# IP defaults to .3ds_ftp_ip (the last console build_3ds.sh --install used).
cd "$(dirname "$0")/.."
IP=$(cat .3ds_ftp_ip 2>/dev/null)
[ "${1:-}" = "-i" ] && { IP="$2"; shift 2; }
[ -n "$IP" ] || { echo "no console IP (pass -i IP)" >&2; exit 1; }
curl -sv --max-time "${SITE_TIMEOUT:-8}" -Q "SITE $*" "ftp://$IP:5000/" -o /dev/null 2>&1 \
  | awk '/^> SITE/{f=1;next} f&&/^< [0-9]/{sub(/^< /,"");print;exit}'
