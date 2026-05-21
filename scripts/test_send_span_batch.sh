#!/usr/bin/env bash
#
# Smoke-test the SendSpanBatch unary RPC against a live collector.
#
# Builds the http_server example, launches it with debug logging and
# the given collector host, generates traffic (single requests + a
# concurrent burst), then prints a summary of the batch-send activity.
#
# Usage:
#   ./scripts/test_send_span_batch.sh [OPTIONS]
#
# Options:
#   -h, --host HOST        Collector host (default: localhost)
#   -b, --build-dir DIR    CMake build dir (default: build/fetchcontent)
#   -p, --port PORT        http_server listen port (default: 8090)
#   -B, --burst N          Number of concurrent requests in the burst (default: 30)
#   -s, --singles N        Number of single sequential requests (default: 5)
#   -W, --wait SEC         Seconds to wait after traffic before stopping (default: 15)
#   -l, --log FILE         Where to write the server log (default: /tmp/pinpoint-http-server.log)
#       --no-build         Skip the build step
#       --help             Show this help

set -euo pipefail

HOST="localhost"
BUILD_DIR="build/fetchcontent"
PORT=8090
BURST=30
SINGLES=5
WAIT_AFTER_TRAFFIC=15
LOG_FILE="/tmp/pinpoint-http-server.log"
SKIP_BUILD=false

usage() {
    sed -n '2,22p' "$0" | sed 's/^#\s\?//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--host)       HOST="$2";              shift 2 ;;
        -b|--build-dir)  BUILD_DIR="$2";         shift 2 ;;
        -p|--port)       PORT="$2";              shift 2 ;;
        -B|--burst)      BURST="$2";             shift 2 ;;
        -s|--singles)    SINGLES="$2";           shift 2 ;;
        -W|--wait)       WAIT_AFTER_TRAFFIC="$2"; shift 2 ;;
        -l|--log)        LOG_FILE="$2";          shift 2 ;;
        --no-build)      SKIP_BUILD=true;        shift ;;
        --help)          usage ;;
        *)               echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_PATH="$REPO_ROOT/$BUILD_DIR"
SERVER_BIN="$BUILD_PATH/example/http_server"

SERVER_PID=""
cleanup() {
    local rc=$?
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "--- stopping server (pid=$SERVER_PID) ---"
        # The example's server.listen() does not honor SIGINT/SIGTERM,
        # so go straight to SIGKILL. The agent's logs were flushed to the
        # log file during the run as the stdio buffer filled.
        kill -9 "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    return $rc
}
trap cleanup EXIT INT TERM

if ! $SKIP_BUILD; then
    if [[ ! -d "$BUILD_PATH" ]]; then
        echo "Error: build dir does not exist: $BUILD_PATH" >&2
        echo "Configure first, e.g.: cmake --preset default" >&2
        exit 1
    fi
    echo "--- building http_server in $BUILD_DIR ---"
    if ! grep -q "BUILD_EXAMPLES:BOOL=ON" "$BUILD_PATH/CMakeCache.txt" 2>/dev/null; then
        echo "Re-enabling BUILD_EXAMPLES=ON in $BUILD_DIR"
        cmake -DBUILD_EXAMPLES=ON "$BUILD_PATH" > /dev/null
    fi
    ninja -C "$BUILD_PATH" http_server
fi

if [[ ! -x "$SERVER_BIN" ]]; then
    echo "Error: http_server binary not found at $SERVER_BIN" >&2
    exit 1
fi

if lsof -i :"$PORT" >/dev/null 2>&1; then
    echo "Error: port $PORT is already in use" >&2
    exit 1
fi

: > "$LOG_FILE"
echo "--- starting http_server ---"
echo "  collector host: $HOST"
echo "  binary:         $SERVER_BIN"
echo "  log file:       $LOG_FILE"
echo "  listen port:    $PORT"

PINPOINT_CPP_GRPC_HOST="$HOST" \
PINPOINT_CPP_LOG_LEVEL=debug \
"$SERVER_BIN" > "$LOG_FILE" 2>&1 &
SERVER_PID=$!

# Wait for the server to bind to the port.
echo "--- waiting for server to accept connections ---"
WAITED=0
until curl -s -o /dev/null -m 1 "http://localhost:$PORT/foo" 2>/dev/null; do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "Error: server exited during startup" >&2
        tail -50 "$LOG_FILE" >&2
        exit 1
    fi
    if [[ $WAITED -ge 30 ]]; then
        echo "Error: server did not become ready within 30s" >&2
        exit 1
    fi
    sleep 1
    WAITED=$((WAITED + 1))
done
echo "  server ready (waited ${WAITED}s)"

# Give the agent time to connect to the collector and register. We can't
# poll the log here because stdout is block-buffered when redirected to a
# file, so log lines may not appear in real time. 10s is comfortably
# beyond the typical 1-5s registration handshake.
echo "--- warming up agent (10s) ---"
sleep 10

# Sequential singles → exercises 1s flush_interval_ms path.
echo "--- sending $SINGLES sequential requests ---"
for i in $(seq 1 "$SINGLES"); do
    curl -s -m 5 -o /dev/null "http://localhost:$PORT/foo" || true
done

# Concurrent burst → exercises 500ms collect_deadline_ms / batching path.
# Use xargs -P to bound parallelism and avoid bash `wait` hangs.
echo "--- sending $BURST concurrent requests ---"
seq 1 "$BURST" | xargs -n1 -P "$BURST" -I{} \
    curl -s -m 5 -o /dev/null "http://localhost:$PORT/foo" || true

echo "--- waiting ${WAIT_AFTER_TRAFFIC}s for batches to flush ---"
sleep "$WAIT_AFTER_TRAFFIC"

# Stop the server now so the final stdio buffer is flushed.
cleanup || true
SERVER_PID=""
trap - EXIT INT TERM

echo
echo "=========================================="
echo "SendSpanBatch test summary"
echo "=========================================="

echo
echo "--- registration / channel ---"
grep -E "wait .* grpc channel ready|success to register the agent" "$LOG_FILE" | head -5 || true

echo
echo "--- batch sizes successfully sent (count × size) ---"
if grep -q "SendSpanBatch success" "$LOG_FILE"; then
    grep "SendSpanBatch success" "$LOG_FILE" \
        | awk -F'batchSize=' '{print $2}' \
        | sort -n | uniq -c
else
    echo "  (no successful sends observed)"
fi

echo
echo "--- collected batches (count × spans collected) ---"
if grep -q "collect_batch:" "$LOG_FILE"; then
    grep "collect_batch:" "$LOG_FILE" \
        | sed -E 's/.*collected=([0-9]+).*/\1/' \
        | sort -n | uniq -c
fi

echo
echo "--- errors / partial success / overload ---"
if grep -qE "partial success|SendSpanBatch failed|SendSpanBatch skipped|SendSpanBatch warning" "$LOG_FILE"; then
    grep -E "partial success|SendSpanBatch failed|SendSpanBatch skipped|SendSpanBatch warning" "$LOG_FILE"
else
    echo "  (none)"
fi

echo
echo "--- totals ---"
echo "  enqueueSpan calls:    $(grep -c 'enqueueSpan: queue_size' "$LOG_FILE" || true)"
echo "  collect_batch sum:    $(grep 'collect_batch:' "$LOG_FILE" | awk -F'collected=' '{print $2}' | awk '{sum+=$1} END {print sum+0}')"
echo "  SendSpanBatch sends:  $(grep -c 'SendSpanBatch sending' "$LOG_FILE" || true)"
echo "  SendSpanBatch oks:    $(grep -c 'SendSpanBatch success' "$LOG_FILE" || true)"

MAX_INFLIGHT=$(grep -oE 'concurrentRequests=[0-9]+/[0-9]+' "$LOG_FILE" \
    | awk -F'[=/]' '{print $2}' | sort -n | tail -1)
echo "  peak concurrentReq:   ${MAX_INFLIGHT:-0}/10"

echo
echo "Full log: $LOG_FILE"
