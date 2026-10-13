#!/usr/bin/env bash
# Run CUSA08809 and dump the presented frame at the given seconds (default 15 30 45 60 90).
# Output: out/sotc-dump/*.raw|png and out/sotc-run.log. SOTC_TIMEOUT ends the run (default 100).
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
dump=$PWD/out/sotc-dump/$(date +%Y%m%d-%H%M%S); trigger=$dump/.trigger
mkdir -p "$dump"
export BB_DUMP_DIR=$dump BB_FRAME_DUMP_TRIGGER=$trigger SOTC_SKIP_PREPARE=${SOTC_SKIP_PREPARE:-1}
timeout "${SOTC_TIMEOUT:-100}" python tools/sotc/run_sotc.py > out/sotc-run.log 2>&1 &
game=$!
start=$SECONDS
for at in ${*:-15 30 45 60 90}; do
    while (( SECONDS - start < at )); do sleep 1; kill -0 $game 2>/dev/null || break 2; done
    : > "$trigger"
done
wait $game
echo "exit=$?"
python tools/sotc/raw2png.py "$dump"/*.raw
