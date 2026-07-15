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
