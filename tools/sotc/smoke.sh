#!/usr/bin/env bash
# One run that checks the port end to end: boot, skip the intro from the pause menu, the
# title screen and the main menu (~2 minutes). Frames at 30/60/90/110 s, frame and readback
# statistics, GPU time per pass and dispatch, the Ngs2 parameters the game sends and the audio
# it outputs (BB_AUDIO_DUMP),
# summed up by smoke_report.py. Usage: tools/sotc/smoke.sh [seconds, default 120]
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
seconds=${1:-120}
export BB_FRAME_STATS=1 BB_READ_FAULT_STATS=1 BB_GPU_PROFILE=1 BB_BUFFER_STATS=${BB_BUFFER_STATS:-0}
audio=$PWD/out/smoke-audio
rm -rf -- "$audio"; mkdir -p "$audio"
# Windows paths: the probe and Python read these, not MSYS.
audio_w=$(cygpath -w "$audio" 2>/dev/null || echo "$audio")
export BB_AUDIO_DUMP="$audio_w/out"
SOTC_PAD="50:tap=options 53:tap=up 55:tap=cross 58:tap=cross 72:tap=cross 78:tap=cross" \
SOTC_TIMEOUT=$seconds bash tools/sotc/capture.sh 30 60 90 110 > out/smoke-capture.txt 2>&1
dump=$(tail -1 out/smoke-capture.txt)
python tools/sotc/sheet.py "$dump" 4 > /dev/null 2>&1
python tools/sotc/smoke_report.py out/sotc-run.log "$audio_w" "$(cygpath -w "$dump" 2>/dev/null || echo "$dump")" | tee out/smoke-report.txt
# The dumps are kept (out/smoke-audio) for listening; the next run clears them.
