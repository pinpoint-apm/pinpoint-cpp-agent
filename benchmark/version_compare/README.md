# Cross-version public API benchmark

Compares two released agents — for example `v1.1.0` against `main` — by driving
each one through its public API the way an instrumented application does.

This is a different kind of benchmark from the ones in the parent directory.
Those embed a legacy implementation next to the current one inside a single
binary, so they can only compare two implementations that both exist in the
current tree. Comparing two *releases* needs the opposite arrangement: one
benchmark source, compiled once per agent version, run against a shared
collector on the same machine.

## Why a shim is needed

The public surface changed incompatibly between v1.1.0 and the current release
line, so one source cannot compile against both. `pp_compat.h` maps the two
spellings onto one set of operations:

| operation | v1.1.0 | current |
|---|---|---|
| start the agent | `SetConfigString()` + `CreateAgent()` | `StartAgent(AgentOptions)` + `GlobalAgent()` |
| span event handle | `shared_ptr<SpanEvent>` | `SpanEvent*` (non-owning) |
| end a span event | `span->EndSpanEvent()` | `event->EndEvent()` |
| annotate | `target->GetAnnotations()->Append*()` | `target->SetAnnotation()` |
| record SQL | `SetSqlQuery(sql, "a,b,c")` | `SetSqlQuery(sql, vector<SqlBindValue>)` |
| inject context | `span->InjectContext(w)` | `event->InjectContext(w)` |
| read trace id | `TraceId& GetTraceId()` | `std::string GetTraceId()` |
| header lookup | `optional<string> Get()` | `optional<string_view> Get()` |

Where one version makes the *caller* do work the other does not — joining SQL
bind arguments, copying a header value out of the carrier — that work stays
inside the timed region. It is a real cost of that version's API shape, and
hoisting it out of one side only would flatter that side.

## Why a collector is mandatory

Both versions gate span recording on successful AgentInfo registration and hand
out noop spans until it succeeds. Running without a collector measures the noop
path and nothing else, so `bench_collector` must be running first. The benchmark
aborts if `Enable()` never becomes true, and probes that a sampled span really
is sampled before measuring.

## Why span delivery has to be checked

A tight benchmark loop offers spans hundreds of times faster than any sender can
drain them. Once the span queue saturates, `EndSpan` stops serializing and
sending and starts taking a cheap drop path instead — so **the version whose
sender is slower measures as faster, because it is doing less work.**

An early run of this benchmark hit exactly that: at the shipped `Span.QueueSize`
of 1024, v1.1.0 delivered 1.05M spans and the current agent delivered 4.18M from
the same offered load. Read naively, the single-thread scenarios said the current
agent was 15–170% slower. It was not — it was the only one still doing the work.

Three things keep the comparison honest, and the report states the outcome of the
check before it shows any timing:

- `Span.QueueSize` is raised to 65536 so a scenario's generation burst fits.
  65536 is `MAX_SPAN_QUEUE_SIZE` in both versions; anything larger is rejected
  and silently reset to the 1024 default.
- Each scenario is followed by a drain pause (`--drain-ms`) so the next one
  starts from an empty queue instead of inheriting a backlog.
- The benchmark reports how many recording spans it created and the collector
  reports how many messages arrived. `aggregate.py` fails the run loudly when
  either version delivered less than it created.

Delivered messages legitimately *exceed* the span count: a span carrying many
events is split into a span plus chunk messages. The gate is
`delivered >= created`, not `== 100%`.

The cost of raising the queue is that peak RSS reflects that setting rather than
a shipped default, so the RSS figures are only meaningful relative to each other.

`test/it/mock_collector.h` is not reused here: it retains a full protobuf copy of
every message so integration tests can assert on payloads, which at benchmark
volumes would dominate memory and perturb the measurement. `bench_collector`
serves the same five services on the same three-port topology, counts what
arrives, and drops it.

## Scenarios

| scenario | what it targets |
|---|---|
| `s1_span_lifecycle` | one sampled span with three child events — metadata id caches, per-span allocation, queue enqueue |
| `s2_unsampled` | the continue-unsampled path, entered via an inbound `Pinpoint-Sampled: s0` |
| `s3_annotation_heavy` | annotation recording and its storage ownership |
| `s4a_sql_hit` | repeated statement: the raw SQL cache hit path |
| `s4b_sql_hit_binds` | same, plus four bind arguments, so the API shape difference is included |
| `s4c_sql_miss` | rotating pool larger than the cache: every lookup misses |
| `s5a_deep_events` | 30 nested events before unwinding |
| `s5b_wide_events` | 100 sequential events |
| `s6_threads_N` | `s1` at 1/2/4/8 threads — shared hot spots show up as ns/op growth |
| `s7_propagation` | outbound context injection |

`s5a`/`s5b` run a quarter of `--ops`, since each of their operations does 30–100
times the work of the others.

Metrics per scenario: `ns/op` (wall time ÷ ops *per thread*), sampled p50/p99/max
latency, allocations per op on the measuring thread, and process peak RSS.

Allocation counting replaces global `operator new` with a **thread-local**
counter. A global one would attribute the agent's own worker-thread allocations
to the measured loop.

## Controlling the measurement

Dependency versions must be pinned across both builds. Left to their own
CMakeLists, v1.1.0 fetches gRPC v1.63.1 and the current tree fetches v1.76.0,
which would confound every number that touches serialization or transport.
Build the dependencies once and point both agent builds at that prefix.

```bash
BASE=/tmp/pp-compare            # scratch location for worktrees and builds
GRPC_SRC=/path/to/grpc          # v1.76.0 checkout with submodules

git worktree add --detach "$BASE/wt/v1.1.0" v1.1.0
git worktree add --detach "$BASE/wt/main" main
```

```bash
cmake -S "$GRPC_SRC" -B "$BASE/grpc-rel" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$BASE/deps" \
  -DCMAKE_CXX_STANDARD=17 -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DgRPC_INSTALL=ON -DgRPC_BUILD_TESTS=OFF -DgRPC_BUILD_CSHARP_EXT=OFF \
  -DgRPC_ABSL_PROVIDER=module -DgRPC_PROTOBUF_PROVIDER=module \
  -DgRPC_ZLIB_PROVIDER=module -DgRPC_CARES_PROVIDER=module \
  -DgRPC_RE2_PROVIDER=module -DgRPC_SSL_PROVIDER=module \
  -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_INSTALL=ON \
  -Dutf8_range_ENABLE_INSTALL=ON -DABSL_ENABLE_INSTALL=ON \
  -DABSL_PROPAGATE_CXX_STD=ON
cmake --build "$BASE/grpc-rel" --target install -j 12
```

Install yaml-cpp 0.8.0 and fmt 11.2.0 into the same `$BASE/deps` prefix, then
configure both agents identically. v1.1.0 needs `-DVCPKG_DETECTED=ON`: its
non-vcpkg branch assumes the FetchContent layout and refers to a raw
`grpc_cpp_plugin` target that a `find_package`-provided gRPC does not define.

```bash
for V in main v1.1.0; do
  EXTRA=""; [ "$V" = "v1.1.0" ] && EXTRA="-DVCPKG_DETECTED=ON"
  cmake -S "$BASE/wt/$V" -B "$BASE/build-$V" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$BASE/deps" \
    -DCMAKE_CXX_STANDARD=17 -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON $EXTRA
  cmake --build "$BASE/build-$V" -j 12
done
```

Then build the benchmark against each:

```bash
cmake -S benchmark/version_compare -B "$BASE/bench-main" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$BASE/deps" \
  -DPINPOINT_SOURCE_DIR="$BASE/wt/main" -DPINPOINT_BUILD_DIR="$BASE/build-main" \
  -DPP_API_V1=OFF -DBUILD_COLLECTOR=ON
cmake --build "$BASE/bench-main" -j 12
```

```bash
cmake -S benchmark/version_compare -B "$BASE/bench-v1.1.0" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$BASE/deps" \
  -DPINPOINT_SOURCE_DIR="$BASE/wt/v1.1.0" -DPINPOINT_BUILD_DIR="$BASE/build-v1.1.0" \
  -DPP_API_V1=ON
cmake --build "$BASE/bench-v1.1.0" -j 12
```

## Running

```bash
./benchmark/version_compare/run_compare.sh \
  --baseline-bin "$BASE/bench-v1.1.0/api_benchmark" \
  --candidate-bin "$BASE/bench-main/api_benchmark" \
  --collector "$BASE/bench-main/bench_collector" \
  --ops 2500 --warmup 1000 --drain-ms 3500 --repeats 7 \
  --out ./version_compare_results
```

`--ops` is bounded from above by the delivery gate, not by patience: raising it
past what the senders can drain reintroduces the dropped-span artifact described
above. Buy precision with `--repeats` instead. The runner starts a fresh
collector per measured run so the delivery check is answerable per run.

The runs are interleaved (baseline, candidate, baseline, candidate, ...) rather
than done in blocks. On a laptop, thermal drift over a few minutes is large
enough that all-A-then-all-B would report it as a version difference.
`aggregate.py` takes medians across repetitions and prints the min-max spread
next to each one; a change smaller than that spread is noise, not a result.

Run on an otherwise idle machine, on mains power.

## Phase 2: HTTP load comparison

`bench_http_server.cpp` serves the endpoint contract of
`test/e2e/fixed_rps_test.py` (`/simple /deep /wide /annotated /features /mixed
/error /db-*` plus `/stats` and `/ready`) through the same shim, so one server
source compiles against both agent versions. `--disable` starts it with
`Enable: false`: the handlers make identical API calls but get noop spans,
which is the baseline that separates harness cost from agent overhead.

```bash
./benchmark/version_compare/run_load_compare.sh \
  --baseline-server "$BASE/bench-v1.1.0/bench_http_server" \
  --candidate-server "$BASE/bench-main/bench_http_server" \
  --collector "$BASE/bench-main/bench_collector" \
  --load-script test/e2e/fixed_rps_test.py \
  --rps-list "1000 4000" --duration 30 --repeats 3 \
  --out ./load_compare_results
```

The orchestrator interleaves four variants per repetition (each version ×
enabled/disabled), runs every RPS level against a fresh server and collector,
samples the server's CPU%/RSS every 0.5 s during each pass, and aggregates with
`aggregate_load.py`. Read the *agent overhead* rows (enabled − disabled of the
same binary), not the absolute latencies: the absolute numbers include the
Python generator and loopback, which are identical across variants.

Keep the offered rate well inside the generator's capacity (it self-reports
dropped arrivals; ~5000 RPS is the ceiling for the stdlib generator on this
class of machine) and check the span-delivery table first, same as phase 1.

## Reading the results

- `ns/op` for the threaded rows is wall time divided by ops *per thread*. A flat
  value as thread count rises means the path is not accumulating cross-core
  contention; growth means it is.
- Thread counts stop at 8 on purpose. The agent runs its own gRPC worker threads
  in the same process, so higher counts measure scheduler oversubscription
  rather than the tracing path.
- Check the delivery gate first. If it failed, nothing below it means anything.
- Cross-check `s1_span_lifecycle` against `s6_threads_1`: they run the same
  workload, so a large gap between them is a sign the run is not in steady state
  (too little warmup, or a saturated queue) rather than a version difference.
- The two versions use different span transports: v1.1.0 streams through
  `SendSpan`, the current agent batches through the unary `SendSpanBatch`. That
  is a real behavioral difference between the releases, not a harness artifact,
  but it means the sender-side load on the collector is not like-for-like.
