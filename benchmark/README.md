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
rest of the project uses the `debug-cached` preset:

```sh
cmake --preset debug-cached -DBUILD_BENCHMARKS=ON
cmake --build --preset debug-cached --target span_queue_benchmark
./build/debug-cached/benchmark/span_queue_benchmark --verify
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
cmake --preset debug-cached -DBUILD_BENCHMARKS=ON
cmake --build --preset debug-cached --target raw_sql_cache_benchmark
./build/debug-cached/benchmark/raw_sql_cache_benchmark 500000
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
cmake --preset debug-cached -DBUILD_BENCHMARKS=ON
cmake --build --preset debug-cached --target id_cache_benchmark
./build/debug-cached/benchmark/id_cache_benchmark 2000000
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
cmake --preset debug-cached -DBUILD_BENCHMARKS=ON
cmake --build --preset debug-cached --target atomic_shared_ptr_benchmark
./build/debug-cached/benchmark/atomic_shared_ptr_benchmark 1000000
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
