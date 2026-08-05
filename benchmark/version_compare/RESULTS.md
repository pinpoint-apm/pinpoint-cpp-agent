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
signature of removed shared hot spots rather than a constant-factor win. It lines
up with `d5c341e` (shard the metadata id caches), `de40e4a` (batch the span queue
drain), `4541cb8` (cache AtomicSharedPtr snapshots per thread), and `e06ede8`
(close per-request allocation and false-sharing gaps). The 8-thread rows have the
tightest spreads in the whole table (±2%/±3%) and p99 improves alongside the
median at every thread count, so this is the most reliable result here.

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
  `Stat.Enable: false` — a candidate for gating.
- Thread counts stop at 8 because the agent's own gRPC workers share the 10
  performance cores; beyond that the numbers measure oversubscription.
- Measured on a laptop. The thread-scaling shape and the allocation counts should
  transfer; absolute nanoseconds should not.

## Raw data

- [raw-2026-08-05.tsv](raw-2026-08-05.tsv) — run 1 (7 repetitions)
- [raw-2026-08-05-run2.tsv](raw-2026-08-05-run2.tsv) — run 2 (7 repetitions)
