#!/usr/bin/env bash
#
# Long-running fixed-rate soak for the live e2e stack.
# Defaults to the 12 h / 100 RPS / full-mode / 30 %-sampling shape.
#
# Why this does not simply pass --load-duration 43200 to run_e2e.sh: that
# script's correctness phases and a reduced-sampling soak are mutually
# exclusive.
#
#   * smoke_test.sh asserts "sampled":true, "propagated":true and
#     parent_span_matches per request. Those hold only when every request is
#     sampled; at 30 % it fails ~12 assertions for reasons that are not bugs.
#   * run_e2e.sh's transport evidence greps the upstream log for
#     "SendSpanBatch success", which is a *debug*-level line. A 12 h run cannot
#     afford debug logging (measured: ~174 B/request across the three logs,
#     ~0.75 GB over 12 h, plus the CPU and write() bandwidth to format it, which
#     perturbs the very drift the run exists to measure -- info is ~6 B/request).
#
# So the run is two phases: the documented suite first, unmodified and at full
# sampling, as a gate; then the soak proper at the requested sampling and a
# quiet log level, with the stack driven directly.
#
# The soak's real output is the resource time series, not a pass/fail. A single
# first/max/last RSS triple cannot tell "warmed up, then flat" from "leaking
# slowly"; 720 samples can.
#
# Usage:
#   export PINPOINT_CPP_COLLECTOR_HOST=your-collector-host
#   ./test/e2e/soak_test.sh --build-dir ./build/default/test/e2e
#
# Detached (survives logout); stdout is only a heartbeat, every artifact lands
# in the output directory:
#   nohup ./test/e2e/soak_test.sh -b ./build/default/test/e2e >/dev/null 2>&1 &
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# --- the requested shape, all overridable ---------------------------------
BUILD_DIR=""
OUT_DIR=""
MODE="full"
RPS=100
DURATION=43200            # 12 h
SAMPLING_RATE="30.0"      # percent of requests traced
# With --rps this is the in-flight ceiling, not a worker count: arrivals are
# dropped once it is reached.
#
# It must stay BELOW the server's httplib worker count, which is
# CPPHTTPLIB_THREAD_POOL_COUNT = max(8, nproc-1) -- 19 on a 20-core host.
# cpp-httplib is thread-per-connection and load_test.py reuses keep-alive
# connections, so a ceiling above the pool size means the excess connections are
# accepted but never assigned a worker: they sit in the backlog until the client
# gives up. A 12 h run at 200 produced exactly that -- 4.89 % of requests timing
# out at 30 s, in-flight pinned at ~182, /stats unreachable for 11 h -- while
# throughput held 100 RPS, because the ~19 connections that owned a worker were
# served in under a millisecond. That is a harness artefact, not agent latency.
#
# Sized to the pool, a latency spike costs dropped arrivals (bounded by
# --rps-tolerance, 5 % by default) instead of silent 30 s hangs.
CONCURRENCY=16
# load_test.py evaluates this once, after the run -- never mid-flight -- so it
# only decides whether one transient blip in 4.3 M requests marks the soak
# failed. 0 (its default) is the wrong choice for a 12 h unattended run.
MAX_ERROR_RATE="0.1"
SAMPLE_INTERVAL=60
HEARTBEAT_EVERY=30        # samples between stdout heartbeats
RUN_VALIDATE=true
HOST=127.0.0.1
PORT=8090
DOWNSTREAM_PORT=8091
GRPC_PORT=50051

usage() {
    sed -n '3,4p;27,33p' "$0" | sed 's/^#\{1,2\} \{0,1\}//'
    cat <<EOF
Options:
  -b, --build-dir DIR     Directory containing the e2e binaries (required)
      --out-dir DIR       Artifact directory (default: ./soak-<timestamp>)
      --mode MODE         load_test.py mode (default: $MODE)
      --rps RPS           Fixed arrival rate (default: $RPS)
      --duration SEC      Soak duration (default: $DURATION = 12h)
      --sampling-rate PCT Trace sampling percent (default: $SAMPLING_RATE)
      --concurrency N     Max in-flight requests (default: $CONCURRENCY)
      --max-error-rate P  Tolerated final error rate (default: $MAX_ERROR_RATE)
      --sample-interval S Resource sampling period (default: $SAMPLE_INTERVAL)
      --skip-validate     Skip the phase-1 smoke gate (not recommended)
      --port N            Upstream port (default: $PORT)
      --downstream-port N Downstream port (default: $DOWNSTREAM_PORT)
      --grpc-port N       gRPC port (default: $GRPC_PORT)
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
        --sampling-rate)    SAMPLING_RATE=$2; shift 2 ;;
        --concurrency)      CONCURRENCY=$2; shift 2 ;;
        --max-error-rate)   MAX_ERROR_RATE=$2; shift 2 ;;
        --sample-interval)  SAMPLE_INTERVAL=$2; shift 2 ;;
        --skip-validate)    RUN_VALIDATE=false; shift ;;
        --port)             PORT=$2; shift 2 ;;
        --downstream-port)  DOWNSTREAM_PORT=$2; shift 2 ;;
        --grpc-port)        GRPC_PORT=$2; shift 2 ;;
        -h|--help)          usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# ==========================================================================
# Preflight -- every check here would otherwise surface hours in
# ==========================================================================
fail() { echo "PREFLIGHT FAIL: $*" >&2; exit 2; }

[[ -n "${PINPOINT_CPP_COLLECTOR_HOST:-}" ]] || fail "PINPOINT_CPP_COLLECTOR_HOST must be set."

if [[ -z "$BUILD_DIR" ]]; then
    for c in "$PROJECT_DIR/build/default/test/e2e" "$PROJECT_DIR/build/profiling/test/e2e"; do
        [[ -x "$c/it_test_server" ]] && { BUILD_DIR=$c; break; }
    done
fi
[[ -n "$BUILD_DIR" ]] || fail "could not locate e2e binaries; pass --build-dir."
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
for b in it_test_server http_downstream_server grpc_server c_api_scenario fork_scenario; do
    [[ -x "$BUILD_DIR/$b" ]] || fail "missing binary: $BUILD_DIR/$b"
done
for v in RPS DURATION SAMPLE_INTERVAL CONCURRENCY; do
    [[ "${!v}" =~ ^[0-9]+$ && "${!v}" -gt 0 ]] || fail "--${v,,} must be a positive integer."
done

# The collector is the dependency that makes the whole run worthless if absent,
# and phase 1 would only discover it after the smoke suite has run.
timeout 5 bash -c "cat < /dev/null > /dev/tcp/$PINPOINT_CPP_COLLECTOR_HOST/9991" 2>/dev/null \
    || fail "collector $PINPOINT_CPP_COLLECTOR_HOST:9991 is not reachable."

for p in "$PORT" "$DOWNSTREAM_PORT" "$GRPC_PORT"; do
    timeout 1 bash -c "cat < /dev/null > /dev/tcp/127.0.0.1/$p" 2>/dev/null \
        && fail "port $p is already in use (another stack running?)."
done

: "${OUT_DIR:=$PROJECT_DIR/soak-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT_DIR"; OUT_DIR="$(cd "$OUT_DIR" && pwd)"
CSV="$OUT_DIR/resources.csv"
LOAD_LOG="$OUT_DIR/load.log"
VALIDATE_LOG="$OUT_DIR/validate.log"
mkdir -p "$OUT_DIR/logs"

# ~6 B/request at info across the three server logs, measured on this stack;
# +300 MB covers load.log, which gets one interval line per second.
need_mb=$(( RPS * DURATION * 6 / 1000000 + 300 ))
avail_mb=$(df -Pm "$OUT_DIR" | awk 'NR==2 {print $4}')
[[ "$avail_mb" -gt "$need_mb" ]] || fail "need ~${need_mb} MB in $OUT_DIR, only ${avail_mb} MB free."

hours=$(python3 -c "print(f'{$DURATION/3600:.2f}')")
cat <<EOF
============================================================
 Pinpoint C++ agent - fixed-rate soak
============================================================
Collector    : $PINPOINT_CPP_COLLECTOR_HOST
Binaries     : $BUILD_DIR
Load         : $MODE at $RPS RPS, max $CONCURRENCY in flight
Duration     : ${DURATION}s (${hours} h)  ~$(( RPS * DURATION )) requests
Sampling     : PERCENT $SAMPLING_RATE %
Error budget : $MAX_ERROR_RATE %
Artifacts    : $OUT_DIR
Started      : $(date -Is)
============================================================
EOF

# ==========================================================================
# Phase 1 -- the documented suite as a gate, at its own settings
# ==========================================================================
if $RUN_VALIDATE; then
    echo ""
    echo "--- phase 1/2: correctness gate (run_e2e.sh, full sampling, no load) ---"
    # No sampling override and no log-level override: the smoke assertions need
    # every request sampled, and the transport check needs debug-level lines.
    if env -u PINPOINT_CPP_SAMPLING_TYPE -u PINPOINT_CPP_SAMPLING_PERCENT_RATE \
           -u PINPOINT_CPP_LOG_LEVEL \
           "$SCRIPT_DIR/run_e2e.sh" --build-dir "$BUILD_DIR" \
           --port "$PORT" --downstream-port "$DOWNSTREAM_PORT" --grpc-port "$GRPC_PORT" \
           --log-dir "$OUT_DIR/logs/validate" --keep-logs \
           > "$VALIDATE_LOG" 2>&1; then
        echo "    $(grep -m1 'Smoke results' "$VALIDATE_LOG" || echo 'smoke passed')"
    else
        echo "phase 1 FAILED -- not starting a ${hours} h soak on a broken stack." >&2
        grep -E "FAIL|Smoke results" "$VALIDATE_LOG" | head -20 >&2
        echo "full log: $VALIDATE_LOG" >&2
        exit 1
    fi
fi

# ==========================================================================
# Phase 2 -- the soak
# ==========================================================================
export PINPOINT_CPP_COLLECTOR_HOST
export PINPOINT_CPP_CONFIG_FILE="${PINPOINT_CPP_CONFIG_FILE:-$SCRIPT_DIR/pinpoint-config.yaml}"
export PINPOINT_CPP_SAMPLING_TYPE="PERCENT"
export PINPOINT_CPP_SAMPLING_PERCENT_RATE="$SAMPLING_RATE"
export PINPOINT_CPP_LOG_LEVEL="${SOAK_LOG_LEVEL:-info}"
export PINPOINT_CPP_LOG_FILE_PATH=""

SUFFIX="soak-$(date +%Y%m%d-%H%M%S)-$$"
SLOG="$OUT_DIR/logs"
UPSTREAM_PID=""; DOWNSTREAM_PID=""; GRPC_PID=""; SAMPLER_PID=""; LOAD_PID=""

stop_stack() {
    [[ -n "$SAMPLER_PID" ]] && kill "$SAMPLER_PID" 2>/dev/null || true
    [[ -n "$LOAD_PID" ]] && kill -TERM "$LOAD_PID" 2>/dev/null || true
    curl -sS --max-time 5 -H 'Content-Length: 0' \
        -X POST "http://$HOST:$PORT/server/shutdown" >/dev/null 2>&1 || true
    curl -sS --max-time 5 -H 'Content-Length: 0' \
        -X POST "http://$HOST:$DOWNSTREAM_PORT/shutdown" >/dev/null 2>&1 || true
    local waited=0
    while [[ -n "$UPSTREAM_PID" ]] && kill -0 "$UPSTREAM_PID" 2>/dev/null && (( waited < 30 )); do
        sleep 1; waited=$(( waited + 1 ))
    done
    for p in "$UPSTREAM_PID" "$DOWNSTREAM_PID" "$GRPC_PID"; do
        [[ -n "$p" ]] && kill -0 "$p" 2>/dev/null && kill "$p" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
on_signal() {
    echo "" >&2
    echo "interrupted at $(date -Is) -- stopping and summarising what we have" >&2
    stop_stack
    summarise 130
    exit 130
}
trap on_signal INT TERM

echo ""
echo "--- phase 2/2: ${hours} h soak at ${SAMPLING_RATE} % sampling, log level $PINPOINT_CPP_LOG_LEVEL ---"

PINPOINT_CPP_APPLICATION_NAME="cpp-it-grpc-downstream" \
PINPOINT_CPP_AGENT_NAME="it-grpc-$SUFFIX" \
    "$BUILD_DIR/grpc_server" "$GRPC_PORT" >"$SLOG/grpc_server.log" 2>&1 &
GRPC_PID=$!
PINPOINT_CPP_APPLICATION_NAME="cpp-it-http-downstream" \
PINPOINT_CPP_AGENT_NAME="it-down-$SUFFIX" \
    "$BUILD_DIR/http_downstream_server" "$DOWNSTREAM_PORT" \
    >"$SLOG/http_downstream_server.log" 2>&1 &
DOWNSTREAM_PID=$!
GRPC_TARGET="$HOST:$GRPC_PORT" HTTP_TARGET="$HOST:$DOWNSTREAM_PORT" \
PINPOINT_CPP_APPLICATION_NAME="cpp-it-http-upstream" \
PINPOINT_CPP_AGENT_NAME="it-up-$SUFFIX" \
    "$BUILD_DIR/it_test_server" "$PORT" >"$SLOG/it_test_server.log" 2>&1 &
UPSTREAM_PID=$!

ready=""
for _ in $(seq 1 60); do
    kill -0 "$UPSTREAM_PID" 2>/dev/null || { echo "upstream died on startup" >&2; tail -20 "$SLOG/it_test_server.log" >&2; exit 1; }
    ready=$(curl -sS --max-time 3 "http://$HOST:$PORT/ready" 2>/dev/null || true)
    [[ "$ready" == *'"agent_enabled":true'* ]] && break
    sleep 1
done
[[ "$ready" == *'"agent_enabled":true'* ]] || { echo "agent never became ready" >&2; exit 1; }

# "success to register the agent" is info-level, so this works at the soak's log
# level; "SendSpanBatch success" is debug-only and deliberately not asserted --
# span flow is watched instead through agent_enabled in the series below.
sleep 3
for l in it_test_server http_downstream_server grpc_server; do
    grep -q 'success to register the agent' "$SLOG/$l.log" \
        || { echo "$l did not register with the collector" >&2; exit 1; }
done
echo "    all three agents registered; upstream pid $UPSTREAM_PID"

sampler() {
    # A monitoring loop must outlive what it monitors. Previously this inherited
    # errexit+pipefail, so when /stats stopped answering, `grep -o` matched
    # nothing, the pipeline returned non-zero and the whole sampler exited
    # silently -- losing the series at exactly the moment it got interesting.
    set +e
    echo "iso_time,elapsed_s,rss_kb,vsz_kb,threads,fds,cpu_s,agent_enabled,total_requests" > "$CSV"
    local start n=0
    start=$(date +%s)
    while kill -0 "$UPSTREAM_PID" 2>/dev/null; do
        local stat cpu thr rss vsz fds now st enabled total
        stat=$(sed 's/.*) //' "/proc/$UPSTREAM_PID/stat" 2>/dev/null) || break
        cpu=$(awk '{printf "%.2f", ($12+$13)/100}' <<<"$stat")
        thr=$(awk '{print $18}' <<<"$stat")
        rss=$(awk '/^VmRSS:/  {print $2}' "/proc/$UPSTREAM_PID/status" 2>/dev/null || echo 0)
        vsz=$(awk '/^VmSize:/ {print $2}' "/proc/$UPSTREAM_PID/status" 2>/dev/null || echo 0)
        # An FD leak is a classic soak finding and is invisible in RSS.
        fds=$(ls "/proc/$UPSTREAM_PID/fd" 2>/dev/null | wc -l)
        # agent_enabled going false mid-soak is the single most important signal
        # here: the agent stopping silently looks identical to a healthy run in
        # every request-side metric.
        st=$(curl -sS --max-time 5 "http://$HOST:$PORT/stats" 2>/dev/null || echo '{}')
        enabled=$(grep -o '"agent_enabled":[a-z]*' <<<"$st" | cut -d: -f2); : "${enabled:=unknown}"
        total=$(grep -o '"total_requests":[0-9]*' <<<"$st" | cut -d: -f2); : "${total:=0}"
        now=$(date +%s)
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$(date -Is)" "$(( now - start ))" \
            "$rss" "$vsz" "$thr" "$fds" "$cpu" "$enabled" "$total" >> "$CSV"
        n=$(( n + 1 ))
        if (( n % HEARTBEAT_EVERY == 0 )); then
            printf '[%s] +%dh%02dm  rss=%d MB  thr=%s  fds=%s  cpu=%ss  agent=%s  req=%s\n' \
                "$(date +%H:%M:%S)" "$(( (now-start)/3600 ))" "$(( ((now-start)%3600)/60 ))" \
                "$(( rss / 1024 ))" "$thr" "$fds" "${cpu%.*}" "$enabled" "$total"
        fi
        sleep "$SAMPLE_INTERVAL"
    done
}

summarise() {
    local status=$1
    echo ""
    echo "============================================================"
    echo " Soak summary   (load phase exit $status)"
    echo "============================================================"
    [[ -s "$LOAD_LOG" ]] && sed -n '/Results/,$p' "$LOAD_LOG" | grep -vE '^\s*[0-9]+\.[0-9]+ \|'
    echo ""
    # Grouped by signature rather than counted: `full` mode deliberately crosses
    # the e2e config's reduced Span.MaxEventDepth/MaxEventSequence, so a bare
    # count reads as a problem when the commonest entry is an expected one. The
    # agent rate-limits repeats ("N occurrences since last"), so the line count
    # stays small over 12 h even though the event count does not.
    local warnfile="$OUT_DIR/warnings.txt" warns
    grep -hoiE '\[(warning|error)\]\[pinpoint\]\[[^]]+\] [^(]*' "$SLOG"/*.log 2>/dev/null \
        | sed 's/[[:space:]]*$//' | sort | uniq -c | sort -rn > "$warnfile" || true
    warns=$(awk '{s+=$1} END {print s+0}' "$warnfile")
    echo "warning/error log lines across the three server logs: $warns"
    [[ "$warns" -gt 0 ]] && sed -n '1,5p' "$warnfile" | sed 's/^/    /'
    if [[ -s "$CSV" ]]; then
        python3 - "$CSV" "$SAMPLE_INTERVAL" <<'PY'
import sys, csv
rows = list(csv.DictReader(open(sys.argv[1])))
if len(rows) < 4:
    print("resource series too short to summarise"); sys.exit()
num = lambda r, k: float(r[k])
el  = [num(r, "elapsed_s") for r in rows]
rss = [num(r, "rss_kb")/1024 for r in rows]
hours = el[-1]/3600
print(f"\nResource series: {len(rows)} samples over {hours:.2f} h")
print(f"  RSS     first {rss[0]:7.1f} MB   max {max(rss):7.1f} MB   last {rss[-1]:7.1f} MB"
      f"   delta {rss[-1]-rss[0]:+.1f} MB")
# Compare within the second half: the first half carries allocator/cache/thread
# warm-up, a one-off step that would read as a trend against sample 0.
h = len(rows)//2
for k in ("threads", "fds"):
    v = [num(r, k) for r in rows]
    drift = v[-1] - min(v[h:])
    print(f"  {k:7s} first {v[0]:7.0f}      max {max(v):7.0f}      last {v[-1]:7.0f}"
          f"   2nd-half drift {drift:+.0f}" + ("   <-- GROWING" if drift > 0 else ""))
cpu = [num(r, "cpu_s") for r in rows]
span = el[-1] - el[0]
print(f"  CPU     {cpu[-1]-cpu[0]:.1f} s over the series ({(cpu[-1]-cpu[0])/span*100:.2f} % of one core)")
reqs = num(rows[-1], "total_requests") - num(rows[0], "total_requests")
if reqs > 0:
    print(f"  CPU/req {(cpu[-1]-cpu[0])/reqs*1e6:.0f} us   over {reqs:,.0f} server-side requests")
bad = [r["iso_time"] for r in rows if r["agent_enabled"] != "true"]
print(f"  agent_enabled != true at {len(bad)} of {len(rows)} samples"
      + (f"   FIRST: {bad[0]}" if bad else ""))
xs, ys = el[h:], rss[h:]
n = len(xs); mx = sum(xs)/n; my = sum(ys)/n
den = sum((x-mx)**2 for x in xs)
slope = (sum((x-mx)*(y-my) for x, y in zip(xs, ys))/den if den else 0.0) * 3600
print(f"\n  RSS slope, 2nd half only: {slope:+.3f} MB/h")
if hours < 1:
    print("  Verdict: run too short for a leak verdict -- RSS is still warming up.")
elif abs(slope) < 0.5:
    print("  Verdict: flat -- no leak signal.")
else:
    print(f"  Verdict: sustained growth, ~{slope*24:+.1f} MB/day extrapolated -- investigate.")
PY
    fi
    cat <<EOF

Artifacts:
  load log       $LOAD_LOG
  resource CSV   $CSV
  server logs    $SLOG
  phase-1 log    $VALIDATE_LOG
Finished       $(date -Is)
EOF
    return 0
}

sampler &
SAMPLER_PID=$!

set +e
python3 "$SCRIPT_DIR/load_test.py" \
    --base-url "http://$HOST:$PORT" \
    --mode "$MODE" --rps "$RPS" --duration "$DURATION" \
    --concurrency "$CONCURRENCY" --max-error-rate "$MAX_ERROR_RATE" \
    --rss-pid "$UPSTREAM_PID" > "$LOAD_LOG" 2>&1 &
LOAD_PID=$!
wait "$LOAD_PID"
STATUS=$?
set -e
LOAD_PID=""

kill "$SAMPLER_PID" 2>/dev/null || true
wait "$SAMPLER_PID" 2>/dev/null || true
SAMPLER_PID=""
stop_stack
trap - INT TERM

summarise "$STATUS"
exit "$STATUS"
