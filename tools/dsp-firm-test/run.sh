#!/bin/bash
# Host test of the DSP firmware validator (RSDKv5/RSDK/Dev/CTR/DspFirm.c): a synthetic firmware with real SHA-256
# hashes, then damaged copies. No game data, no console. Usage: tools/dsp-firm-test/run.sh [path/to/dspfirm.cdc]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-/tmp/sm-dsp-firm-test}
mkdir -p "$WORK"
gcc -O1 -g -Wall -Wextra -Werror -I"$ROOT/RSDKv5" "$ROOT/RSDKv5/RSDK/Dev/CTR/DspFirm.c" "$ROOT/tools/dsp-firm-test/dsp_firm_test.c" \
    -o "$WORK/dsp_firm_test"
"$WORK/dsp_firm_test" "${1:-}"
