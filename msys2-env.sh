# Sourced by the Windows build/run scripts: puts the MSYS2 CLANG64 toolchain first on PATH.
# MSYS2_ROOT overrides the default C:/msys64.
msys2_root=${MSYS2_ROOT:-/c/msys64}
if [[ -d $msys2_root/clang64/bin ]]; then
    export PATH="$msys2_root/clang64/bin:$msys2_root/usr/bin:$PATH"
    export MSYSTEM=CLANG64
fi
# A non-login MSYS2 bash leaves TMP/TEMP unset; native tools (windres) then fail with
# "Unable to create temp file".
if [[ -z ${TEMP:-} ]] && command -v cygpath >/dev/null; then
    TEMP=$(cygpath -w "${TMPDIR:-/tmp}")
    export TEMP TMP=$TEMP
fi
