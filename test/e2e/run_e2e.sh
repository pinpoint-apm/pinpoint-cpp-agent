#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR=""
HOST="127.0.0.1"
PORT=8090
DOWNSTREAM_PORT=8091
GRPC_PORT=50051
LOAD_MODE=""
LOAD_DURATION=30
LOAD_CONCURRENCY=5
LOAD_RPS=""
PROFILE=false
PROFILE_OUTPUT=""
PROFILE_FREQUENCY=99
RUN_C_API=true
RUN_FORK=true
KEEP_LOGS=false
LOG_DIR=""

usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Build and run the live-collector integration suite.

Options:
  -b, --build-dir DIR       Directory containing e2e binaries
      --host HOST           HTTP bind/check host (default: $HOST)
      --port PORT           Upstream HTTP port (default: $PORT)
      --downstream-port N   Downstream HTTP port (default: $DOWNSTREAM_PORT)
      --grpc-port N         gRPC port (default: $GRPC_PORT)
      --load-mode MODE      Load workload to run after smoke checks
                            (unthrottled maximum throughput unless --load-rps)
      --load-duration SEC   Load duration (default: $LOAD_DURATION)
      --load-concurrency N  Load workers, or fixed-RPS max in-flight requests
                            (default: $LOAD_CONCURRENCY)
      --load-rps RPS        Use constant-arrival-rate load at this target RPS
      --profile             Profile it_test_server during the load phase
      --profile-output PATH Profile output (.trace on macOS, perf.data on Linux)
      --profile-frequency N Linux perf sampling frequency (default: $PROFILE_FREQUENCY)
      --skip-c-api          Skip the pure-C API scenario
      --skip-fork           Skip the cold-create/fork scenario
      --log-dir DIR         Store process logs in DIR
      --keep-logs           Keep an auto-created log directory on success
  -h, --help                Show this help

Environment:
  PINPOINT_CPP_COLLECTOR_HOST must be set to the collector host.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--build-dir) BUILD_DIR=$2; shift 2 ;;
        --host) HOST=$2; shift 2 ;;
        --port) PORT=$2; shift 2 ;;
        --downstream-port) DOWNSTREAM_PORT=$2; shift 2 ;;
        --grpc-port) GRPC_PORT=$2; shift 2 ;;
        --load-mode) LOAD_MODE=$2; shift 2 ;;
        --load-duration) LOAD_DURATION=$2; shift 2 ;;
        --load-concurrency) LOAD_CONCURRENCY=$2; shift 2 ;;
        --load-rps) LOAD_RPS=$2; shift 2 ;;
        --profile) PROFILE=true; shift ;;
        --profile-output) PROFILE_OUTPUT=$2; shift 2 ;;
        --profile-frequency) PROFILE_FREQUENCY=$2; shift 2 ;;
        --skip-c-api) RUN_C_API=false; shift ;;
        --skip-fork) RUN_FORK=false; shift ;;
        --log-dir) LOG_DIR=$2; shift 2 ;;
        --keep-logs) KEEP_LOGS=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if $PROFILE && [[ -z "$LOAD_MODE" && -z "$LOAD_RPS" ]]; then
    echo "--profile requires a load phase (--load-mode or --load-rps)." >&2
    exit 2
fi
if ! $PROFILE && [[ -n "$PROFILE_OUTPUT" ]]; then
    echo "--profile-output requires --profile." >&2
    exit 2
fi
if [[ -n "$LOAD_RPS" && -z "$LOAD_MODE" ]]; then
    LOAD_MODE="mixed"
fi

if [[ -z "${PINPOINT_CPP_COLLECTOR_HOST:-}" ]]; then
    echo "PINPOINT_CPP_COLLECTOR_HOST must be set." >&2
    exit 2
fi

find_build_dir() {
    local candidate
    for candidate in \
        "$SCRIPT_DIR" \
        "$PROJECT_DIR/build/default/test/e2e" \
        "$PROJECT_DIR/build/test/e2e" \
        "$PROJECT_DIR/bazel-bin/test/e2e"; do
        if [[ -x "$candidate/it_test_server" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

if [[ -z "$BUILD_DIR" ]]; then
    BUILD_DIR=$(find_build_dir) || {
        echo "Could not locate integration-test binaries." >&2
        echo "Build with: cmake --build --preset default --target it_test_server http_downstream_server grpc_server c_api_scenario fork_scenario" >&2
        exit 2
    }
fi
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"

UPSTREAM_BIN="$BUILD_DIR/it_test_server"
DOWNSTREAM_BIN="$BUILD_DIR/http_downstream_server"
GRPC_BIN="$BUILD_DIR/grpc_server"
C_API_BIN="$BUILD_DIR/c_api_scenario"
FORK_BIN="$BUILD_DIR/fork_scenario"

for binary in "$UPSTREAM_BIN" "$DOWNSTREAM_BIN" "$GRPC_BIN"; do
    if [[ ! -x "$binary" ]]; then
        echo "Missing integration-test binary: $binary" >&2
        exit 2
    fi
done
if $RUN_C_API && [[ ! -x "$C_API_BIN" ]]; then
    echo "Missing C API scenario binary: $C_API_BIN" >&2
    exit 2
fi
if $RUN_FORK && [[ ! -x "$FORK_BIN" ]]; then
    echo "Missing fork scenario binary: $FORK_BIN" >&2
    exit 2
fi

if [[ -z "$LOG_DIR" ]]; then
    LOG_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pinpoint-cpp-it.XXXXXX")
    AUTO_LOG_DIR=true
else
    mkdir -p "$LOG_DIR"
    LOG_DIR="$(cd "$LOG_DIR" && pwd)"
    AUTO_LOG_DIR=false
fi

export PINPOINT_CPP_COLLECTOR_HOST
export PINPOINT_CPP_CONFIG_FILE="${PINPOINT_CPP_CONFIG_FILE:-$SCRIPT_DIR/pinpoint-config.yaml}"
export PINPOINT_CPP_LOG_LEVEL="${PINPOINT_CPP_LOG_LEVEL:-debug}"
export PINPOINT_CPP_LOG_FILE_PATH=""
export PINPOINT_IT_AGENT_TIMEOUT="${PINPOINT_IT_AGENT_TIMEOUT:-30}"

RUN_SUFFIX="$(date +%H%M%S)-$$"
UPSTREAM_PID=""
DOWNSTREAM_PID=""
GRPC_PID=""

stop_process() {
    local pid=$1
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

# Waits up to 5s for a process asked to shut down to leave on its own.
# Signalling one mid-shutdown skips its exit handlers, and under coverage
# instrumentation that means it writes no profile at all.
wait_exit() {
    local pid=$1 waited=0
    [[ -n "$pid" ]] || return 0
    while kill -0 "$pid" 2>/dev/null && [[ $waited -lt 50 ]]; do
        sleep 0.1
        waited=$((waited + 1))
    done
}

cleanup() {
    # Content-Length: 0 — cpp-httplib 400s a bodiless POST without it, which
    # would silently skip the graceful shutdown and leave only the kill below.
    curl -sS --max-time 2 -H 'Content-Length: 0' \
        -X POST "http://$HOST:$PORT/server/shutdown" >/dev/null 2>&1 || true
    curl -sS --max-time 2 -H 'Content-Length: 0' \
        -X POST "http://$HOST:$DOWNSTREAM_PORT/shutdown" >/dev/null 2>&1 || true
    wait_exit "$UPSTREAM_PID"
    wait_exit "$DOWNSTREAM_PID"
    # The gRPC server has no control channel; it leaves through its SIGTERM
    # handler, so signal it rather than waiting for an exit that never comes.
    stop_process "$UPSTREAM_PID"
    stop_process "$DOWNSTREAM_PID"
    stop_process "$GRPC_PID"
}
trap cleanup EXIT INT TERM

echo "============================================"
echo " Pinpoint C++ Agent - Integration Test Stack"
echo "============================================"
echo "Collector: $PINPOINT_CPP_COLLECTOR_HOST"
echo "Config:    $PINPOINT_CPP_CONFIG_FILE"
echo "Binaries:  $BUILD_DIR"
echo "Logs:      $LOG_DIR"
echo "Run ID:    $RUN_SUFFIX"
echo ""

PINPOINT_CPP_APPLICATION_NAME="cpp-it-grpc-downstream" \
PINPOINT_CPP_AGENT_NAME="it-grpc-$RUN_SUFFIX" \
    "$GRPC_BIN" "$GRPC_PORT" >"$LOG_DIR/grpc_server.log" 2>&1 &
GRPC_PID=$!

PINPOINT_CPP_APPLICATION_NAME="cpp-it-http-downstream" \
PINPOINT_CPP_AGENT_NAME="it-down-$RUN_SUFFIX" \
    "$DOWNSTREAM_BIN" "$DOWNSTREAM_PORT" \
    >"$LOG_DIR/http_downstream_server.log" 2>&1 &
DOWNSTREAM_PID=$!

GRPC_TARGET="$HOST:$GRPC_PORT" HTTP_TARGET="$HOST:$DOWNSTREAM_PORT" \
PINPOINT_CPP_APPLICATION_NAME="cpp-it-http-upstream" \
PINPOINT_CPP_AGENT_NAME="it-up-$RUN_SUFFIX" \
    "$UPSTREAM_BIN" "$PORT" >"$LOG_DIR/it_test_server.log" 2>&1 &
UPSTREAM_PID=$!

sleep 1
for process in "$GRPC_PID:$GRPC_BIN" "$DOWNSTREAM_PID:$DOWNSTREAM_BIN" \
               "$UPSTREAM_PID:$UPSTREAM_BIN"; do
    pid=${process%%:*}
    name=${process#*:}
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "Process exited during startup: $name" >&2
        exit 1
    fi
done

SMOKE_ENV=(
    "HOST=$HOST"
    "PORT=$PORT"
    "DOWNSTREAM_PORT=$DOWNSTREAM_PORT"
)
if $RUN_C_API; then
    SMOKE_ENV+=("C_API_SCENARIO=$C_API_BIN")
fi
if $RUN_FORK; then
    SMOKE_ENV+=("FORK_SCENARIO=$FORK_BIN")
fi

set +e
env "${SMOKE_ENV[@]}" bash "$SCRIPT_DIR/smoke_test.sh"
RESULT=$?
set -e

if [[ -n "$LOAD_MODE" ]]; then
    echo ""
    set +e
    if [[ -n "$LOAD_RPS" ]]; then
        echo "Running fixed-RPS load mode: $LOAD_MODE at $LOAD_RPS RPS"
        LOAD_KIND="fixed-rps"
    else
        echo "Running maximum-throughput load mode: $LOAD_MODE"
        LOAD_KIND="max-throughput"
    fi
    LOAD_COMMAND=(python3 "$SCRIPT_DIR/load_test.py" \
        --base-url "http://$HOST:$PORT" --mode "$LOAD_MODE" \
        --duration "$LOAD_DURATION" --concurrency "$LOAD_CONCURRENCY" \
        --rss-pid "$UPSTREAM_PID" \
        ${LOAD_RPS:+--rps "$LOAD_RPS"})

    if $PROFILE; then
        if [[ -z "$PROFILE_OUTPUT" ]]; then
            PROFILE_OUTPUT="$LOG_DIR/profiles/${LOAD_KIND}-${RUN_SUFFIX}"
            KEEP_LOGS=true
        fi
        bash "$SCRIPT_DIR/profile_load.sh" \
            --pid "$UPSTREAM_PID" \
            --output "$PROFILE_OUTPUT" \
            --frequency "$PROFILE_FREQUENCY" \
            -- "${LOAD_COMMAND[@]}"
    else
        "${LOAD_COMMAND[@]}"
    fi
    LOAD_RESULT=$?
    set -e
    if [[ $LOAD_RESULT -ne 0 ]]; then
        RESULT=$LOAD_RESULT
    fi
fi

# Give the async span sender time to flush before validating the transport log.
sleep 3

echo ""
echo "Collector transport evidence"
for log in it_test_server.log http_downstream_server.log grpc_server.log; do
    if grep -q 'success to register the agent' "$LOG_DIR/$log"; then
        echo "  PASS  $log registered with collector"
    else
        echo "  FAIL  $log has no successful agent registration" >&2
        RESULT=1
    fi
done
if grep -q 'SendSpanBatch success' "$LOG_DIR/it_test_server.log"; then
    echo "  PASS  upstream span batch reached collector"
else
    echo "  FAIL  upstream log has no successful span batch" >&2
    RESULT=1
fi

if [[ $RESULT -ne 0 ]]; then
    echo ""
    echo "Integration test failed. Logs kept at: $LOG_DIR" >&2
    exit "$RESULT"
fi

echo ""
echo "Integration test passed."
if $KEEP_LOGS || ! $AUTO_LOG_DIR; then
    echo "Logs: $LOG_DIR"
else
    rm -rf "$LOG_DIR"
fi
