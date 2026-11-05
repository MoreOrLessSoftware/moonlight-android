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
#   tools/pacer-trace.sh presets         list the Quick Launch presets (debuggable builds only)
#   tools/pacer-trace.sh run <preset> [seconds] [--prop name=value]... [--keep-app]
#                                        unattended: start a stream from a Quick Launch preset
#                                        with tracing on, record for <seconds> (default 60), end
#                                        the stream and quit the host's app (so the next preset,
#                                        maybe at another frame rate, starts it afresh; not with
#                                        --keep-app), then pull and analyse the trace. --prop
#                                        sets a system property for this run only. A preset named
#                                        "<n> FPS" is checked against the stream's frame rate.
#                                        The screen must be unlocked.
#
# Environment: MOONLIGHT_PACKAGE (default com.limelight.mlsoft.debug), ADB, CXX (default g++),
# PACER_TRACE_DIR (default ./pacer-traces), RUN_QUIT_WAIT (seconds to wait after quitting the
# host's app, default 5).

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

# A C++17 compiler: CXX if set, else one on PATH, else a usual MSYS2 install (Git Bash doesn't
# put MSYS2 on its PATH)
find_cxx() {
    if [ -n "${CXX:-}" ]; then
        echo "$CXX"
        return
    fi
    local candidate
    for candidate in g++ clang++; do
        if command -v "$candidate" >/dev/null 2>&1; then
            echo "$candidate"
            return
        fi
    done
    for candidate in /c/msys64/ucrt64/bin/g++.exe /c/msys64/mingw64/bin/g++.exe /c/msys64/clang64/bin/clang++.exe \
                     /c/msys64/ucrt64/bin/clang++.exe; do
        if [ -x "$candidate" ]; then
            echo "$candidate"
            return
        fi
    done
}

replay_tool() {
    local exe="$OUT/.bin/pacer_replay"
    case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) exe="$exe.exe" ;; esac
    mkdir -p "$OUT/.bin"
    # Rebuilt whenever the pacer changes, so the replay always uses this checkout's code. The
    # check is against the program itself: a build that didn't replace it must not look current.
    if [ ! -f "$exe" ] || \
       [ -n "$(find "$SRC/frame_pacer.cpp" "$SRC/frame_pacer.h" "$SRC/tests/pacer_replay.cpp" -newer "$exe" 2>/dev/null)" ]; then
        local cxx
        cxx="$(find_cxx)"
        if [ -z "$cxx" ]; then
            echo "No C++ compiler found for the replay tool. Install one (on Windows, MSYS2's" >&2
            echo "mingw-w64-ucrt-x86_64-gcc) or set CXX to its path." >&2
            exit 1
        fi
        echo "Building the replay tool with $cxx..." >&2
        rm -f "$exe"
        # Static, so it runs without the compiler's runtime libraries on PATH
        local static=""
        case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) static="-static" ;; esac
        # With its own directory on PATH: MSYS2's compiler runs helpers that need the DLLs there
        local cxxdir=""
        case "$cxx" in */*) cxxdir="$(dirname "$cxx"):" ;; esac
        PATH="$cxxdir$PATH" "$cxx" -std=c++17 -O2 $static -I"$SRC" "$SRC/frame_pacer.cpp" "$SRC/tests/pacer_replay.cpp"             -o "$exe" >&2 || true
        if [ ! -f "$exe" ]; then
            echo "Building the replay tool didn't produce $exe" >&2
            exit 1
        fi
    fi
    echo "$exe"
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

cmd_presets() {
    # Stored as computer|app|name under each preset's key; keys starting with _ are bookkeeping
    adb_ shell run-as "$PKG" cat shared_prefs/QuickLaunch.xml | tr -d '\r' |
        sed -n 's/.*<string name="[^_"][^"]*">\([^|<]*\)|\([^|<]*\)|\([^<]*\)<\/string>.*/\3\t(\1: \2)/p'
}

# Whether the stream's activity is the one in front
stream_running() {
    adb_ shell dumpsys activity activities | tr -d '\r' |
        grep -E 'mResumedActivity|topResumedActivity' | grep -q "$PKG/com.limelight.Game"
}

# Ends the stream as leaving it would, without quitting the host's app: the app's task is
# replaced by its main screen, which finishes the stream. (Home would put the stream in
# picture-in-picture, where it keeps going.)
stop_stream() {
    adb_ shell am start -n "$PKG/com.limelight.PcView" -a android.intent.action.MAIN -f 0x10008000 >/dev/null
}

# Ends the stream and quits the host's app (StreamControlReceiver, as the quit shortcut does), so
# the next stream starts it afresh with its own settings: a stream started while the app runs
# resumes it at the frame rate it was started with. Then gives the host time to wind it down.
quit_stream() {
    adb_ shell am broadcast -a com.limelight.QUIT_STREAM -n "$PKG/com.limelight.receivers.StreamControlReceiver" >/dev/null
    local tries=0
    while stream_running && [ "$tries" -lt 15 ]; do
        sleep 1
        tries=$((tries + 1))
    done
    if stream_running; then
        echo "The stream didn't end after asking it to quit; leaving it instead" >&2
        stop_stream
    fi
    sleep "${RUN_QUIT_WAIT:-5}"
}

end_stream() {
    if [ "$RUN_KEEP_APP" = 1 ]; then stop_stream; else quit_stream; fi
}

# Starts the preset and echoes the trace's path on the device once the renderer starts one
start_stream() {
    local preset="$1" remote="" waited=0
    adb_ logcat -c || true
    : > "$RUN_LOG"
    echo "Starting Quick Launch preset '$preset'..." >&2
    adb_ shell am start -n "$PKG/com.limelight.ShortcutTrampoline" --es QuickLaunchName "'$preset'" >/dev/null
    # The renderer logs the trace's path once the stream's video starts
    while [ -z "$remote" ]; do
        if [ "$waited" -ge 60 ]; then
            echo "No trace started within 60 s. Is a dialog showing (another app running on the" >&2
            echo "host, host offline), the screen locked, or the renderer not Vulkan?" >&2
            return 1
        fi
        sleep 1
        waited=$((waited + 1))
        remote="$(sed -n 's/.*Recording pacer trace to \(.*\.csv\).*/\1/p' "$RUN_LOG" | tail -n 1 | tr -d '\r')"
    done
    echo "$remote"
}

# The frame rate the stream actually runs at, from the median spacing of host timestamps in the
# trace's first frames (the C line has the rate asked for, which a resumed host app ignores)
trace_fps() {
    local fps="" tries=0 deltas n median
    while [ -z "$fps" ] && [ "$tries" -lt 15 ]; do
        sleep 1
        tries=$((tries + 1))
        # Past the first frames, which come unevenly while the stream starts
        deltas="$(adb_ shell "grep '^F,' '$1' | sed -n '21,140p'" 2>/dev/null | tr -d '\r' |
            awk -F, 'NR > 1 && $3 > p { print $3 - p } { p = $3 }' | sort -n)"
        n="$(printf '%s\n' "$deltas" | grep -c . || true)"
        if [ "$n" -ge 100 ]; then
            median="$(printf '%s\n' "$deltas" | sed -n "$((n / 2 + 1))p")"
            fps="$(awk -v d="$median" 'BEGIN { printf "%d", 1e9 / d + 0.5 }')"
        fi
    done
    echo "$fps"
}

remote_size() {
    adb_ shell stat -c %s "$1" 2>/dev/null | tr -d '\r'
}

cmd_run() {
    local preset="" seconds=60
    # Global, for the exit trap
    RUN_PROPS=()
    RUN_KEEP_APP=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --prop) RUN_PROPS+=("$2"); shift 2 ;;
            --prop=*) RUN_PROPS+=("${1#--prop=}"); shift ;;
            --keep-app) RUN_KEEP_APP=1; shift ;;
            *)
                if [ -z "$preset" ]; then preset="$1"
                elif [[ "$1" =~ ^[0-9]+$ ]]; then seconds="$1"
                else echo "Unexpected argument: $1" >&2; exit 2
                fi
                shift ;;
        esac
    done
    if [ -z "$preset" ]; then
        echo "usage: $0 run <preset> [seconds] [--prop name=value]... [--keep-app]" >&2
        echo "Presets:" >&2
        cmd_presets >&2 || true
        exit 2
    fi
    adb_ get-state >/dev/null

    RUN_LOG="$(mktemp)"
    RUN_LOGCAT=""
    run_cleanup() {
        [ -n "$RUN_LOGCAT" ] && kill "$RUN_LOGCAT" 2>/dev/null || true
        local p
        for p in "${RUN_PROPS[@]}"; do
            adb_ shell setprop "${p%%=*}" "''" || true
        done
        adb_ shell setprop "$PROPERTY" 0 || true
        rm -f "$RUN_LOG"
    }
    trap run_cleanup EXIT

    local p
    for p in "${RUN_PROPS[@]}"; do
        adb_ shell setprop "${p%%=*}" "'${p#*=}'"
        echo "Set ${p%%=*}=${p#*=} for this run"
    done
    adb_ shell setprop "$PROPERTY" 1

    adb_ shell input keyevent KEYCODE_WAKEUP
    adb_ shell wm dismiss-keyguard >/dev/null 2>&1 || true

    adb_ logcat -v brief -s VulkanPacer:I VulkanRenderer:I >> "$RUN_LOG" 2>/dev/null &
    RUN_LOGCAT=$!

    # A preset named for its frame rate ("72 FPS") is checked against the stream's: a host app
    # left running from a stream at another rate is resumed at that rate, so it's quit and the
    # preset started again
    local expected="" remote fps attempt
    if [[ "$preset" =~ ([0-9]+)\ ?[Ff][Pp][Ss] ]]; then
        expected="${BASH_REMATCH[1]}"
    fi
    for attempt in 1 2; do
        remote="$(start_stream "$preset")" || { stop_stream; exit 1; }
        [ -n "$expected" ] || break
        fps="$(trace_fps "$remote")"
        # Within a few fps: the host runs at the display's measured rate, such as 143.65
        if [ -n "$fps" ] && [ "$fps" -ge $((expected - 3)) ] && [ "$fps" -le $((expected + 3)) ]; then
            echo "Streaming at $fps fps" >&2
            break
        fi
        echo "The stream is at ${fps:-an unknown rate} fps, not $expected (host app left running at another rate?)" >&2
        quit_stream
        adb_ shell rm -f "$remote"
        if [ "$attempt" = 2 ]; then
            echo "Giving up" >&2
            exit 1
        fi
        echo "Quit the host app; starting again" >&2
    done
    echo "Recording $remote for $seconds s"

    local elapsed=0
    while [ "$elapsed" -lt "$seconds" ]; do
        sleep 5
        elapsed=$((elapsed + 5))
        if ! stream_running; then
            echo "The stream ended early, after about $elapsed s" >&2
            break
        fi
    done

    end_stream
    # The trace is closed with the renderer; until then it grows every couple of seconds
    local last="" size stable=0 tries=0
    while [ "$stable" -lt 3 ] && [ "$tries" -lt 30 ]; do
        sleep 1
        tries=$((tries + 1))
        size="$(remote_size "$remote")"
        if [ -n "$size" ] && [ "$size" = "$last" ] && ! stream_running; then
            stable=$((stable + 1))
        else
            stable=0
        fi
        last="$size"
    done
    echo "Stream ended"

    mkdir -p "$OUT"
    adb_ pull "$remote" "$(local_path "$OUT")" >/dev/null
    adb_ shell rm -f "$remote"
    local file="$OUT/$(basename "$remote")"
    echo "Trace: $file"
    echo
    echo "Pacer summaries:"
    grep -E 'VulkanPacer' "$RUN_LOG" | tr -d '\r' | sed 's/^/  /' || true
    echo
    cmd_analyze "$file"
}

case "${1:-}" in
    on) cmd_on ;;
    presets) cmd_presets ;;
    run) shift; cmd_run "$@" ;;
    off) cmd_off ;;
    live) cmd_live ;;
    pull) shift; cmd_pull "$@" ;;
    analyze) shift; cmd_analyze "$@" ;;
    session) cmd_session ;;
    *) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
