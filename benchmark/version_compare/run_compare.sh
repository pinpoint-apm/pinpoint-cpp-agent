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

# Runs two api_benchmark builds against one collector and aggregates medians.
#
# The two builds are interleaved (baseline, candidate, baseline, candidate, ...)
# rather than run in blocks: on a laptop, thermal drift over a few minutes is
# large enough that all-A-then-all-B would report it as a version difference.

set -euo pipefail

BASELINE_BIN=""
CANDIDATE_BIN=""
COLLECTOR_BIN=""
BASELINE_LABEL="v1.1.0"
CANDIDATE_LABEL="main"
OPS=8000
WARMUP=2000
DRAIN_MS=3000
REPEATS=5
AGENT_PORT=19991
SPAN_PORT=19993
STAT_PORT=19992
OUT_DIR="./version_compare_results"

usage() {
    cat <<'EOF'
Usage: run_compare.sh --baseline-bin PATH --candidate-bin PATH --collector PATH [options]

Required:
  --baseline-bin PATH    api_benchmark built against the older agent
  --candidate-bin PATH   api_benchmark built against the newer agent
  --collector PATH       bench_collector binary

Options:
  --baseline-label NAME  default: v1.1.0
  --candidate-label NAME default: main
  --ops N                measured ops per thread per scenario (default 8000)
  --warmup N             warmup ops per thread per scenario (default 2000)
  --drain-ms N           sender drain pause between scenarios (default 3000)
  --repeats N            interleaved repetitions (default 5)
  --out DIR              output directory (default ./version_compare_results)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --baseline-bin) BASELINE_BIN="$2"; shift 2 ;;
        --candidate-bin) CANDIDATE_BIN="$2"; shift 2 ;;
        --collector) COLLECTOR_BIN="$2"; shift 2 ;;
        --baseline-label) BASELINE_LABEL="$2"; shift 2 ;;
        --candidate-label) CANDIDATE_LABEL="$2"; shift 2 ;;
        --ops) OPS="$2"; shift 2 ;;
        --warmup) WARMUP="$2"; shift 2 ;;
        --drain-ms) DRAIN_MS="$2"; shift 2 ;;
        --repeats) REPEATS="$2"; shift 2 ;;
        --out) OUT_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
    esac
done

if [[ -z "$BASELINE_BIN" || -z "$CANDIDATE_BIN" || -z "$COLLECTOR_BIN" ]]; then
    echo "--baseline-bin, --candidate-bin and --collector are all required" >&2
    usage
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$OUT_DIR"
RAW="$OUT_DIR/raw.tsv"
: > "$RAW"

COLLECTOR_PID=""
cleanup() {
    if [[ -n "$COLLECTOR_PID" ]] && kill -0 "$COLLECTOR_PID" 2>/dev/null; then
        kill "$COLLECTOR_PID" 2>/dev/null || true
        wait "$COLLECTOR_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# One collector per measured run. Its counters are cumulative, so a fresh
# process per run is what makes "did every span arrive?" answerable per run.
run_one() {
    local label="$1" bin="$2" rep="$3"
    local collector_out="$OUT_DIR/collector-${label}-${rep}.out"

    echo "-- $label rep $rep --"
    "$COLLECTOR_BIN" --agent-port "$AGENT_PORT" --span-port "$SPAN_PORT" --stat-port "$STAT_PORT" \
        > "$collector_out" 2>> "$OUT_DIR/collector.err" &
    COLLECTOR_PID=$!

    for _ in $(seq 1 100); do
        if grep -q READY "$collector_out" 2>/dev/null; then break; fi
        sleep 0.1
    done
    if ! grep -q READY "$collector_out" 2>/dev/null; then
        echo "collector failed to start; see $OUT_DIR/collector.err" >&2
        exit 1
    fi

    # Each run is its own process: the agent is a process-global singleton and
    # its shutdown is terminal, so one process cannot host two measured runs.
    "$bin" --host 127.0.0.1 \
        --agent-port "$AGENT_PORT" --span-port "$SPAN_PORT" --stat-port "$STAT_PORT" \
        --ops "$OPS" --warmup "$WARMUP" --drain-ms "$DRAIN_MS" \
        2>> "$OUT_DIR/${label}.log" \
        | sed "s/^/${rep}\t/" >> "$RAW"

    kill "$COLLECTOR_PID" 2>/dev/null || true
    wait "$COLLECTOR_PID" 2>/dev/null || true
    COLLECTOR_PID=""

    # span_messages (streamed) + spans_in_batches (batched): the two transports
    # the two release lines use.
    awk -F'\t' -v rep="$rep" -v label="$label" \
        '$1=="COUNTERS" { print rep "\tDELIVERED\t" label "\t" ($4 + $6) }' \
        "$collector_out" >> "$RAW"
}

for rep in $(seq 1 "$REPEATS"); do
    run_one "$BASELINE_LABEL" "$BASELINE_BIN" "$rep"
    run_one "$CANDIDATE_LABEL" "$CANDIDATE_BIN" "$rep"
done

echo "== aggregating =="
python3 "$SCRIPT_DIR/aggregate.py" \
    --raw "$RAW" \
    --baseline "$BASELINE_LABEL" \
    --candidate "$CANDIDATE_LABEL" \
    --out "$OUT_DIR/report.md" \
    --ops "$OPS" --repeats "$REPEATS"

echo
echo "raw:    $RAW"
echo "report: $OUT_DIR/report.md"
