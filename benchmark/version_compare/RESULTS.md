# v1.1.0 vs main — measured results

Produced by `run_compare.sh`; see [README.md](README.md) for the methodology and
reproduction steps.

## Environment

| | |
|---|---|
| machine | Apple M4 Pro, 10 performance + 4 efficiency cores, macOS 24.3.0 |
| compiler | Apple clang 16.0.0, `-O3`, `CMAKE_BUILD_TYPE=Release`, static libs |
| baseline | `v1.1.0` (`bdb52be`), agent reports `1.1.0.1001` |
| candidate | `main` (`acf8cff`), 284 commits ahead, agent reports `2.0.0.1001` |
| shared dependencies | gRPC 1.76.0 (+ its bundled protobuf/absl/re2/c-ares), yaml-cpp 0.8.0, fmt 11.2.0 — one install, both builds |
| collector | local counting collector, same process for both versions of a repetition pair |
| schedule | two independent runs of 7 interleaved repetitions (14 total), 2500 ops/thread/scenario (625 for `s5*`), 1000 warmup ops, 3500 ms drain between scenarios |
| agent config | sampling 1/1, `Span.QueueSize: 65536`, stats disabled, log level error |

Left to their own build systems v1.1.0 would have pulled gRPC v1.63.1 and main
v1.76.0. Both were pointed at one v1.76.0 install so no number below reflects a
dependency difference. v1.1.0 needs `-DVCPKG_DETECTED=ON` to accept a
`find_package`-provided gRPC.

The two runs were separate sessions hours apart. Their per-scenario `ns/op`
medians agree within a few percentage points everywhere (e.g. `s4a` -65.1% in
both, `s6_threads_8` -56.5% in both), so the median-based conclusions replicate.
The single-thread p99 columns do **not** replicate run to run (`s1` Δp99 +81% in
one run, +34% in the other) — that instability is itself a finding; see the
diagnosis below.

## Validity

| variant | recording spans created | messages delivered | ratio |
|---|---:|---:|---:|
| v1.1.0 | 76,751 | 86,501 | 112.7% |
| main | 76,751 | 86,501 | 112.7% |

Both variants delivered the identical message count in every one of the 14 runs,
so the per-span timings compare equal work. This check is not a formality — see
[README.md](README.md#why-span-delivery-has-to-be-checked) for the run where it
was violated and inverted the headline conclusion.

## Results

`ns/op` is wall time ÷ ops *per thread*. `±` is half the min-max range across the
14 repetitions.

| scenario | thr | v1.1.0 ns/op | main ns/op | Δ ns/op | v1.1.0 p99 | main p99 | Δ p99 | v1.1.0 alloc/op | main alloc/op |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| s1_span_lifecycle | 1 | 2,697 ±13% | 1,998 ±48% | **-25.9%** | 6,146 | 10,792 | `+75.6%` | 23.0 | 17.0 |
| s2_unsampled | 1 | 418 ±212% | 429 ±18% | `+2.7%` | 459 | 459 | +0.0% | 2.0 | 2.0 |
| s3_annotation_heavy | 1 | 2,829 ±30% | 1,986 ±51% | **-29.8%** | 6,688 | 7,416 | `+10.9%` | 31.0 | 17.0 |
| s4a_sql_hit | 1 | 4,111 ±24% | 1,432 ±50% | **-65.2%** | 8,188 | 5,604 | **-31.5%** | 17.0 | 11.0 |
| s4b_sql_hit_binds | 1 | 4,370 ±14% | 1,715 ±37% | **-60.7%** | 7,458 | 4,917 | **-34.1%** | 19.0 | 13.0 |
| s4c_sql_miss | 1 | 3,979 ±18% | 7,511 ±16% | `+88.8%` | 7,270 | 28,375 | `+290.3%` | 17.0 | 20.0 |
| s5a_deep_events | 1 | 10,820 ±18% | 14,133 ±42% | `+30.6%` | 14,188 | 56,312 | `+296.9%` | 82.0 | 57.0 |
| s5b_wide_events | 1 | 22,276 ±16% | 16,395 ±36% | **-26.4%** | 33,625 | 70,771 | `+110.5%` | 244.0 | 130.0 |
| s6_threads_1 | 1 | 2,912 ±23% | 2,152 ±41% | **-26.1%** | 6,854 | 7,146 | `+4.2%` | 23.0 | 17.0 |
| s6_threads_2 | 2 | 6,379 ±18% | 3,684 ±12% | **-42.3%** | 33,250 | 26,646 | **-19.9%** | 23.0 | 17.0 |
| s6_threads_4 | 4 | 14,386 ±3% | 6,912 ±12% | **-52.0%** | 69,646 | 53,312 | **-23.5%** | 23.0 | 17.0 |
| s6_threads_8 | 8 | 33,694 ±2% | 14,668 ±3% | **-56.5%** | 161,125 | 102,666 | **-36.3%** | 23.0 | 17.0 |
| s7_propagation | 1 | 2,587 ±19% | 1,551 ±37% | **-40.0%** | 5,375 | 5,438 | `+1.2%` | 14.0 | 11.0 |

Peak RSS: v1.1.0 157.4 MiB, main 67.2 MiB. Comparable to each other only — both
reflect the raised `Span.QueueSize`, not a shipped default.

## What holds up

**Thread scaling is the headline.** The advantage grows monotonically with thread
count — -26% at 1 thread, -42% at 2, -52% at 4, -56% at 8 — which is the
signature of removed shared hot spots rather than a constant-factor win.

How to read those numbers: `s6_threads_N` runs the `s1` workload (one sampled
span, three events, EndSpan enqueue) on N threads released from a single atomic
start gate, and `ns/op` is the wall time of the whole scenario divided by ops
*per thread*. Under perfect scaling N threads doing their 2,500 ops in parallel
finish in the same wall time as one thread doing 2,500 — a flat `ns/op` across
thread counts — so any growth is accumulated cross-core contention, not extra
work. Two derived views of the headline table:

| threads | v1.1.0 ns/op | ×1T | main ns/op | ×1T | v1.1.0 total spans/s | main total spans/s |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 2,912 | 1.00 | 2,152 | 1.00 | 343k | 465k |
| 2 | 6,379 | 2.19 | 3,684 | 1.71 | 314k | 543k |
| 4 | 14,386 | 4.94 | 6,912 | 3.21 | 278k | 579k |
| 8 | 33,694 | **11.57** | 14,668 | **6.82** | **237k** | **545k** |

The `×1T` column is the per-thread cost growth factor. v1.1.0's roughly doubles
with every doubling of threads — the classic signature of a serialized section
all threads queue behind. Converted to aggregate throughput (threads ×
10⁹/ns-per-op), v1.1.0 caps out around 240–340k spans/s and *declines* as
threads are added, i.e. a global lock has become the whole-process ceiling;
main holds ~465–580k/s. The serialized sections in question are what the main
line removed: the single-mutex span queue with a cv notify per enqueue
(`de40e4a` replaced it with a thread-sharded queue and batch drain), the one
`shared_mutex` in front of the metadata id caches whose lock word ping-pongs
between cores even for readers (`d5c341e` sharded it), the per-request config
snapshot refcount (`4541cb8` made it per-thread), plus false-sharing fixes
(`e06ede8`).

Reliability: the 8-thread rows have the tightest spreads in the whole table
(±2%/±3%) — contention is a highly repeatable phenomenon — the 8-thread delta
reproduced across three independent sessions (-56.5/-56.5/-56.3%), and p99
improves alongside the median at every thread count, consistent with shorter
lock waits. Two scope notes: thread counts stop at 8 because the agent's own
gRPC workers share the 10 performance cores, and these are hot-path creation
costs — scenario bursts are absorbed by the queue and drained between
scenarios, so "total spans/s" is the instrumentation cost the application
threads can bear, not a sustained end-to-end delivery rate.

**The raw SQL cache does what it was built for.** `s4a_sql_hit` -65% and
`s4b_sql_hit_binds` -61%, with no overlap between the two versions' ranges across
14 repetitions. Attributable to `11225cd` (enable the raw SQL cache by default)
and `488ba1e` (allocation-free bind joining).

**Allocations per operation are down across the board** — 23→17 on a plain
request span, 31→17 with annotations, 244→130 on the 100-event span. This is the
most robust measurement in the set, since it is a count rather than a timing.

## What does not

**`s4c_sql_miss` is a real regression: +89%.** The two versions' ranges do not
overlap, and p99 is ~4× worse. On a zero-hit-rate workload the front cache pays
hash, insert, and eviction churn on top of normalization and saves nothing. This
is the documented trade-off in [../README.md](../README.md#raw-sql-prepared-cache),
now measured end to end at the public API: applications that inline literals
instead of binding parameters are slower on this version than on v1.1.0.

**`s5a_deep_events` (30-deep nesting) regresses: +31% median across 14 reps.**
The ranges overlap (v1.1.0 8.5–12.3 µs, main 8.6–20.4 µs) but main's whole
distribution is shifted up and its spread is much wider. Deep nesting produces
the largest span chunks, which makes it the scenario most exposed to the
batch-burst mechanism diagnosed below.

**Single-thread p99 is worse and unstable — diagnosed.** Where medians improved,
tails often regressed (`s1` +76%, `s5a` +297%, `s5b` +111%), and these columns do
not replicate between runs. A targeted investigation (per-op trace with phase
decomposition, batch-size sweep, allocator-zone toggle; tools:
`api_benchmark --scenario --trace`, `analyze_trace.py`) established the cause:

- The stalls are 10–70 µs windows in which the application thread runs the *same
  work slower* (identical allocs/op), in runs of consecutive operations, landing
  in whichever phase of the span lifecycle is executing — i.e. external
  interference, not a slow code path.
- They are **not** the unary `SendSpanBatch` RPC itself: with batch size 1 (20×
  more RPCs) the tail is the *best* measured (p99 3.7 µs); with batch 200 it is
  the worst (15 µs). The tail scales with the length of the sender's contiguous
  work bursts, not the number of sends.
- They are **not** the producer→worker cv notify: phase attribution spreads the
  spike excess across all phases including ones that never touch the queue, and
  the saturated regime (worker never sleeps, zero notifies) shows the same tail.
- They **are** allocator lock contention: the sender worker serializes and then
  frees an entire batch of producer-allocated span chunks in one burst
  (cross-thread frees), colliding with the app thread's ~17 allocations/op on
  macOS libmalloc's locked zones. Forcing the lock-free nano zone
  (`MallocNanoZone=1`) collapses main's p99 from ~10 µs to a tight 4.4 µs —
  *better than v1.1.0* — and disabling it (`=0`) makes the tail worst of all.
  The ambient default sits between the two, which is also why the p99 columns
  vary so much run to run.

v1.1.0 has the same cross-thread free pattern but sends span-by-span on a
stream, so its allocator traffic interleaves finely instead of arriving in
bursts — narrow collision windows, clean tail, at the cost of the worse medians
and scaling above.

## Inconclusive

- `s2_unsampled` (+2.7%, v1.1.0 spread ±212%): no signal. The unsampled path is
  ~420 ns and dominated by measurement overhead at this resolution.

## Caveats

- The p99 magnitudes are macOS/libmalloc-specific. The structural hazard
  (batch-bursty cross-thread frees contending with app-thread allocation) exists
  on glibc too, but the numbers must be re-measured on the deployment platform;
  under jemalloc/tcmalloc the effect likely shrinks substantially.
- Improvement directions the diagnosis suggests: pool/recycle span chunks
  instead of freeing them on the worker, trim the ~17 allocs/op on the request
  path, or spread batch destruction between collect waits.
- Side observation from profiling: `AgentStats::addActiveSpan` performs a
  hash-map insert (with allocation) on every `NewSpan` even with
  `Stat.Enable: false` — a candidate for gating. (Addressed upstream by the
  intrusive active-span rework measured in the `6da2fdd` section below.)
- Measured on a laptop. The thread-scaling shape and the allocation counts should
  transfer; absolute nanoseconds should not.

## HTTP load comparison (phase 2)

The API microbenchmark above measures a tight loop; this pass measures a served
HTTP request. `bench_http_server` (one source, both versions via the shim,
identical embedded httplib, 8 worker threads) is driven by
`test/e2e/fixed_rps_test.py --mode mixed` on loopback: 30 s per pass, 3
interleaved repetitions, four variants — each version with the agent enabled
and with `Enable: false`. The enabled−disabled delta of the *same binary* is
the agent's overhead; comparing those deltas across versions cancels the
harness (Python client, httplib, loopback) out of the comparison.

Validity: enabled variants delivered ~268k span messages per run for ~151k
requests (chunk splitting plus `/features` async spans put delivery above the
request count); both `-noagent` variants delivered exactly 0.

### 4000 RPS (the clean pass)

| variant | achieved RPS | p50 ms | p99 ms | CPU % | RSS MiB |
|---|---:|---:|---:|---:|---:|
| v1.1.0 | 3,999 | 0.110 | 0.240 | 35.3 | 15.6 |
| main | 3,999 | 0.100 | 0.260 | 19.6 | 32.9 |
| v1.1.0 noagent | 3,999 | 0.100 | 0.210 | 12.4 | 11.3 |
| main noagent | 3,999 | 0.100 | 0.200 | 12.3 | 11.5 |

Agent overhead (enabled − disabled):

| version | Δp50 ms | Δp99 ms | ΔCPU |
|---|---:|---:|---:|
| v1.1.0 | +0.010 | +0.030 | **+22.8 pt** |
| main | +0.000 | +0.060 | **+7.3 pt** |

At 1000 RPS the CPU deltas are +8.9 pt (v1.1.0) vs +3.0 pt (main) — the same
~3× ratio.

**CPU per request is the phase-2 headline: main's agent costs ~⅓ of v1.1.0's
CPU at the same offered load.** The two noagent baselines are within 0.1 pt of
each other, so the delta is all agent.

**The microbenchmark's p99 regression does not materialize at realistic
rates.** At 4000 RPS the agent adds ≤0.06 ms to p99 on either version. The
phase-1 tail came from allocator collisions with the sender's batch bursts
under back-to-back span production (~1 µs apart); at 250 µs between requests
the sender's bursts are small and rarely collide with the handler thread.

**The RSS gap is an artifact of this harness's queue setting.** main's sharded
span queue preallocates `shard_count × QueueSize` physical cells; at the
benchmark's `QueueSize: 65536` that is 32 × 65536 × 8 B ≈ 17 MiB, which is the
entire 32.9 − 15.6 MiB difference. At the shipped default (1024) the same
structure costs ~256 KiB.

### 1-in-100 sampling (production-like)

The same pass rerun with `Sampling.CounterRate: 100` (~1% of requests recorded,
the other 99% taking the continue-unsampled path). Delivery scaled as expected:
~2,670 spans per ~151k-request run for both variants, matching 1% of requests
times the same ×1.78 chunk/async inflation seen at full sampling; both
`-noagent` variants delivered 0.

Agent overhead (enabled − disabled), 4000 RPS:

| version | Δp50 ms | Δp99 ms | ΔCPU |
|---|---:|---:|---:|
| v1.1.0 | +0.000 | -0.010 | +0.7 pt |
| main | +0.000 | +0.020 | +0.4 pt |

At 1000 RPS both versions' ΔCPU rounds to −0.1 pt — below the measurement
floor. **At 1% sampling the agent is effectively free on either version**, and
the 3× CPU gap measured at full sampling collapses, because it lives almost
entirely in the record-serialize-send pipeline that unsampled requests never
enter (the unsampled path itself measured near-identical in the
microbenchmark: `s2_unsampled` ±3%). The versions differ materially only in
what recording a span costs — so the higher the sampling rate (or traffic into
a fixed 1/N), the more main's advantage matters.

Caveats:

- The 1000-RPS p99/max columns are polluted by sporadic multi-second stalls
  with `Connection reset by peer` in the generator log. They hit enabled and
  disabled variants alike (e.g. max 4.0 s on `main` enabled, 4.0 s on `v1.1.0
  noagent`; max 2.1 s on `main-noagent` in the 1%-sampling session), so they
  are loopback/TCP environment noise, not agent behavior. Per-version Δp99 at
  1000 RPS should not be read.
- gRPC downstream and outbound-HTTP endpoints of the e2e suite are not
  exercised; they need downstream infrastructure and version-specific APIs.

## Re-measurement against `6da2fdd` (2026-08-06)

main moved from `acf8cff` to `6da2fdd` (active-span registration reworked into
an intrusive per-shard list, metadata uploads pipelined as async unary calls,
and this tooling's validity gates tightened to per-run checks with
cross-variant delivery equality). Both phases were rerun: 7 microbenchmark
repetitions and the full load pass, all gates green.

What moved:

- **Allocations per op dropped by exactly one in every scenario** (17→16 on
  the request span, 2→1 on the unsampled path, 244→129 on the 100-event span):
  the intrusive active-span node removed the per-request hash-map insert that
  profiling had flagged.
- **`s4c_sql_miss` narrowed from +89% to +50%** and `s5a_deep_events` measured
  +1.7% (was +31%) — within its ±35% spread, so the deep-nesting regression is
  no longer distinguishable from noise in this run.
- Everything else replicated: thread scaling -39/-50/-46/-56% (1/2/4/8
  threads), SQL hit -64/-66%, propagation -44%, peak RSS 70.6 vs 163.9 MiB.
- Phase 2 replicated at 4000 RPS: agent CPU overhead +6.7 pt (main) vs
  +21.8 pt (v1.1.0), agent Δp99 +0.01 ms vs +0.05 ms. The 1000-RPS deltas
  (+4.8 vs +12.3 pt) hold at the same ~3× ratio.
- The single-thread p99 columns remain unstable between runs (`s7` Δp99 was
  +1% in the 08-05 sessions, +140% here; `s1` +76% → +47%), consistent with
  the ambient-allocator diagnosis above. Read the threaded p99 rows, the
  load-test Δp99, and the diagnosis — not any single session's single-thread
  p99 column.

## Raw data

- [raw-2026-08-05.tsv](raw-2026-08-05.tsv) — microbenchmark vs `acf8cff`, run 1 (7 repetitions)
- [raw-2026-08-05-run2.tsv](raw-2026-08-05-run2.tsv) — microbenchmark vs `acf8cff`, run 2 (7 repetitions)
- [raw-2026-08-06.tsv](raw-2026-08-06.tsv) — microbenchmark vs `6da2fdd` (7 repetitions)
- phase-2 logs are regenerable with `run_load_compare.sh` (see README)
