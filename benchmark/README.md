# Microbenchmarks

## Span queue

This benchmark compares the former single-`std::mutex` span queue with the
preallocated sharded bounded queue used by `GrpcSpan`. It measures producer
throughput and sampled enqueue p50/p99/max latency without allocating a payload per operation.
The move-only benchmark token has the same ownership-transfer shape and size as
a `std::unique_ptr`.

## Memory trade-off

`QueueSize` is the global logical retention bound, but it is not the number of
physically allocated cells. To let any one producer shard borrow the entire
logical capacity without allocating on the enqueue path, every shard
preallocates a `QueueSize`-cell ring:

```text
physical cell count = shard_count * QueueSize
approximate cell storage = shard_count * QueueSize * sizeof(queue value)
```

With the defaults (`QueueSize=1024`, 32 shards, and an 8-byte pointer-sized
value), this is 32,768 physical cells, or approximately 256 KiB, excluding shard
metadata and allocator overhead. At most `QueueSize` values are logically
retained across all shards.

The benchmark target is always compiled with optimization, including when the
rest of the project uses the `debug` preset:

```sh
cmake --preset debug -DBUILD_BENCHMARKS=ON
cmake --build --preset debug --target span_queue_benchmark
./build/debug/benchmark/span_queue_benchmark --verify
```

Useful options:

```text
--producers 1,8,32,64
--operations 100000        # per producer and repetition
--repeats 3
--queue-size 1024
--sample-every 64
--mode drain|slow|paused|all
--verify                   # return non-zero if completion gates fail
```

`drain` continuously consumes, `slow` adds CPU work to the consumer, and
`paused` leaves the queue saturated so producers exercise the overflow path.
The completion gates use the `drain` results:

- 32-producer throughput is at least 2x the mutex baseline.
- 32-producer enqueue p99 is at least 70% lower than the mutex baseline.
- 1-producer throughput regression is no more than 5%.

Run on an otherwise idle machine and compare medians from multiple repetitions.
Latency includes the two clock reads on sampled operations equally for both
implementations.

## Raw SQL prepared cache

`raw_sql_cache_benchmark` compares the former per-call SQL normalization plus
canonical ID lookup with a warmed `RawSqlCache` hit. It also intercepts
`operator new`/`operator new[]` during the measured loop and fails if a cache
hit allocates or invokes the preparation factory. An eight-thread pass checks
the shared-lock hit path under contention.

```sh
cmake --preset debug -DBUILD_BENCHMARKS=ON
cmake --build --preset debug --target raw_sql_cache_benchmark
./build/debug/benchmark/raw_sql_cache_benchmark 500000
```

The optional argument is the total operation count used by both the
single-thread and parallel comparisons. Run several times on an otherwise idle
machine and compare the median `ns/op` and speedup values.

The `speedup` figures describe the **cache-hit** path only, i.e. workloads that
repeat the same raw SQL text (prepared statements or drivers that reuse query
strings with bind parameters). The final `miss (distinct raw)` lines report the
opposite extreme: a pool of distinct statements larger than the cache, so every
lookup misses and pays the hash, insert, and eviction churn on top of
normalization. Its `overhead` value is `raw-cache ns/op ÷ legacy ns/op`; a value
above `1.0` means that for workloads which inline literals instead of binding
parameters (hit rate ≈ 0), the front cache costs more than it saves. Weigh both
numbers against the expected raw-SQL repetition of the target application.

## Metadata id cache hit path

`id_cache_benchmark` measures the warm `ApiIdCache` hit under thread
contention. The api id cache is consulted once per sampled span plus once per
named span event; before sharding, every hit performed two atomic RMWs
(shared-lock acquire/release) on one `shared_mutex` cache line, so the line
ping-ponged between cores as request threads were added. The benchmark
compares a single-shard cache (the former layout) with the default
`kDefaultCacheShardCount` sharding at 1, 4, and 8 threads, and fails if any
measured lookup misses or the two configurations disagree on ids.

```sh
cmake --preset debug -DBUILD_BENCHMARKS=ON
cmake --build --preset debug --target id_cache_benchmark
./build/debug/benchmark/id_cache_benchmark 2000000
```

The optional argument is the total operation count per configuration. The
interesting figure is how `ns/op` grows from 1 thread to N threads within each
configuration: single-shard degrades by multiples while sharded should stay
close to its single-thread cost. Run several times on an otherwise idle
machine and compare medians.

## Atomic shared pointer snapshots

`atomic_shared_ptr_benchmark` compares the former C++17 `shared_mutex` plus
owning-copy load with the generation-validated thread-local cache. It reports
both the compatible by-value `load()` and the opt-in reference accessor. The
thread-cached owning load uses a per-thread localized control block: it keeps
the lifetime and re-entrancy safety of a `shared_ptr` copy without updating one
source refcount from every reader core. The legacy implementation is embedded
in the benchmark, so one run produces the before/after comparison under the
same machine load.

```sh
cmake --preset debug -DBUILD_BENCHMARKS=ON
cmake --build --preset debug --target atomic_shared_ptr_benchmark
./build/debug/benchmark/atomic_shared_ptr_benchmark 1000000
```

The optional argument is the operation count per thread. Each row is the median
of three runs. `ns/op` is elapsed wall time divided by operations per thread,
so an unchanged value as thread count rises indicates that the reader path is
not accumulating cross-core contention.

## Active span registry

Compares the former active-span registry (64-shard `std::unordered_map` keyed
by span id — one map-node malloc/free per request) with the intrusive-list
`ActiveSpanRegistry` that replaced it. One measured pair is
`addActiveSpan` + `dropActiveSpan`, the two registry touches every request
makes. The legacy implementation is embedded in the benchmark verbatim; the
new one is included from `src/active_span.h`, so the benchmark measures the
exact production code. Both registries run against a steady resident
population of in-flight spans and identical per-thread span-id streams, with
sampled per-pair latency (p50/p99/max) alongside aggregate throughput. A
final phase times the `collect()` snapshot scan with 10,000 in-flight spans —
that scan runs once per stat-collect interval, so it only needs to show no
regression.

## C handle registry sharding

Sweeps the shard count of `tracer_c.cpp`'s `OwnedHandleRegistry` — the map that
turns a `pt_span_t`/`pt_agent_t` token back into its wrapper — from 1 to 32.
The registry lives in an anonymous namespace and cannot be included, so it is
copied in verbatim with `kShardCount` lifted to a template parameter; `shards=1`
is therefore the un-sharded candidate running otherwise identical code, not a
strawman.

The first table measures the production unit: insert + 10 finds + erase, one
traced request's worth of traffic. The second holds the population fixed and
only looks up, isolating reader contention from the exclusive-lock insert/erase
traffic — and showing the cost the sharding does carry, since scattering a
thread's handles across shards loses map locality. See
[Measured Complexity Decisions](../doc/complexity_decisions.md) §7.

```sh
cmake --preset default -DBUILD_BENCHMARKS=ON
cmake --build --preset default --target owned_handle_registry_benchmark
./build/default/benchmark/owned_handle_registry_benchmark 400000
```

## Span lifecycle

The benchmarks above each isolate one mechanism and answer "was this data
structure worth it". This one answers the question a host application asks
instead: **what does one instrumented request cost the thread that makes it?**
It drives a real `AgentImpl` through the whole
`NewSpan` → `NewSpanEvent` → `EndSpan` lifecycle rather than any single
component of it.

That gap was not theoretical. `AgentImpl::tracing_active()` carried a
`getpid()` per span — ~1 ns on Darwin but ~150 ns on Linux/glibc, more than
every component measured above put together — and no component benchmark
could have shown it.

**Unlike the other targets here, this one must be built against an optimized
library.** The others are header-only and get compiled into the benchmark
translation unit, which this directory always builds with optimization; this
one measures code that lives in `libpinpoint_cpp`, so a `debug`
(Debug) library reports numbers several times too slow:

```sh
cmake --preset default -DBUILD_BENCHMARKS=ON
cmake --build --preset default --target span_lifecycle_benchmark
./build/default/benchmark/span_lifecycle_benchmark 50000
```

The optional argument is the request count **per thread**, so the total work
grows with the thread count and `ns/req` stays comparable across rows.
`ns/req` is wall time divided by requests per thread — the same per-thread
convention as the atomic-shared-pointer benchmark, so a column that stays
flat as threads are added means the agent accumulates no cross-core
contention, and one that grows is a cost to explain.

Four request shapes, because they cost very different amounts and real
traffic is a mix of them:

| shape | what it exercises |
|---|---|
| `filtered` | the URL is excluded, so `NewSpan` returns the shared noop span |
| `unsampled` | admitted but not sampled: an `UnsampledSpan`, still registered in the active-request registry and still timed at `EndSpan`. The majority path whenever sampling is on, so this is what most production requests actually pay |
| `sampled` | a full `SpanImpl` for a fresh root trace, with N span events |
| `continued` | a full `SpanImpl` for an inbound trace: the trace id is parsed and every propagation header is copied into `SpanData` |

A continued trace bypasses the sampler entirely (see
`TraceSampler::isContinueSampled`), so that shape is always sampled
regardless of the configured rate; the `unsampled` rows therefore run against
a second agent configured at `percent_rate = 0`.

Each shape is verified before it is timed — the noop span reports span id 0,
an `UnsampledSpan` reports a real span id but `IsSampled() == false`, and a
`SpanImpl` reports both — so a phase cannot silently degrade to noop spans
(an agent that never came online, a filter that stopped matching) and still
publish a fast number. A failed verification exits non-zero.

The gRPC clients are the production classes with their channels held unready,
so the span worker collects batches and discards them exactly as it does
during a collector outage: the queue is drained and chunks are destroyed, as
in production, without a network or a mock framework. Watch the run for
`span queue overflow` warnings — a machine loaded enough to push the worker
behind moves enqueues onto the head-drop path and changes what is measured.

Two caveats when reading the output:

- For the very cheap shapes the sampled `p50`/`p99` are dominated by the two
  `Clock::now()` calls around each timed request (tens of ns). Read `ns/req`
  for those rows; the percentiles are meaningful once a request costs
  hundreds of ns or more.
- On a heterogeneous CPU (Apple silicon: 10 performance + 4 efficiency
  cores), the 8-thread rows spill onto efficiency cores, so part of the 1→8
  rise is core heterogeneity rather than contention. The 1→4 comparison is
  the clean one there.
