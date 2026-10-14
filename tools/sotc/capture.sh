#!/usr/bin/env bash
# Run CUSA08809 and dump the presented frame at the given seconds (default 15 30 45 60 90).
# SOTC_PAD="40:cross 41: 50:down,cross 51:" holds the listed buttons from that second on
# (comma-separated BB_PAD_FILE tokens; empty releases); "90:frame" records the frame analyzer
# (vk_frame_capture) at that second. Output: out/sotc-dump/<time>/ and
# out/sotc-run.log. SOTC_TIMEOUT ends the run (default 100).
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
dump=$PWD/out/sotc-dump/$(date +%Y%m%d-%H%M%S); trigger=$dump/.trigger; pad=$dump/pad.txt
mkdir -p "$dump"
[[ -n ${SOTC_PAD:-} ]] && { : > "$pad"; export BB_PAD_FILE=$pad; }
export BB_DUMP_DIR=$dump BB_FRAME_DUMP_TRIGGER=$trigger BB_CAPTURE_TRIGGER=$dump/.analyze BB_CAPTURE_DIR=$dump SOTC_SKIP_PREPARE=${SOTC_SKIP_PREPARE:-1}
timeout "${SOTC_TIMEOUT:-100}" python tools/sotc/run_sotc.py > out/sotc-run.log 2>&1 &
game=$!
start=$SECONDS
events=()
for at in ${*:-15 30 45 60 90}; do events+=("$at:dump"); done
for entry in ${SOTC_PAD:-}; do events+=("$entry"); done
while IFS= read -r event; do
    at=${event%%:*} what=${event#*:}
    while (( SECONDS - start < at )); do sleep 0.2; kill -0 $game 2>/dev/null || break 2; done
    if [[ $what == frame ]]; then : > "$dump/.analyze"; continue; fi
    if [[ $what == dump ]]; then
        : > "$trigger"
        # Desktop screenshots only on request: they capture whatever is on the screen.
        [[ ${SOTC_SCREENSHOT:-0} == 1 ]] && powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/sotc/screenshot.ps1 "$(cygpath -w "$dump")/screen_$at.png" &
    else echo "${what//,/ }" > "$pad"; fi
done < <(printf '%s\n' "${events[@]}" | sort -t: -k1,1n)
wait $game
echo "exit=$?"
shopt -s nullglob
raws=("$dump"/*.raw)
(( ${#raws[@]} )) && python tools/sotc/raw2png.py "${raws[@]}"
echo "$dump"
