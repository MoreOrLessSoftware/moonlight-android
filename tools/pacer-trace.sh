#!/usr/bin/env bash
# Records and analyses frame pacing traces from the Vulkan renderer over adb.
#
#   tools/pacer-trace.sh session         switch tracing on, wait while you stream, then pull the
#                                        trace, analyse it and switch tracing off
#   tools/pacer-trace.sh on | off        switch tracing on or off for streams started afterwards
#   tools/pacer-trace.sh live            follow the pacer's 10 second summaries in logcat
#   tools/pacer-trace.sh pull [--clear]  copy traces off the device (and delete them there)
#   tools/pacer-trace.sh analyze [file...]
#                                        analyse traces (the newest pulled one by default), and
#                                        replay them through the pacer as it is in this checkout
#
# Environment: MOONLIGHT_PACKAGE (default com.limelight.mlsoft.debug), ADB, CXX (default g++),
# PACER_TRACE_DIR (default ./pacer-traces).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG="${MOONLIGHT_PACKAGE:-com.limelight.mlsoft.debug}"
OUT="${PACER_TRACE_DIR:-$ROOT/pacer-traces}"
REMOTE="/sdcard/Android/data/$PKG/files/pacer-traces"
SRC="$ROOT/app/src/main/jni/vulkan"
PROPERTY="debug.moonlight.pacer_trace"

if [ -n "${ADB:-}" ]; then
    :
elif command -v adb >/dev/null 2>&1; then
    ADB=adb
elif [ -n "${LOCALAPPDATA:-}" ] && [ -x "$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe" ]; then
    ADB="$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe"
elif [ -n "${ANDROID_HOME:-}" ] && [ -x "$ANDROID_HOME/platform-tools/adb" ]; then
    ADB="$ANDROID_HOME/platform-tools/adb"
else
    echo "adb not found; set ADB to its path" >&2
    exit 1
fi

# Git Bash rewrites arguments that look like Unix paths (/sdcard/...) into Windows paths
# before handing them to native programs like adb.exe. Device paths must reach adb untouched.
# (Not switched off for the whole script: the MSYS2 compiler needs its paths converted.)
adb_() {
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$ADB" "$@"
}

# A local path in the form adb takes, which on Windows is a Windows path
local_path() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else echo "$1"; fi
}

replay_tool() {
    local bin="$OUT/.bin/pacer_replay"
    mkdir -p "$OUT/.bin"
    # Rebuilt whenever the pacer changes, so the replay always uses this checkout's code
    if [ ! -x "$bin" ] && [ ! -x "$bin.exe" ] || \
       [ -n "$(find "$SRC/frame_pacer.cpp" "$SRC/frame_pacer.h" "$SRC/tests/pacer_replay.cpp" -newer "$OUT/.bin/built" 2>/dev/null)" ]; then
        echo "Building the replay tool..." >&2
        "${CXX:-g++}" -std=c++17 -O2 -I"$SRC" "$SRC/frame_pacer.cpp" "$SRC/tests/pacer_replay.cpp" -o "$bin"
        touch "$OUT/.bin/built"
    fi
    if [ -x "$bin.exe" ]; then echo "$bin.exe"; else echo "$bin"; fi
}

cmd_on() {
    adb_ shell setprop "$PROPERTY" 1
    echo "Pacer tracing on. Streams started from now on (with the Vulkan renderer) are recorded."
}

cmd_off() {
    adb_ shell setprop "$PROPERTY" 0
    echo "Pacer tracing off."
}

cmd_live() {
    adb_ logcat -v time -s VulkanPacer:I VulkanRenderer:I
}

cmd_pull() {
    mkdir -p "$OUT"
    if ! adb_ shell ls "$REMOTE" >/dev/null 2>&1; then
        echo "No traces on the device yet ($REMOTE)" >&2
        echo "  $PROPERTY is now '$(adb_ shell getprop "$PROPERTY" | tr -d '\r')'. Tracing starts with the" >&2
        echo "  next stream after it's set to 1, and only with the Vulkan renderer. A recording" >&2
        echo "  stream logs 'Recording pacer trace to ...' under the VulkanPacer tag." >&2
        return 1
    fi
    # MSYS would otherwise rewrite the device path into a Windows one
    # The local side, unlike the device side, has to be a path adb.exe understands
    adb_ pull "$REMOTE/." "$(local_path "$OUT")"
    if [ "${1:-}" = "--clear" ]; then
        adb_ shell "rm -f $REMOTE/*.csv"
    fi
    echo "Traces are in $OUT"
}

cmd_analyze() {
    local files=("$@")
    if [ ${#files[@]} -eq 0 ]; then
        local newest
        newest="$(ls -t "$OUT"/pacer-*.csv 2>/dev/null | head -n 1 || true)"
        if [ -z "$newest" ]; then
            echo "No traces in $OUT; run '$0 pull' first" >&2
            return 1
        fi
        files=("$newest")
    fi
    local tool
    tool="$(replay_tool)"
    for f in "${files[@]}"; do
        echo "=== $f"
        "$tool" "$f" --events "${PACER_EVENTS:-25}"
        echo
    done
}

cmd_session() {
    adb_ get-state >/dev/null
    cmd_on
    echo
    echo "Start a stream on the device now (Video renderer: Vulkan). The pacer's summaries"
    echo "follow below every 10 seconds. End the stream, then press Enter here."
    echo
    adb_ logcat -c || true
    adb_ logcat -v time -s VulkanPacer:I &
    local logcat=$!
    read -r _
    kill "$logcat" 2>/dev/null || true
    cmd_off
    cmd_pull --clear
    cmd_analyze
}

case "${1:-}" in
    on) cmd_on ;;
    off) cmd_off ;;
    live) cmd_live ;;
    pull) shift; cmd_pull "$@" ;;
    analyze) shift; cmd_analyze "$@" ;;
    session) cmd_session ;;
    *) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
