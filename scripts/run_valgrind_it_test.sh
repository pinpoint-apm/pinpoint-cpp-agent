#!/usr/bin/env bash
#
# Launch it_test_server (and optionally grpc_server) under valgrind,
# run the integration test, then collect valgrind reports.
#
# Usage:
#   ./scripts/run_valgrind_it_test.sh [OPTIONS]
#
# Options:
#   -b, --build-dir DIR    CMake build directory (default: ./build)
#   -o, --output-dir DIR   Valgrind report directory (default: ./valgrind-reports)
#   -m, --mode MODE        it_test.sh mode (default: mixed)
#   -d, --duration SEC     it_test.sh duration (default: 30)
#   -c, --concurrency N    it_test.sh concurrency (default: 5)
#   --no-grpc              Skip grpc_server
#   -h, --help             Show this help

set -euo pipefail

BUILD_DIR="build"
OUTPUT_DIR="valgrind-reports"
MODE="mixed"
DURATION=30
CONCURRENCY=5
USE_GRPC=true
PORT=8090
GRPC_PORT=50051

usage() {
    sed -n '2,16p' "$0" | sed 's/^#\s\?//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -b|--build-dir)    BUILD_DIR="$2";    shift 2 ;;
        -o|--output-dir)   OUTPUT_DIR="$2";   shift 2 ;;
        -m|--mode)         MODE="$2";         shift 2 ;;
        -d|--duration)     DURATION="$2";     shift 2 ;;
        -c|--concurrency)  CONCURRENCY="$2";  shift 2 ;;
        --no-grpc)         USE_GRPC=false;    shift ;;
        -h|--help)         usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
IT_TEST_DIR="$BUILD_DIR/test/it_test"
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"

IT_TEST_SERVER="$IT_TEST_DIR/it_test_server"
GRPC_SERVER="$IT_TEST_DIR/grpc_server"
IT_TEST_SH="$PROJECT_DIR/test/it_test/it_test.sh"

if ! command -v valgrind &>/dev/null; then
    echo "ERROR: valgrind is not installed." >&2
    exit 1
fi

if [[ ! -x "$IT_TEST_SERVER" ]]; then
    echo "ERROR: it_test_server not found at $IT_TEST_SERVER" >&2
    echo "Build the project first: cmake --build $BUILD_DIR" >&2
    exit 1
fi

if $USE_GRPC && [[ ! -x "$GRPC_SERVER" ]]; then
    echo "WARNING: grpc_server not found at $GRPC_SERVER — skipping gRPC server."
    USE_GRPC=false
fi

# PIDs to clean up on exit
GRPC_PID=""
SERVER_PID=""

cleanup() {
    echo ""
    echo "Shutting down servers..."
    [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null && wait "$SERVER_PID" 2>/dev/null || true
    [[ -n "$GRPC_PID" ]]  && kill "$GRPC_PID"  2>/dev/null && wait "$GRPC_PID"  2>/dev/null || true
    echo "Done."
}
trap cleanup EXIT

VALGRIND_OPTS=(
    --leak-check=full
    --show-leak-kinds=all
    --track-origins=yes
    --trace-children=no
)

echo "============================================"
echo "  Valgrind Integration Test"
echo "============================================"
echo "Build dir : $BUILD_DIR"
echo "Output dir: $OUTPUT_DIR"
echo "Mode      : $MODE"
echo "Duration  : ${DURATION}s"
echo "Concurrency: $CONCURRENCY"
echo "valgrind  : $(valgrind --version)"
echo "============================================"
echo ""

# --- Start grpc_server under valgrind ---
if $USE_GRPC; then
    GRPC_REPORT="$OUTPUT_DIR/grpc_server.valgrind.txt"
    echo "[START] grpc_server under valgrind (port $GRPC_PORT)..."
    valgrind "${VALGRIND_OPTS[@]}" \
        --log-file="$GRPC_REPORT" \
        "$GRPC_SERVER" "$GRPC_PORT" &
    GRPC_PID=$!
    echo "  PID=$GRPC_PID  report=$GRPC_REPORT"

    # Wait for grpc_server to be ready
    echo -n "  Waiting for grpc_server..."
    for i in $(seq 1 30); do
        if kill -0 "$GRPC_PID" 2>/dev/null; then
            sleep 1
            echo -n "."
        else
            echo " FAILED (process exited)"
            exit 1
        fi
        # grpc_server has no HTTP health check; just give it a few seconds
        if [[ $i -ge 3 ]]; then
            echo " ready (assumed after ${i}s)"
            break
        fi
    done
fi

# --- Start it_test_server under valgrind ---
SERVER_REPORT="$OUTPUT_DIR/it_test_server.valgrind.txt"
echo "[START] it_test_server under valgrind (port $PORT)..."

GRPC_TARGET="localhost:$GRPC_PORT"
if $USE_GRPC; then
    export GRPC_TARGET
fi

valgrind "${VALGRIND_OPTS[@]}" \
    --log-file="$SERVER_REPORT" \
    "$IT_TEST_SERVER" "$PORT" &
SERVER_PID=$!
echo "  PID=$SERVER_PID  report=$SERVER_REPORT"

# Wait for it_test_server to be ready
echo -n "  Waiting for it_test_server..."
for i in $(seq 1 30); do
    if curl -sf --max-time 2 "http://localhost:${PORT}/stats" >/dev/null 2>&1; then
        echo " ready (${i}s)"
        break
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo " FAILED (process exited)"
        exit 1
    fi
    sleep 1
    echo -n "."
done

if ! curl -sf --max-time 2 "http://localhost:${PORT}/stats" >/dev/null 2>&1; then
    echo " FAILED (timeout)"
    exit 1
fi

echo ""

# --- Run integration test ---
echo "[TEST] Running it_test.sh (mode=$MODE, duration=${DURATION}s, concurrency=$CONCURRENCY)..."
echo ""

HOST=localhost PORT=$PORT bash "$IT_TEST_SH" \
    -m "$MODE" -d "$DURATION" -c "$CONCURRENCY" || true

echo ""

# --- Stop servers gracefully ---
echo "[STOP] Stopping it_test_server (PID=$SERVER_PID)..."
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

if $USE_GRPC && [[ -n "$GRPC_PID" ]]; then
    echo "[STOP] Stopping grpc_server (PID=$GRPC_PID)..."
    kill "$GRPC_PID" 2>/dev/null || true
    wait "$GRPC_PID" 2>/dev/null || true
    GRPC_PID=""
fi

# Give valgrind a moment to flush log files
sleep 1

# --- Print summary ---
echo ""
echo "============================================"
echo "  Valgrind Reports"
echo "============================================"

print_report_summary() {
    local name=$1 report=$2

    if [[ ! -f "$report" ]]; then
        echo "  [$name] report not found"
        return
    fi

    local errors lost
    errors=$(grep -c "ERROR SUMMARY:" "$report" 2>/dev/null || echo "0")
    lost=$(grep "definitely lost:" "$report" 2>/dev/null | tail -1 || echo "  n/a")

    echo "  [$name] $report"

    grep "ERROR SUMMARY:" "$report" 2>/dev/null | tail -1 | sed 's/^==[0-9]*== /    /'
    grep "definitely lost:" "$report" 2>/dev/null | tail -1 | sed 's/^==[0-9]*== /    /'
    grep "indirectly lost:" "$report" 2>/dev/null | tail -1 | sed 's/^==[0-9]*== /    /'
    grep "possibly lost:" "$report" 2>/dev/null | tail -1 | sed 's/^==[0-9]*== /    /'
    grep "still reachable:" "$report" 2>/dev/null | tail -1 | sed 's/^==[0-9]*== /    /'
    echo ""
}

print_report_summary "it_test_server" "$SERVER_REPORT"
if $USE_GRPC; then
    print_report_summary "grpc_server" "$GRPC_REPORT"
fi

echo "Full reports saved to: $OUTPUT_DIR"
echo "============================================"
