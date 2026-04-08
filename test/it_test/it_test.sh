#!/usr/bin/env bash
set -euo pipefail

HOST="${HOST:-localhost}"
PORT="${PORT:-8090}"
BASE_URL="http://${HOST}:${PORT}"

# Defaults
DURATION=60
CONCURRENCY=10
MODE="mixed"
INTERVAL=1

usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Integration test for pinpoint-cpp-agent performance and memory leak detection.

Prerequisites:
  1. Start MySQL:    cd test/it_test && docker-compose up -d
  2. Start servers:  ./grpc_server &
                     ./it_test_server &

Options:
  -d, --duration SEC     Test duration in seconds (default: $DURATION)
  -c, --concurrency N    Number of concurrent workers (default: $CONCURRENCY)
  -m, --mode MODE        Test mode (default: $MODE)
  -i, --interval SEC     Stats polling interval (default: $INTERVAL)
  -h, --help             Show this help

Test Modes:
  simple      Minimal span endpoint only
  deep        Deeply nested spans only
  wide        Many sequential spans only
  annotated   Annotation-heavy spans only
  mixed       Rotate through all HTTP endpoints
  stress      High concurrency (50) mixed workload
  db-crud     MySQL CRUD operations
  db-batch    MySQL batch insert/select
  db-complex  MySQL complex queries (JOIN, subquery, aggregation)
  db-all      All MySQL endpoints combined
  grpc-unary  gRPC unary calls
  grpc-stream gRPC server-streaming calls
  grpc-bidi   gRPC bidirectional streaming calls
  grpc-all    All gRPC methods combined
  full        All endpoints (HTTP + gRPC + MySQL)

Environment:
  HOST    Server host (default: localhost)
  PORT    Server port (default: 8090)

Examples:
  $0 -d 120 -c 20 -m mixed
  $0 -m stress -d 300
  $0 -m db-all -d 60 -c 5
  $0 -m grpc-all -d 60 -c 10
  $0 -m full -d 180 -c 15
  HOST=10.0.0.1 PORT=9090 $0 -d 60
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--duration)    DURATION="$2"; shift 2 ;;
        -c|--concurrency) CONCURRENCY="$2"; shift 2 ;;
        -m|--mode)        MODE="$2"; shift 2 ;;
        -i|--interval)    INTERVAL="$2"; shift 2 ;;
        -h|--help)        usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

if [[ "$MODE" == "stress" ]]; then
    CONCURRENCY=50
fi

# Check server is reachable
if ! curl -sf "${BASE_URL}/stats" > /dev/null 2>&1; then
    echo "ERROR: Server not reachable at ${BASE_URL}"
    echo "Start the it_test_server first."
    exit 1
fi

# Build URL list based on mode
build_urls() {
    case $MODE in
        simple)
            echo "${BASE_URL}/simple"
            ;;
        deep)
            echo "${BASE_URL}/deep?depth=10"
            echo "${BASE_URL}/deep?depth=30"
            echo "${BASE_URL}/deep?depth=50"
            ;;
        wide)
            echo "${BASE_URL}/wide?width=20"
            echo "${BASE_URL}/wide?width=100"
            echo "${BASE_URL}/wide?width=300"
            ;;
        annotated)
            echo "${BASE_URL}/annotated"
            ;;
        db-crud)
            echo "${BASE_URL}/db-crud"
            ;;
        db-batch)
            echo "${BASE_URL}/db-batch?size=10"
            echo "${BASE_URL}/db-batch?size=50"
            echo "${BASE_URL}/db-batch?size=100"
            ;;
        db-complex)
            echo "${BASE_URL}/db-complex"
            ;;
        db-all)
            echo "${BASE_URL}/db-crud"
            echo "${BASE_URL}/db-batch?size=10"
            echo "${BASE_URL}/db-batch?size=50"
            echo "${BASE_URL}/db-complex"
            ;;
        grpc-unary)
            echo "${BASE_URL}/grpc-unary"
            ;;
        grpc-stream)
            echo "${BASE_URL}/grpc-stream"
            ;;
        grpc-bidi)
            echo "${BASE_URL}/grpc-bidi?count=3"
            echo "${BASE_URL}/grpc-bidi?count=10"
            ;;
        grpc-all)
            echo "${BASE_URL}/grpc-unary"
            echo "${BASE_URL}/grpc-stream"
            echo "${BASE_URL}/grpc-bidi?count=3"
            echo "${BASE_URL}/grpc-all"
            ;;
        mixed|stress)
            echo "${BASE_URL}/simple"
            echo "${BASE_URL}/deep?depth=10"
            echo "${BASE_URL}/deep?depth=30"
            echo "${BASE_URL}/wide?width=20"
            echo "${BASE_URL}/wide?width=100"
            echo "${BASE_URL}/annotated"
            echo "${BASE_URL}/mixed"
            echo "${BASE_URL}/error"
            ;;
        full)
            echo "${BASE_URL}/simple"
            echo "${BASE_URL}/deep?depth=10"
            echo "${BASE_URL}/deep?depth=30"
            echo "${BASE_URL}/wide?width=20"
            echo "${BASE_URL}/wide?width=100"
            echo "${BASE_URL}/annotated"
            echo "${BASE_URL}/mixed"
            echo "${BASE_URL}/error"
            echo "${BASE_URL}/grpc-unary"
            echo "${BASE_URL}/grpc-stream"
            echo "${BASE_URL}/grpc-bidi?count=3"
            echo "${BASE_URL}/grpc-all"
            echo "${BASE_URL}/db-crud"
            echo "${BASE_URL}/db-batch?size=20"
            echo "${BASE_URL}/db-complex"
            ;;
        *)
            echo "Unknown mode: $MODE" >&2
            exit 1
            ;;
    esac
}

# Quick smoke test: verify one endpoint per category
check_endpoint() {
    local name=$1 url=$2
    if curl -sf --max-time 5 "$url" > /dev/null 2>&1; then
        echo "  $name OK"
        return 0
    else
        echo "  $name FAILED"
        return 1
    fi
}

echo "Pre-flight checks..."
check_endpoint "HTTP"  "${BASE_URL}/simple" || { echo "ERROR: HTTP endpoints failed."; exit 1; }
check_endpoint "gRPC"  "${BASE_URL}/grpc-unary" || echo "WARNING: gRPC endpoints not available. Start grpc_server first."
check_endpoint "MySQL" "${BASE_URL}/db-crud" || echo "WARNING: MySQL endpoints not available. Start MySQL (docker-compose up -d) first."
echo ""

mapfile -t URLS < <(build_urls)
URL_COUNT=${#URLS[@]}

# Worker function: sends requests in a loop
worker() {
    local id=$1
    local end_time=$2
    local count=0
    local errors=0

    while [[ $(date +%s) -lt $end_time ]]; do
        local url="${URLS[$((RANDOM % URL_COUNT))]}"
        if curl -sf --max-time 30 "$url" > /dev/null 2>&1; then
            ((count++))
        else
            ((errors++))
        fi
    done

    echo "${count},${errors}"
}

# Get server PID for memory monitoring (macOS / Linux)
get_server_rss_kb() {
    local pid
    pid=$(lsof -ti :"${PORT}" 2>/dev/null | head -1) || true
    if [[ -n "$pid" ]]; then
        if [[ "$(uname)" == "Darwin" ]]; then
            ps -o rss= -p "$pid" 2>/dev/null | tr -d ' '
        else
            awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null || \
            ps -o rss= -p "$pid" 2>/dev/null | tr -d ' '
        fi
    else
        echo "0"
    fi
}

echo "============================================"
echo "  Pinpoint C++ Agent - Integration Test"
echo "============================================"
echo "Server:      ${BASE_URL}"
echo "Mode:        ${MODE}"
echo "Duration:    ${DURATION}s"
echo "Concurrency: ${CONCURRENCY}"
echo "Endpoints:   ${URL_COUNT}"
for u in "${URLS[@]}"; do
    echo "  -> ${u}"
done
echo "============================================"
echo ""

# Record initial memory
INITIAL_RSS=$(get_server_rss_kb)
echo "Initial server RSS: ${INITIAL_RSS} KB"
echo ""

END_TIME=$(( $(date +%s) + DURATION ))
TMPDIR_WORK=$(mktemp -d)
trap 'rm -rf "$TMPDIR_WORK"' EXIT

# Start workers in background
for i in $(seq 1 "$CONCURRENCY"); do
    worker "$i" "$END_TIME" > "${TMPDIR_WORK}/worker_${i}.out" 2>/dev/null &
done

# Monitor stats while workers are running
echo "Time(s)  | Requests  | Active | RSS(KB)  | RPS"
echo "---------|-----------|--------|----------|--------"

MONITOR_START=$(date +%s)
PREV_TOTAL=0

while [[ $(date +%s) -lt $END_TIME ]]; do
    sleep "$INTERVAL"
    ELAPSED=$(( $(date +%s) - MONITOR_START ))

    STATS=$(curl -sf "${BASE_URL}/stats" 2>/dev/null || echo '{}')
    CURRENT_TOTAL=$(echo "$STATS" | grep -o '"total_requests":[0-9]*' | cut -d: -f2)
    ACTIVE=$(echo "$STATS" | grep -o '"active_requests":[0-9]*' | cut -d: -f2)
    CURRENT_TOTAL=${CURRENT_TOTAL:-0}
    ACTIVE=${ACTIVE:-0}

    RSS=$(get_server_rss_kb)
    RPS=$(( (CURRENT_TOTAL - PREV_TOTAL) / INTERVAL ))
    PREV_TOTAL=$CURRENT_TOTAL

    printf "%-8s | %-9s | %-6s | %-8s | %s\n" \
           "$ELAPSED" "$CURRENT_TOTAL" "$ACTIVE" "$RSS" "$RPS"
done

# Wait for all workers to finish
wait

# Collect results
TOTAL_SENT=0
TOTAL_ERRORS=0
for f in "${TMPDIR_WORK}"/worker_*.out; do
    if [[ -f "$f" ]]; then
        IFS=',' read -r count errors < "$f"
        TOTAL_SENT=$((TOTAL_SENT + count))
        TOTAL_ERRORS=$((TOTAL_ERRORS + errors))
    fi
done

FINAL_RSS=$(get_server_rss_kb)

echo ""
echo "============================================"
echo "  Results"
echo "============================================"
echo "Total requests sent: ${TOTAL_SENT}"
echo "Total errors:        ${TOTAL_ERRORS}"
if [[ "$DURATION" -gt 0 ]]; then
    echo "Avg RPS:             $(( TOTAL_SENT / DURATION ))"
fi
echo ""
echo "Memory:"
echo "  Initial RSS:       ${INITIAL_RSS} KB"
echo "  Final RSS:         ${FINAL_RSS} KB"
if [[ "$INITIAL_RSS" -gt 0 && "$FINAL_RSS" -gt 0 ]]; then
    DIFF=$((FINAL_RSS - INITIAL_RSS))
    echo "  Delta:             ${DIFF} KB"
    if [[ "$DIFF" -gt 0 ]]; then
        PCT=$(awk "BEGIN{printf \"%.1f\", ($DIFF/$INITIAL_RSS)*100}")
        echo "  Growth:            ${PCT}%"
    fi
fi
echo "============================================"

# Memory leak heuristic
if [[ "$INITIAL_RSS" -gt 0 && "$FINAL_RSS" -gt 0 ]]; then
    GROWTH=$((FINAL_RSS - INITIAL_RSS))
    THRESHOLD=$((INITIAL_RSS / 2))  # 50% growth threshold
    if [[ "$GROWTH" -gt "$THRESHOLD" ]]; then
        echo ""
        echo "WARNING: RSS grew by more than 50%. Possible memory leak."
        echo "Consider running a longer test or using valgrind/ASan for confirmation."
    fi
fi
