#!/usr/bin/env bash
#
# Copyright 2020-present NAVER Corp.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Phase-2 orchestrator: drives bench_http_server builds of two agent versions
# (each in enabled and disabled/baseline mode) with test/e2e/fixed_rps_test.py,
# sampling server CPU/RSS during each pass and collecting span-delivery
# counters from a fresh collector per run.
#
# The four variants are interleaved within each repetition, for the same
# thermal-drift reason run_compare.sh interleaves its two.

set -euo pipefail

BASELINE_SERVER=""
CANDIDATE_SERVER=""
COLLECTOR_BIN=""
LOAD_SCRIPT=""
BASELINE_LABEL="v1.1.0"
CANDIDATE_LABEL="main"
RPS_LIST="1000 4000"
DURATION=30
REPEATS=3
MODE="mixed"
POOL=8
HTTP_PORT=18090
AGENT_PORT=19991
SPAN_PORT=19993
STAT_PORT=19992
OUT_DIR="./load_compare_results"

usage() {
    cat <<'EOF'
Usage: run_load_compare.sh --baseline-server PATH --candidate-server PATH \
         --collector PATH --load-script PATH [options]

Options:
  --rps-list "R1 R2 ..."  load levels per server run (default "1000 4000")
  --duration SEC          seconds per load pass (default 30)
  --repeats N             interleaved repetitions (default 3)
  --mode NAME             fixed_rps_test.py workload mode (default mixed)
  --pool N                server worker threads (default 8)
  --out DIR               output directory (default ./load_compare_results)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --baseline-server) BASELINE_SERVER="$2"; shift 2 ;;
        --candidate-server) CANDIDATE_SERVER="$2"; shift 2 ;;
        --collector) COLLECTOR_BIN="$2"; shift 2 ;;
        --load-script) LOAD_SCRIPT="$2"; shift 2 ;;
        --rps-list) RPS_LIST="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --repeats) REPEATS="$2"; shift 2 ;;
        --mode) MODE="$2"; shift 2 ;;
        --pool) POOL="$2"; shift 2 ;;
        --out) OUT_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
    esac
done

if [[ -z "$BASELINE_SERVER" || -z "$CANDIDATE_SERVER" || -z "$COLLECTOR_BIN" || -z "$LOAD_SCRIPT" ]]; then
    echo "--baseline-server, --candidate-server, --collector and --load-script are required" >&2
    usage
    exit 2
fi

mkdir -p "$OUT_DIR"

COLLECTOR_PID=""
SERVER_PID=""
cleanup() {
    [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null || true
    [[ -n "$COLLECTOR_PID" ]] && kill "$COLLECTOR_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

wait_http_200() {
    local url="$1" tries="$2"
    for _ in $(seq 1 "$tries"); do
        if curl -sf "$url" > /dev/null 2>&1; then return 0; fi
        sleep 0.2
    done
    return 1
}

# One (variant, rep): fresh collector + fresh server, then every RPS level.
run_variant() {
    local label="$1" server_bin="$2" mode_flag="$3" rep="$4"
    local tag="${label}-rep${rep}"
    local collector_out="$OUT_DIR/${tag}.collector.txt"

    echo "== $tag =="
    "$COLLECTOR_BIN" --agent-port "$AGENT_PORT" --span-port "$SPAN_PORT" --stat-port "$STAT_PORT" \
        > "$collector_out" 2>/dev/null &
    COLLECTOR_PID=$!
    for _ in $(seq 1 100); do
        grep -q READY "$collector_out" 2>/dev/null && break
        sleep 0.1
    done
    # Fail fast like run_compare.sh: without a READY collector the enabled
    # variants either burn a 30s registration timeout per pass or, worse,
    # register against a stale collector from a crashed earlier invocation —
    # leaving this run's delivery counters near zero and the span-delivery
    # table misleading instead of the script failing here.
    if ! grep -q READY "$collector_out" 2>/dev/null; then
        echo "collector for $tag never became READY; see $collector_out" >&2
        exit 1
    fi

    local server_args=(--port "$HTTP_PORT" --pool "$POOL" --host 127.0.0.1 \
        --agent-port "$AGENT_PORT" --span-port "$SPAN_PORT" --stat-port "$STAT_PORT")
    [[ "$mode_flag" == "disabled" ]] && server_args+=(--disable)
    "$server_bin" "${server_args[@]}" > /dev/null 2> "$OUT_DIR/${tag}.server.txt" &
    SERVER_PID=$!
    if ! wait_http_200 "http://127.0.0.1:$HTTP_PORT/ready" 150; then
        echo "server $tag never became ready" >&2
        exit 1
    fi

    # Warm the server (connections, lazy metadata) outside the measured passes.
    python3 "$LOAD_SCRIPT" --base-url "http://127.0.0.1:$HTTP_PORT" \
        --rps 500 --duration 3 --mode "$MODE" --max-error-rate 100 --rps-tolerance 100 \
        --no-require-agent > /dev/null 2>&1 || true

    local load_flags=(--mode "$MODE" --max-error-rate 1 --rps-tolerance 10)
    [[ "$mode_flag" == "disabled" ]] && load_flags+=(--no-require-agent)

    for rps in $RPS_LIST; do
        local pass="$OUT_DIR/${tag}-rps${rps}"
        # CPU/RSS sampler for the duration of this pass.
        (
            while kill -0 "$SERVER_PID" 2>/dev/null; do
                ps -o rss=,pcpu= -p "$SERVER_PID" 2>/dev/null || true
                sleep 0.5
            done
        ) > "${pass}.ps.txt" &
        local sampler=$!

        python3 "$LOAD_SCRIPT" --base-url "http://127.0.0.1:$HTTP_PORT" \
            --rps "$rps" --duration "$DURATION" "${load_flags[@]}" \
            > "${pass}.load.txt" 2>&1 || echo "LOADFAIL" >> "${pass}.load.txt"

        kill "$sampler" 2>/dev/null || true
        wait "$sampler" 2>/dev/null || true
        # Let the sender drain before the next pass shares the counters.
        sleep 5
    done

    curl -s "http://127.0.0.1:$HTTP_PORT/stats" > "$OUT_DIR/${tag}.stats.json" || true
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
    sleep 3   # collector prints COUNTERS on SIGTERM below
    kill "$COLLECTOR_PID" 2>/dev/null || true
    wait "$COLLECTOR_PID" 2>/dev/null || true
    COLLECTOR_PID=""
}

for rep in $(seq 1 "$REPEATS"); do
    run_variant "$BASELINE_LABEL"           "$BASELINE_SERVER"  enabled  "$rep"
    run_variant "$CANDIDATE_LABEL"          "$CANDIDATE_SERVER" enabled  "$rep"
    run_variant "${BASELINE_LABEL}-noagent"  "$BASELINE_SERVER"  disabled "$rep"
    run_variant "${CANDIDATE_LABEL}-noagent" "$CANDIDATE_SERVER" disabled "$rep"
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$SCRIPT_DIR/aggregate_load.py" \
    --dir "$OUT_DIR" \
    --baseline "$BASELINE_LABEL" --candidate "$CANDIDATE_LABEL" \
    --rps-list "$RPS_LIST" --repeats "$REPEATS" \
    --duration "$DURATION" --mode "$MODE" \
    --out "$OUT_DIR/report.md"

echo
echo "report: $OUT_DIR/report.md"
