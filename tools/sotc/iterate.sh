#!/usr/bin/env bash
# Build bb-probe, run CUSA08809 for SOTC_TIMEOUT seconds (default 60) and show what happened
# after the game took over: its own log lines, runtime events and the stop reason.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
export PATH="/c/msys64/clang64/bin:/c/msys64/usr/bin:$PATH"
if ! bash build.sh > out/sotc-build.log 2>&1; then grep -E "error" out/sotc-build.log | head -20; exit 1; fi
export SOTC_TIMEOUT=${SOTC_TIMEOUT:-60} SOTC_SKIP_PREPARE=${SOTC_SKIP_PREPARE:-1}
# bb-probe --timeout is not ported to Windows: timeout(1) ends the run.
timeout "$SOTC_TIMEOUT" python tools/sotc/run_sotc.py "$@" > out/sotc-run.log 2>&1
echo "exit=$?"
grep -v -E "Warning|CollectImageFormatInfo|^Runtime: .*=0(,|$)" out/sotc-run.log |
    sed -n '/Entering original/,$p' | grep -v -E "^Runtime: (system module|mapped|direct allocation|reserved virtual)" |
    tail -n "${SOTC_TAIL:-45}"
