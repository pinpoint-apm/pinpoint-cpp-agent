#!/usr/bin/env bash
set -euo pipefail

TARGET_PID=""
OUTPUT=""
FREQUENCY=99
CHECK_ONLY=false
PROFILER_PID=""
PROFILER_STATUS=0
PROFILER_LOG=""
PLATFORM=""
PARANOID_PATH=/proc/sys/kernel/perf_event_paranoid

usage() {
    cat <<EOF
Usage: $0 --pid PID [--output PATH] [--frequency HZ] -- COMMAND [ARGS...]
       $0 --check [--frequency HZ]

Profile an already-running server while COMMAND generates load.

Options:
  --pid PID          Process to attach to (required)
  --output PATH      Output .trace (macOS) or perf.data file (Linux)
  --frequency HZ     Linux perf sampling frequency (default: $FREQUENCY)
  --check            Verify the profiler can record, then exit without
                     profiling anything (--pid and COMMAND not required)
  -h, --help         Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) TARGET_PID=$2; shift 2 ;;
        --output) OUTPUT=$2; shift 2 ;;
        --frequency) FREQUENCY=$2; shift 2 ;;
        --check) CHECK_ONLY=true; shift ;;
        --) shift; break ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ ! "$FREQUENCY" =~ ^[1-9][0-9]*$ ]]; then
    echo "--frequency must be a positive integer." >&2
    exit 2
fi
if ! $CHECK_ONLY; then
    if [[ -z "$TARGET_PID" || ! "$TARGET_PID" =~ ^[0-9]+$ ]]; then
        echo "--pid must be a running numeric process ID." >&2
        exit 2
    fi
    if ! kill -0 "$TARGET_PID" 2>/dev/null; then
        echo "Profile target PID $TARGET_PID is not running." >&2
        exit 2
    fi
    if [[ $# -eq 0 ]]; then
        echo "A load-test command is required after --." >&2
        exit 2
    fi
fi

check_darwin_profiler() {
    if ! command -v xcrun >/dev/null 2>&1; then
        echo "xcrun is required for macOS profiling." >&2
        return 1
    fi
    if ! xcrun --find xctrace >/dev/null 2>&1; then
        echo "xctrace is required; install Xcode Command Line Tools." >&2
        return 1
    fi
}

# `command -v perf` proves nothing: on Ubuntu /usr/bin/perf is a dispatch
# wrapper that exits 2 when no kernel-matched binary is installed, and a
# perf_event_paranoid above 2 denies perf_event_open even for a process perf
# started itself. Take a throwaway capture instead, and report every cause
# that applies -- a host can have both, and the wrapper failure hides the
# paranoid one.
check_linux_profiler() {
    local log status output paranoid diagnosed=false

    if ! command -v perf >/dev/null 2>&1; then
        echo "perf is required for Linux profiling; install linux-tools-$(uname -r)." >&2
        return 1
    fi

    log=$(mktemp "${TMPDIR:-/tmp}/perf-preflight.XXXXXX")
    set +e
    perf record -F "$FREQUENCY" -g --call-graph dwarf -o /dev/null -- true \
        >"$log" 2>&1
    status=$?
    set -e
    output=$(cat "$log")
    rm -f "$log"
    if [[ $status -eq 0 ]]; then
        return 0
    fi

    echo "perf cannot record on this host (perf record exited $status)." >&2
    if grep -q 'perf not found for kernel' <<<"$output"; then
        cat >&2 <<EOF
  - The perf wrapper has no binary for kernel $(uname -r); the
    linux-tools-$(uname -r) package is missing or ships no perf.
EOF
        diagnosed=true
    fi
    if [[ -r "$PARANOID_PATH" ]]; then
        paranoid=$(tr -d '[:space:]' <"$PARANOID_PATH")
        if [[ "$paranoid" =~ ^-?[0-9]+$ && "$paranoid" -gt 2 ]]; then
            cat >&2 <<EOF
  - kernel.perf_event_paranoid is $paranoid; above 2 the kernel denies
    perf_event_open to unprivileged users outright, even for a process
    perf itself launched.
EOF
            diagnosed=true
        fi
    fi
    if ! $diagnosed; then
        sed -n '1,40p' <<<"$output" >&2
    fi
    cat >&2 <<EOF
  Fixes (each needs root):
    sudo sysctl -w kernel.perf_event_paranoid=1
    sudo apt install linux-tools-$(uname -r)
    or run the profiled pass in a container with --cap-add=CAP_PERFMON
EOF
    return 1
}

PLATFORM=$(uname -s)
case "$PLATFORM" in
    Darwin) check_darwin_profiler || exit 2 ;;
    Linux) check_linux_profiler || exit 2 ;;
    *)
        echo "Profiling is supported only on macOS and Linux (found: $PLATFORM)." >&2
        exit 2
        ;;
esac

if $CHECK_ONLY; then
    if [[ "$PLATFORM" == "Darwin" ]]; then
        echo "Profiler preflight passed: xctrace is available."
    else
        echo "Profiler preflight passed: perf recorded at ${FREQUENCY}Hz."
    fi
    exit 0
fi

if [[ "$PLATFORM" == "Darwin" ]]; then
    OUTPUT=${OUTPUT:-"profile-$(date +%Y%m%d-%H%M%S).trace"}
    if [[ "$OUTPUT" != *.trace ]]; then
        OUTPUT="${OUTPUT}.trace"
    fi
else
    OUTPUT=${OUTPUT:-"perf-$(date +%Y%m%d-%H%M%S).data"}
    if [[ "$OUTPUT" != *.data ]]; then
        OUTPUT="${OUTPUT}.data"
    fi
fi

if [[ -e "$OUTPUT" ]]; then
    echo "Profile output already exists: $OUTPUT" >&2
    exit 2
fi
mkdir -p "$(dirname "$OUTPUT")"
PROFILER_LOG="${OUTPUT}.log"

stop_profiler() {
    if [[ -z "$PROFILER_PID" ]]; then
        return
    fi
    local pid=$PROFILER_PID
    PROFILER_PID=""
    if kill -0 "$pid" 2>/dev/null; then
        kill -INT "$pid" 2>/dev/null || true
    fi
    set +e
    wait "$pid"
    PROFILER_STATUS=$?
    set -e
}

on_signal() {
    stop_profiler
    exit 130
}
trap stop_profiler EXIT
trap on_signal INT TERM

if [[ "$PLATFORM" == "Darwin" ]]; then
    xcrun xctrace record \
        --template "Time Profiler" \
        --attach "$TARGET_PID" \
        --output "$OUTPUT" \
        --no-prompt >"$PROFILER_LOG" 2>&1 &
else
    perf record \
        -F "$FREQUENCY" \
        -g \
        --call-graph dwarf \
        -p "$TARGET_PID" \
        -o "$OUTPUT" >"$PROFILER_LOG" 2>&1 &
fi
PROFILER_PID=$!

sleep 1
if ! kill -0 "$PROFILER_PID" 2>/dev/null; then
    set +e
    wait "$PROFILER_PID"
    PROFILER_STATUS=$?
    set -e
    PROFILER_PID=""
    echo "Profiler failed to start (exit $PROFILER_STATUS)." >&2
    sed -n '1,120p' "$PROFILER_LOG" >&2
    exit 1
fi

echo "Profiling PID $TARGET_PID with $([[ "$PLATFORM" == "Darwin" ]] && echo xctrace || echo perf)"
echo "Profile output: $OUTPUT"

set +e
"$@"
COMMAND_STATUS=$?
set -e

stop_profiler
trap - EXIT INT TERM

if [[ "$PROFILER_STATUS" -ne 0 && "$PROFILER_STATUS" -ne 130 ]]; then
    echo "Profiler exited with status $PROFILER_STATUS." >&2
    sed -n '1,120p' "$PROFILER_LOG" >&2
    exit 1
fi
if [[ "$PLATFORM" == "Darwin" && ! -d "$OUTPUT" ]]; then
    echo "xctrace did not create the expected trace bundle: $OUTPUT" >&2
    exit 1
fi
if [[ "$PLATFORM" == "Linux" && ! -s "$OUTPUT" ]]; then
    echo "perf did not create a non-empty data file: $OUTPUT" >&2
    exit 1
fi

if [[ "$PLATFORM" == "Darwin" ]]; then
    echo "Open the profile with: open '$OUTPUT'"
else
    echo "Inspect the profile with: perf report -i '$OUTPUT'"
fi
exit "$COMMAND_STATUS"
