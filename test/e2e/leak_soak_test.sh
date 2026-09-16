#!/usr/bin/env bash
#
# macOS leak soak: a long agent run against a collector that keeps dying.
#
# What this adds over soak_test.sh, which measures resource drift against a
# live collector on Linux:
#
#   * the collector is the integration suite's MockCollector, run as its own
#     process (test/it/mock_collector_server), so the suite needs no dev
#     collector and no network;
#   * that process is SIGKILLed on a fixed cycle and restarted on the same
#     ports, which is the fault a cooperative in-process shutdown cannot
#     produce: the agent sees a vanished host, its streams die mid-flight, and
#     every reconnect path (AgentInfo re-registration, ping, span batches,
#     stat stream, command stream, metadata retries) runs once per cycle;
#   * the verdict comes from macOS `leaks` rather than RSS alone. RSS cannot
#     separate a leak from allocator fragmentation or a cache that grew once;
#     `leaks` walks the live heap for blocks no root still references, so a
#     per-reconnect leak shows up as leaked bytes climbing cycle over cycle
#     even when RSS looks flat.
#
# The run is deliberately a debug build: assertions on, no inlining, so a leak
# report names real functions.
#
# Usage:
#   cmake --build --preset debug --target \
#       mock_collector_server it_test_server http_downstream_server grpc_server
#   ./test/e2e/leak_soak_test.sh --build-dir ./build/debug
#
# Detached (survives logout); stdout is a heartbeat, artifacts land in --out-dir:
#   nohup ./test/e2e/leak_soak_test.sh -b ./build/debug >/dev/null 2>&1 &
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# --- shape, all overridable -----------------------------------------------
BUILD_DIR=""
OUT_DIR=""
MODE="full"
RPS=20
DURATION=3600             # 1 h
CONCURRENCY=16
# The load generator's own pass/fail is not this run's verdict, and two things
# here legitimately disturb it: `leaks` suspends the whole target process while
# it walks the heap, and the collector outages make the agent's sender threads
# work harder. Both cost latency, never correctness.
MAX_ERROR_RATE="5"
RPS_TOLERANCE="25"
SAMPLE_INTERVAL=30        # resource series period
LEAK_INTERVAL=300         # `leaks` snapshot period
KILL_CYCLE=300            # kill-to-kill period for the collector
DOWNTIME=60               # how long the collector stays dead each cycle
STACK_LOGGING=true        # MallocStackLogging: backtraces in the leak report
HOST=127.0.0.1
PORT=8090
DOWNSTREAM_PORT=8091
GRPC_PORT=50051
AGENT_PORT=9991
SPAN_PORT=9993
STAT_PORT=9992

usage() {
    sed -n '3,4p' "$0" | sed 's/^#\{1,2\} \{0,1\}//'
    cat <<EOF
Options:
  -b, --build-dir DIR     Build tree root, e.g. ./build/debug (required)
      --out-dir DIR       Artifact directory (default: ./leaksoak-<timestamp>)
      --mode MODE         load_test.py mode (default: $MODE)
      --rps RPS           Fixed arrival rate (default: $RPS)
      --duration SEC      Soak duration (default: $DURATION = 1h)
      --concurrency N     Max in-flight requests (default: $CONCURRENCY)
      --kill-cycle SEC    Collector kill-to-kill period (default: $KILL_CYCLE)
      --downtime SEC      Collector downtime per cycle (default: $DOWNTIME)
      --leak-interval SEC \`leaks\` snapshot period (default: $LEAK_INTERVAL)
      --sample-interval S Resource sampling period (default: $SAMPLE_INTERVAL)
      --max-error-rate P  Tolerated final error rate (default: $MAX_ERROR_RATE)
      --no-stack-logging  Skip MallocStackLogging (no backtraces, less overhead)
      --port N            Upstream port (default: $PORT)
      --downstream-port N Downstream port (default: $DOWNSTREAM_PORT)
      --grpc-port N       gRPC port (default: $GRPC_PORT)
      --collector-ports A,S,T  Mock collector agent,span,stat ports
                          (default: $AGENT_PORT,$SPAN_PORT,$STAT_PORT)
  -h, --help              Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--build-dir)     BUILD_DIR=$2; shift 2 ;;
        --out-dir)          OUT_DIR=$2; shift 2 ;;
        --mode)             MODE=$2; shift 2 ;;
        --rps)              RPS=$2; shift 2 ;;
        --duration)         DURATION=$2; shift 2 ;;
        --concurrency)      CONCURRENCY=$2; shift 2 ;;
        --kill-cycle)       KILL_CYCLE=$2; shift 2 ;;
        --downtime)         DOWNTIME=$2; shift 2 ;;
        --leak-interval)    LEAK_INTERVAL=$2; shift 2 ;;
        --sample-interval)  SAMPLE_INTERVAL=$2; shift 2 ;;
        --max-error-rate)   MAX_ERROR_RATE=$2; shift 2 ;;
        --no-stack-logging) STACK_LOGGING=false; shift ;;
        --port)             PORT=$2; shift 2 ;;
        --downstream-port)  DOWNSTREAM_PORT=$2; shift 2 ;;
        --grpc-port)        GRPC_PORT=$2; shift 2 ;;
        --collector-ports)  IFS=, read -r AGENT_PORT SPAN_PORT STAT_PORT <<<"$2"; shift 2 ;;
        -h|--help)          usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# ==========================================================================
# Preflight
# ==========================================================================
fail() { echo "PREFLIGHT FAIL: $*" >&2; exit 2; }
# BSD date has no -Is.
now_iso() { date +%Y-%m-%dT%H:%M:%S%z; }

[[ "$(uname -s)" == "Darwin" ]] || fail "this suite needs macOS \`leaks\`; use soak_test.sh elsewhere."
command -v leaks >/dev/null || fail "\`leaks\` not found (install the Xcode command line tools)."
command -v lsof >/dev/null  || fail "\`lsof\` not found."

if [[ -z "$BUILD_DIR" ]]; then
    BUILD_DIR="$PROJECT_DIR/build/debug"
fi
[[ -d "$BUILD_DIR" ]] || fail "build dir not found: $BUILD_DIR"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
E2E_DIR="$BUILD_DIR/test/e2e"
COLLECTOR_BIN="$BUILD_DIR/test/it/mock_collector_server"
for b in "$E2E_DIR/it_test_server" "$E2E_DIR/http_downstream_server" \
         "$E2E_DIR/grpc_server" "$COLLECTOR_BIN"; do
    [[ -x "$b" ]] || fail "missing binary: $b
  build with: cmake --build --preset debug --target \\
      mock_collector_server it_test_server http_downstream_server grpc_server"
done

# `leaks` can only walk the heap of a process it is allowed to attach to. An
# ordinary CMake binary is ad-hoc signed without com.apple.security.get-task-
# allow, and for those macOS still produces a *report* -- one that says
# "is not debuggable" in its first line and then cheerfully claims 0 leaks.
# That false negative is the failure mode this run exists to avoid, so the
# binaries are re-signed here and the preflight rejects the message outright.
sign_for_leaks() {
    local plist="$1"; shift
    cat > "$plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict><key>com.apple.security.get-task-allow</key><true/></dict>
</plist>
PLIST
    local bin
    for bin in "$@"; do
        codesign -s - -f --entitlements "$plist" "$bin" >/dev/null 2>&1 \
            || fail "codesign --entitlements failed for $bin"
    done
}

# A leak verdict from an optimized build names inlined frames and misses the
# assertions; refuse to imply otherwise in the report.
BUILD_TYPE="$(awk -F= '/^CMAKE_BUILD_TYPE:/ {print $2}' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null)"
[[ "$BUILD_TYPE" == "Debug" ]] || fail "build dir is CMAKE_BUILD_TYPE=${BUILD_TYPE:-unknown}, expected Debug."

for v in RPS DURATION SAMPLE_INTERVAL CONCURRENCY LEAK_INTERVAL KILL_CYCLE; do
    [[ "${!v}" =~ ^[0-9]+$ && "${!v}" -gt 0 ]] || fail "--$(tr 'A-Z_' 'a-z-' <<<"$v") must be a positive integer."
done
(( DOWNTIME > 0 && DOWNTIME < KILL_CYCLE )) || fail "--downtime must be >0 and < --kill-cycle."

for p in "$PORT" "$DOWNSTREAM_PORT" "$GRPC_PORT" "$AGENT_PORT" "$SPAN_PORT" "$STAT_PORT"; do
    timeout_bin=""; command -v timeout >/dev/null && timeout_bin="timeout 1"
    $timeout_bin bash -c "cat < /dev/null > /dev/tcp/127.0.0.1/$p" 2>/dev/null \
        && fail "port $p is already in use (another stack running?)."
done

: "${OUT_DIR:=$PROJECT_DIR/leaksoak-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT_DIR"/{logs,leaks}; OUT_DIR="$(cd "$OUT_DIR" && pwd)"
COLLECTOR_PID_FILE="$OUT_DIR/collector.pid"
COLLECTOR_GEN_FILE="$OUT_DIR/collector.gen"
CSV="$OUT_DIR/resources.csv"
LEAK_CSV="$OUT_DIR/leaks.csv"
CHAOS_LOG="$OUT_DIR/chaos.log"
LOAD_LOG="$OUT_DIR/load.log"
SLOG="$OUT_DIR/logs"

sign_for_leaks "$OUT_DIR/get-task-allow.plist" \
    "$E2E_DIR/it_test_server" "$E2E_DIR/http_downstream_server" \
    "$E2E_DIR/grpc_server"

hours=$(python3 -c "print(f'{$DURATION/3600:.2f}')")
cycles=$(( DURATION / KILL_CYCLE ))
cat <<EOF
============================================================
 Pinpoint C++ agent - collector-outage leak soak (macOS)
============================================================
Build        : $BUILD_DIR ($BUILD_TYPE)
Collector    : mock_collector_server on 127.0.0.1 $AGENT_PORT/$SPAN_PORT/$STAT_PORT
Load         : $MODE at $RPS RPS, max $CONCURRENCY in flight
Duration     : ${DURATION}s (${hours} h)  ~$(( RPS * DURATION )) requests
Chaos        : SIGKILL the collector every ${KILL_CYCLE}s, down ${DOWNTIME}s (~${cycles} cycles)
Leak checks  : \`leaks\` every ${LEAK_INTERVAL}s, stack logging=$STACK_LOGGING
Artifacts    : $OUT_DIR
Started      : $(now_iso)
============================================================
EOF

# ==========================================================================
# Stack
# ==========================================================================
export PINPOINT_CPP_CONFIG_FILE="${PINPOINT_CPP_CONFIG_FILE:-$SCRIPT_DIR/pinpoint-config.yaml}"
export PINPOINT_CPP_COLLECTOR_HOST="$HOST"
export PINPOINT_CPP_COLLECTOR_AGENT_PORT="$AGENT_PORT"
export PINPOINT_CPP_COLLECTOR_SPAN_PORT="$SPAN_PORT"
export PINPOINT_CPP_COLLECTOR_STAT_PORT="$STAT_PORT"
# info, not debug: an hour of debug lines is both a write-bandwidth tax on the
# thing being measured and ~0.2 GB of log. Reconnect messages are info-level.
export PINPOINT_CPP_LOG_LEVEL="${LEAK_SOAK_LOG_LEVEL:-info}"
export PINPOINT_CPP_LOG_FILE_PATH=""

SUFFIX="leaksoak-$(date +%Y%m%d-%H%M%S)-$$"
UPSTREAM_PID=""; DOWNSTREAM_PID=""; GRPC_PID=""
COLLECTOR_PID=""; SAMPLER_PID=""; CHAOS_PID=""; LOAD_PID=""
COLLECTOR_GEN=0

# The chaos loop runs in a subshell, so the COLLECTOR_PID and generation it
# assigns on a restart are invisible to the main shell that has to clean up at
# the end and to the sampler that reports them. The two files are the only
# handles all three share; without the pid file the last incarnation survives
# the run and blocks the ports of the next one, and without the generation
# file every heartbeat reports gen=1 for the whole run.
start_collector() {
    COLLECTOR_GEN=$(( COLLECTOR_GEN + 1 ))
    "$COLLECTOR_BIN" --agent-port "$AGENT_PORT" --span-port "$SPAN_PORT" \
        --stat-port "$STAT_PORT" --report-interval 30 \
        >>"$SLOG/mock_collector.log" 2>&1 &
    COLLECTOR_PID=$!
    echo "$COLLECTOR_PID" > "$COLLECTOR_PID_FILE"
    echo "$COLLECTOR_GEN" > "$COLLECTOR_GEN_FILE"
    local waited=0
    # The previous incarnation's sockets are gone with the process, but the
    # kernel still needs a moment to release the listeners; a restart that
    # silently failed to bind would make the rest of the run measure nothing.
    while (( waited < 30 )); do
        if grep -q "mock collector ready: agent=$AGENT_PORT .* pid=$COLLECTOR_PID" \
                "$SLOG/mock_collector.log" 2>/dev/null; then
            return 0
        fi
        kill -0 "$COLLECTOR_PID" 2>/dev/null || break
        sleep 1; waited=$(( waited + 1 ))
    done
    echo "collector generation $COLLECTOR_GEN failed to start" >&2
    tail -5 "$SLOG/mock_collector.log" >&2
    return 1
}

stop_stack() {
    for p in "$CHAOS_PID" "$SAMPLER_PID" "$LOAD_PID"; do
        [[ -n "$p" ]] && kill -TERM "$p" 2>/dev/null || true
    done
    curl -sS --max-time 5 -H 'Content-Length: 0' \
        -X POST "http://$HOST:$PORT/server/shutdown" >/dev/null 2>&1 || true
    curl -sS --max-time 5 -H 'Content-Length: 0' \
        -X POST "http://$HOST:$DOWNSTREAM_PORT/shutdown" >/dev/null 2>&1 || true
    local waited=0
    while [[ -n "$UPSTREAM_PID" ]] && kill -0 "$UPSTREAM_PID" 2>/dev/null && (( waited < 30 )); do
        sleep 1; waited=$(( waited + 1 ))
    done
    local live_collector=""
    [[ -f "$COLLECTOR_PID_FILE" ]] && live_collector=$(cat "$COLLECTOR_PID_FILE")
    for p in "$UPSTREAM_PID" "$DOWNSTREAM_PID" "$GRPC_PID" "$COLLECTOR_PID" "$live_collector"; do
        [[ -n "$p" ]] && kill -0 "$p" 2>/dev/null && kill "$p" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}

on_signal() {
    echo "" >&2
    echo "interrupted at $(now_iso) -- summarising what we have" >&2
    final_leak_checks || true
    stop_stack
    summarise 130
    exit 130
}

start_collector || exit 1
trap on_signal INT TERM

# MallocStackLogging is set on the agent processes only. It is what turns
# "1.2 kB leaked" into a named allocation site, and it costs memory in
# proportion to *live* allocations, not to every allocation ever made.
MALLOC_ENV=()
if $STACK_LOGGING; then
    MALLOC_ENV=(MallocStackLogging=1)
fi

env "${MALLOC_ENV[@]}" \
    PINPOINT_CPP_APPLICATION_NAME="cpp-it-grpc-downstream" \
    PINPOINT_CPP_AGENT_NAME="it-grpc-$SUFFIX" \
    "$E2E_DIR/grpc_server" "$GRPC_PORT" >"$SLOG/grpc_server.log" 2>&1 &
GRPC_PID=$!
env "${MALLOC_ENV[@]}" \
    PINPOINT_CPP_APPLICATION_NAME="cpp-it-http-downstream" \
    PINPOINT_CPP_AGENT_NAME="it-down-$SUFFIX" \
    "$E2E_DIR/http_downstream_server" "$DOWNSTREAM_PORT" \
    >"$SLOG/http_downstream_server.log" 2>&1 &
DOWNSTREAM_PID=$!
env "${MALLOC_ENV[@]}" \
    GRPC_TARGET="$HOST:$GRPC_PORT" HTTP_TARGET="$HOST:$DOWNSTREAM_PORT" \
    PINPOINT_CPP_APPLICATION_NAME="cpp-it-http-upstream" \
    PINPOINT_CPP_AGENT_NAME="it-up-$SUFFIX" \
    "$E2E_DIR/it_test_server" "$PORT" >"$SLOG/it_test_server.log" 2>&1 &
UPSTREAM_PID=$!

ready=""
for _ in $(seq 1 90); do
    kill -0 "$UPSTREAM_PID" 2>/dev/null || { echo "upstream died on startup" >&2; tail -20 "$SLOG/it_test_server.log" >&2; exit 1; }
    ready=$(curl -sS --max-time 3 "http://$HOST:$PORT/ready" 2>/dev/null || true)
    [[ "$ready" == *'"agent_enabled":true'* ]] && break
    sleep 1
done
[[ "$ready" == *'"agent_enabled":true'* ]] || { echo "agent never became ready" >&2; exit 1; }
for l in it_test_server http_downstream_server grpc_server; do
    grep -q 'success to register the agent' "$SLOG/$l.log" \
        || { echo "$l did not register with the mock collector" >&2; exit 1; }
done
echo "    three agents registered against the mock collector; upstream pid $UPSTREAM_PID"

# `leaks` failing is a setup problem (unsigned binary, permissions, SIP), and
# finding that out an hour in would waste the whole run.
leaks "$UPSTREAM_PID" > "$OUT_DIR/leaks/preflight.txt" 2>&1 || true
if ! grep -q "total leaked bytes" "$OUT_DIR/leaks/preflight.txt"; then
    echo "PREFLIGHT FAIL: \`leaks $UPSTREAM_PID\` does not work here:" >&2
    tail -5 "$OUT_DIR/leaks/preflight.txt" >&2
    stop_stack; exit 2
fi
if grep -q "is not debuggable" "$OUT_DIR/leaks/preflight.txt"; then
    echo "PREFLIGHT FAIL: the agent process is not debuggable, so every leak" >&2
    echo "  count would be a meaningless zero. The get-task-allow re-signing" >&2
    echo "  did not take effect for $E2E_DIR/it_test_server." >&2
    stop_stack; exit 2
fi
echo "    leaks preflight: $(grep -o '[0-9]* leaks for [0-9]* total leaked bytes' "$OUT_DIR/leaks/preflight.txt" | tail -1)"

# ==========================================================================
# Chaos: kill the collector, let it stay dead, bring it back on the same ports
# ==========================================================================
chaos() {
    set +e
    local start n=0
    start=$(date +%s)
    echo "iso_time,elapsed_s,cycle,event,generation,pid" > "$CHAOS_LOG"
    local up_time=$(( KILL_CYCLE - DOWNTIME ))
    while kill -0 "$UPSTREAM_PID" 2>/dev/null; do
        sleep "$up_time"
        kill -0 "$UPSTREAM_PID" 2>/dev/null || break
        n=$(( n + 1 ))
        # SIGKILL, not SIGTERM: a collector that shuts its servers down
        # politely sends GOAWAY and lets streams finish, which is the one
        # outage shape the agent already handles in the gtest suite. The soak
        # is about the other one.
        printf '%s,%s,%s,%s,%s,%s\n' "$(now_iso)" "$(( $(date +%s) - start ))" \
            "$n" "kill" "$COLLECTOR_GEN" "$COLLECTOR_PID" >> "$CHAOS_LOG"
        kill -9 "$COLLECTOR_PID" 2>/dev/null
        wait "$COLLECTOR_PID" 2>/dev/null
        sleep "$DOWNTIME"
        kill -0 "$UPSTREAM_PID" 2>/dev/null || break
        if start_collector; then
            printf '%s,%s,%s,%s,%s,%s\n' "$(now_iso)" "$(( $(date +%s) - start ))" \
                "$n" "restart" "$COLLECTOR_GEN" "$COLLECTOR_PID" >> "$CHAOS_LOG"
        else
            printf '%s,%s,%s,%s,%s,%s\n' "$(now_iso)" "$(( $(date +%s) - start ))" \
                "$n" "restart-failed" "$COLLECTOR_GEN" "" >> "$CHAOS_LOG"
            break
        fi
    done
}

# ==========================================================================
# Sampling: cheap resource series every SAMPLE_INTERVAL, `leaks` every
# LEAK_INTERVAL. They share one loop so a leak snapshot is always aligned with
# a resource row.
# ==========================================================================
leak_snapshot() {
    local pid=$1 tag=$2 elapsed=$3 out
    out="$OUT_DIR/leaks/$tag.txt"
    # leaks exits non-zero when it finds leaks, which is the normal case here.
    leaks "$pid" > "$out" 2>&1 || true
    local line count bytes
    if grep -q "is not debuggable" "$out"; then
        echo "0,0,not-debuggable"
        return
    fi
    line=$(grep -o '[0-9]* leaks for [0-9]* total leaked bytes' "$out" | tail -1)
    if [[ -z "$line" ]]; then
        echo "0,0,failed"
        return
    fi
    count=$(awk '{print $1}' <<<"$line")
    bytes=$(awk '{print $4}' <<<"$line")
    echo "$count,$bytes,ok"
}

sampler() {
    # Never let a failed probe kill the loop: the series matters most exactly
    # when something has started to go wrong.
    set +e
    echo "iso_time,elapsed_s,rss_kb,vsz_kb,threads,fds,cpu_s,agent_enabled,total_requests,collector_gen" > "$CSV"
    echo "iso_time,elapsed_s,tag,leak_count,leaked_bytes,status,rss_kb" > "$LEAK_CSV"
    local start n=0
    start=$(date +%s)
    local next_leak=$LEAK_INTERVAL
    while kill -0 "$UPSTREAM_PID" 2>/dev/null; do
        local rss vsz thr fds cpu now elapsed st enabled total
        rss=$(ps -o rss= -p "$UPSTREAM_PID" 2>/dev/null | tr -d ' '); : "${rss:=0}"
        vsz=$(ps -o vsz= -p "$UPSTREAM_PID" 2>/dev/null | tr -d ' '); : "${vsz:=0}"
        # macOS ps has no thcount; -M lists one row per thread after a header.
        thr=$(ps -M -p "$UPSTREAM_PID" 2>/dev/null | tail -n +2 | wc -l | tr -d ' ')
        # An fd leak is a classic soak finding and is invisible in both RSS and
        # `leaks`: a socket the agent forgot to close is still referenced.
        fds=$(lsof -p "$UPSTREAM_PID" 2>/dev/null | tail -n +2 | wc -l | tr -d ' ')
        cpu=$(ps -o time= -p "$UPSTREAM_PID" 2>/dev/null | awk -F: '
            {gsub(/ /,""); if (NF==3) printf "%.2f", $1*3600+$2*60+$3; else if (NF==2) printf "%.2f", $1*60+$2; else print 0}')
        : "${cpu:=0}"
        st=$(curl -sS --max-time 5 "http://$HOST:$PORT/stats" 2>/dev/null || echo '{}')
        enabled=$(grep -o '"agent_enabled":[a-z]*' <<<"$st" | cut -d: -f2); : "${enabled:=unknown}"
        total=$(grep -o '"total_requests":[0-9]*' <<<"$st" | cut -d: -f2); : "${total:=0}"
        now=$(date +%s); elapsed=$(( now - start ))
        local gen; gen=$(cat "$COLLECTOR_GEN_FILE" 2>/dev/null); : "${gen:=0}"
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$(now_iso)" "$elapsed" \
            "$rss" "$vsz" "$thr" "$fds" "$cpu" "$enabled" "$total" "$gen" >> "$CSV"

        if (( elapsed >= next_leak )); then
            next_leak=$(( next_leak + LEAK_INTERVAL ))
            local tag="t$(printf '%05d' "$elapsed")"
            local res; res=$(leak_snapshot "$UPSTREAM_PID" "$tag" "$elapsed")
            printf '%s,%s,%s,%s,%s\n' "$(now_iso)" "$elapsed" "$tag" "$res" "$rss" >> "$LEAK_CSV"
            printf '[%s] +%02d:%02d  rss=%d MB  thr=%s  fds=%s  agent=%s  req=%s  gen=%s  leaks=%s blocks / %s B\n' \
                "$(date +%H:%M:%S)" "$(( elapsed/3600 ))" "$(( (elapsed%3600)/60 ))" \
                "$(( rss / 1024 ))" "$thr" "$fds" "$enabled" "$total" "$gen" \
                "$(cut -d, -f1 <<<"$res")" "$(cut -d, -f2 <<<"$res")"
        fi
        n=$(( n + 1 ))
        sleep "$SAMPLE_INTERVAL"
    done
}

final_leak_checks() {
    # The downstream agents run the same agent code under a lighter load; a
    # single end-of-run check on each is nearly free and catches a leak that
    # only the server-side (no outbound calls) path reaches.
    for spec in "upstream:$UPSTREAM_PID" "downstream:$DOWNSTREAM_PID" "grpc:$GRPC_PID"; do
        local name=${spec%%:*} pid=${spec##*:}
        [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null || continue
        local res; res=$(leak_snapshot "$pid" "final-$name" 0)
        echo "  final leaks [$name pid $pid]: $(cut -d, -f1 <<<"$res") blocks / $(cut -d, -f2 <<<"$res") bytes"
        if [[ "$name" == "upstream" ]]; then
            printf '%s,%s,%s,%s,%s\n' "$(now_iso)" "final" "final-upstream" "$res" \
                "$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')" >> "$LEAK_CSV"
        fi
    done
}

summarise() {
    local status=$1
    echo ""
    echo "============================================================"
    echo " Leak soak summary   (load phase exit $status)"
    echo "============================================================"
    [[ -s "$LOAD_LOG" ]] && sed -n '/Results/,$p' "$LOAD_LOG" | grep -vE '^\s*[0-9]+\.[0-9]+ \|'
    echo ""
    if [[ -s "$CHAOS_LOG" ]]; then
        local kills restarts failed
        kills=$(grep -c ',kill,' "$CHAOS_LOG" || true)
        restarts=$(grep -c ',restart,' "$CHAOS_LOG" || true)
        failed=$(grep -c ',restart-failed,' "$CHAOS_LOG" || true)
        echo "Collector chaos: $kills kills, $restarts restarts, $failed failed restarts"
    fi
    # The collector-side proof that each restart was actually reached: every
    # incarnation starts its counters at zero, so a generation whose last
    # report still shows span_batches=0 is one the agent never came back to.
    # Without this, a run where the agent gave up after the first kill looks
    # exactly like a clean one -- flat RSS, no leaks, load still passing.
    if [[ -s "$SLOG/mock_collector.log" ]]; then
        echo ""
        echo "Per-incarnation collector traffic (last report of each):"
        awk '
            /mock collector ready/ { gen++; last[gen] = ""; next }
            /^\[\+/ { last[gen] = $0 }
            END {
                recovered = 0
                for (i = 1; i <= gen; i++) {
                    line = (last[i] == "" ? "  (no report before it was killed)" : last[i])
                    printf "  gen %-2d %s\n", i, line
                    if (last[i] ~ /span_batches=[1-9]/) recovered++
                }
                printf "  incarnations that received spans: %d of %d\n", recovered, gen
            }' "$SLOG/mock_collector.log"
    fi
    # The agent registers once per process; reconnects re-open the streams
    # rather than re-registering, so this is a startup check, not a per-cycle one.
    local reg
    reg=$(grep -c 'success to register the agent' "$SLOG/it_test_server.log" 2>/dev/null || echo 0)
    echo ""
    echo "Upstream AgentInfo registrations in the log: $reg"
    local reopened
    reopened=$(grep -c 'connect to command service stream' "$SLOG/it_test_server.log" 2>/dev/null || echo 0)
    echo "Command-stream (re)connects: $reopened"
    local warnfile="$OUT_DIR/warnings.txt" warns
    grep -hoiE '\[(warning|error)\]\[pinpoint\]\[[^]]+\] [^(]*' "$SLOG"/*.log 2>/dev/null \
        | sed 's/[[:space:]]*$//' | sort | uniq -c | sort -rn > "$warnfile" || true
    warns=$(awk '{s+=$1} END {print s+0}' "$warnfile")
    echo "warning/error log lines across the three server logs: $warns"
    [[ "$warns" -gt 0 ]] && sed -n '1,5p' "$warnfile" | sed 's/^/    /'

    python3 - "$CSV" "$LEAK_CSV" <<'PY'
import csv, os, sys

def rows(path):
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return []
    with open(path) as fh:
        return list(csv.DictReader(fh))

# Least-squares slope and its standard error, per hour. A last-minus-first
# delta is positive for anything that merely oscillates, so it cannot tell
# jitter from a trend; the standard error is what says whether the slope is
# bigger than its own noise.
def trend_per_hour(xs, ys):
    n = len(xs)
    if n < 3:
        return 0.0, float("inf")
    mx, my = sum(xs)/n, sum(ys)/n
    den = sum((x-mx)**2 for x in xs)
    if den == 0:
        return 0.0, float("inf")
    b = sum((x-mx)*(y-my) for x, y in zip(xs, ys))/den
    a = my - b*mx
    resid = sum((y-(a+b*x))**2 for x, y in zip(xs, ys))
    return b*3600, (((resid/(n-2))/den) ** 0.5)*3600

res = rows(sys.argv[1])
if len(res) >= 4:
    el  = [float(r["elapsed_s"]) for r in res]
    rss = [float(r["rss_kb"])/1024 for r in res]
    hours = el[-1]/3600
    print(f"\nResource series: {len(res)} samples over {hours:.2f} h")
    print(f"  RSS     first {rss[0]:8.1f} MB  max {max(rss):8.1f} MB  last {rss[-1]:8.1f} MB"
          f"  delta {rss[-1]-rss[0]:+.1f} MB")
    # Second half only: the first half carries allocator, cache and thread-pool
    # warm-up, a one-off step that reads as a trend when fitted from sample 0.
    h = len(res)//2
    for k in ("threads", "fds"):
        v = [float(r[k]) for r in res]
        sec = v[h:]
        drift, err = trend_per_hour(el[h:], sec)
        flag = "   <-- GROWING" if drift > 0.5 and drift > 2*err else ""
        print(f"  {k:7s} first {v[0]:8.0f}     max {max(v):8.0f}     last {v[-1]:8.0f}"
              f"  2nd half {min(sec):.0f}-{max(sec):.0f} at {drift:+.2f}+/-{err:.2f}/h{flag}")
    cpu = [float(r["cpu_s"]) for r in res]
    span = max(el[-1]-el[0], 1)
    print(f"  CPU     {cpu[-1]-cpu[0]:.1f} s over the series ({(cpu[-1]-cpu[0])/span*100:.2f} % of one core)")
    bad = [r["iso_time"] for r in res if r["agent_enabled"] != "true"]
    print(f"  agent_enabled != true at {len(bad)} of {len(res)} samples"
          + (f"   FIRST: {bad[0]}" if bad else ""))
    slope, _ = trend_per_hour(el[h:], rss[h:])
    print(f"  RSS slope, 2nd half only: {slope:+.3f} MB/h")
else:
    print("\nresource series too short to summarise")
    slope = 0.0

lk = [r for r in rows(sys.argv[2]) if r["status"] == "ok"]
print("\nLeak snapshots (macOS `leaks`, upstream agent process):")
if not lk:
    print("  none recorded")
    sys.exit()
print(f"  {'elapsed':>9}  {'blocks':>8}  {'bytes':>12}  {'rss_mb':>8}")
for r in lk:
    el_s = r["elapsed_s"]
    el_txt = el_s if el_s == "final" else f"{int(float(el_s))}s"
    rss_mb = float(r["rss_kb"] or 0)/1024
    print(f"  {el_txt:>9}  {int(r['leak_count']):>8}  {int(r['leaked_bytes']):>12}  {rss_mb:>8.1f}")

timed = [r for r in lk if r["elapsed_s"] != "final"]
if len(timed) >= 3:
    el = [float(r["elapsed_s"]) for r in timed]
    by = [float(r["leaked_bytes"]) for r in timed]
    bl = [float(r["leak_count"]) for r in timed]
    # The first snapshot is the baseline, not a finding: one-shot allocations
    # made during start-up and never freed (gRPC/absl/protobuf singletons and
    # the like) are unreachable by design and will be reported at every
    # snapshot. What matters is whether the number climbs with the cycles.
    base_b, base_c = by[0], bl[0]
    growth_b, err_b = trend_per_hour(el, by)
    growth_c, _ = trend_per_hour(el, bl)
    print(f"\n  baseline (first snapshot): {int(base_c)} blocks / {int(base_b)} bytes")
    print(f"  last snapshot:             {int(bl[-1])} blocks / {int(by[-1])} bytes"
          f"   delta {by[-1]-base_b:+.0f} bytes / {bl[-1]-base_c:+.0f} blocks")
    print(f"  leaked-byte trend: {growth_b:+.0f}+/-{err_b:.0f} B/h, blocks {growth_c:+.1f}/h")
    leaking = by[-1] > base_b and growth_b > 2*err_b and growth_b > 1024
    print("")
    if leaking:
        print(f"  VERDICT: LEAK -- leaked bytes grow with the outage cycles "
              f"(~{growth_b/1024:.1f} KiB/h, {growth_b*24/1048576:.2f} MiB/day).")
        print("           Open the newest leaks/t*.txt for the allocation sites.")
    elif by[-1] > base_b:
        print("  VERDICT: no leak trend -- leaked bytes moved but the change does "
              "not stand clear\n           of its own noise; treat as a one-off, "
              "not per-cycle, growth.")
    else:
        print("  VERDICT: no leak -- leaked bytes flat or lower than the baseline "
              "across the run.")
    if slope > 0.5:
        print(f"  NOTE: RSS still rises {slope:+.2f} MB/h with flat leak counts -- "
              "that is\n        retained-but-reachable memory (cache/queue/"
              "fragmentation), not a leak.")
else:
    print("\n  too few snapshots for a trend; lower --leak-interval or lengthen the run")
PY

    cat <<EOF

Artifacts:
  load log        $LOAD_LOG
  resource CSV    $CSV
  leak CSV        $LEAK_CSV
  leak reports    $OUT_DIR/leaks/
  chaos log       $CHAOS_LOG
  server logs     $SLOG
Finished        $(now_iso)
EOF
    return 0
}

# ==========================================================================
# Run
# ==========================================================================
sampler & SAMPLER_PID=$!
# The chaos loop's stderr is the shell's own "Killed: 9" job report for a
# collector it killed on purpose; keep it out of the heartbeat.
chaos 2>>"$SLOG/chaos.stderr" & CHAOS_PID=$!

set +e
python3 "$SCRIPT_DIR/load_test.py" \
    --base-url "http://$HOST:$PORT" \
    --mode "$MODE" --rps "$RPS" --duration "$DURATION" \
    --concurrency "$CONCURRENCY" --max-error-rate "$MAX_ERROR_RATE" \
    --rps-tolerance "$RPS_TOLERANCE" \
    --rss-pid "$UPSTREAM_PID" > "$LOAD_LOG" 2>&1 &
LOAD_PID=$!
wait "$LOAD_PID"
STATUS=$?
set -e
LOAD_PID=""

for p in CHAOS_PID SAMPLER_PID; do
    kill "${!p}" 2>/dev/null || true
    wait "${!p}" 2>/dev/null || true
done
CHAOS_PID=""; SAMPLER_PID=""

echo ""
echo "--- final leak checks ---"
final_leak_checks
stop_stack
trap - INT TERM

summarise "$STATUS"
exit "$STATUS"
