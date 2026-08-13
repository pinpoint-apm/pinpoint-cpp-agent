# Measured Complexity Decisions

Six mechanisms an over-engineering audit flagged as *possibly* removable, each
adjudicated — five by benchmark A/B, one by correctness review. **All six stay.**
The numbers are recorded here so a future change to the workload can revisit them
against a baseline.

Environment: Apple silicon (arm64, Darwin 24), `-O3`, median of repeated runs.
Absolute numbers are machine-specific; the *ratios* are the finding. Reproduction
recipes are at the end.

| Mechanism | Simplification considered | Verdict | Decisive number |
|---|---|---|---|
| `AtomicSharedPtr` `SnapshotCache::ThreadCached` | Delete; keep only `Uncached` | **Keep** | 62x slower loads at 14 threads |
| `ShardedBoundedQueue` quota borrowing | Fixed per-shard quota | **Keep** | 97% retention loss with 1 producer |
| `cache.h` hash-carrying key twins | Hash again per lookup | **Keep** | +58% on the raw-SQL hit (190 B), worse as SQL grows |
| LRU aged promotion | Promote every full-cache hit | **Keep** | 17.7x slower hits at 16 threads |
| `HttpUrlFilter` Exact/Prefix/SegmentPrefix fast paths | Always run the Ant DP | **Keep** | 3.6x slower per request |
| Deferred-destroy shutdown half | Rely on the keep-alive path alone | **Keep** | Cannot work: `weak_from_this()` is expired in the deleter |

## 1. `AtomicSharedPtr` ThreadCached mode (`src/atomic_shared_ptr.h`)

Every span admission loads the `runtime_` snapshot. `Uncached` (a plain atomic
`shared_ptr` load) bumps one shared control-block refcount per load, so it
degrades as threads are added; `ThreadCached` stays flat.

| threads | Uncached load ns/op | ThreadCached load ns/op |
|---|---|---|
| 1 | 12.7 | 4.5 |
| 4 | 200.5 | 4.2 |
| 8 | 340.6 | 4.2 |
| 14 | 428.1 | 6.9 |

`atomic_shared_ptr_benchmark` carries an `uncached load` column permanently, so
this comparison re-runs with the benchmark.

## 2. `ShardedBoundedQueue` quota borrowing (`src/sharded_bounded_queue.h`)

Not a performance feature — raw throughput is mixed, and a fixed per-shard quota
is even *faster* in some drain scenarios (32 producers: 606 vs 364 Mops/s)
because the borrow branch disappears. The point is the retention contract:
`QueueSize` is the *global* logical bound while producers stick to their thread's
home shard. With a paused consumer (collector stall):

| producers | retained (borrowing) | retained (fixed quota) |
|---|---|---|
| 1 | 1,024 (= QueueSize) | 32 (= QueueSize / shards) |
| 4 | 1,024 | 128 |

A server with fewer busy threads than shards — the common case — would lose up
to 97% of its buffered spans during any collector stall.

## 3. Hash-carrying cache key twins (`src/cache.h`)

The `*CacheKey`/`StoredKey`/`KeyTraits` structs carry the key's hash so the
shard's map never re-hashes it. Hashing again inside the map costs one extra
`std::hash` over the raw key per lookup, against a raw-SQL-cache hit of **25 ns**
(single-threaded):

| key size | extra hash cost | overhead on a 25 ns hit |
|---|---|---|
| 190 B (benchmark query) | 14.6 ns | +58% |
| 1 KB | 41.3 ns | +164% |
| 8 KB | 301 ns | 12x |
| 64 KB (max SQL length) | 2.4 µs | 96x |

The raw SQL cache exists to make hits ~76x cheaper than re-normalizing; giving a
growing fraction of that back per lookup defeats it exactly where SQL is large.

## 4. LRU aged promotion (`src/cache.h`)

Only relevant when a cache is full (e.g. more than 1,024 distinct
non-parameterized SQL statements — the classic raw-SQL blowup). Aged promotion
keeps a stable hot set on the pure shared-lock path; promoting under the
exclusive lock on every full-cache hit serializes all hits on the splice.
Hot-set hits, cache full, production shard count:

| threads | aged (current) ns/op | promote-every-hit ns/op |
|---|---|---|
| 1 | 31.4 | 35.7 |
| 4 | 32.2 | 158.7 |
| 8 | 67.1 | 644.4 |
| 16 | 75.2 | 1,332.7 |

## 5. `HttpUrlFilter` literal fast paths (`src/http.cpp`)

The Ant token DP matches everything the Exact/Prefix/SegmentPrefix
specializations match (A/B produced identical match results), so they are pure
speed — but the filter runs once per request. With a representative 5-pattern
exclude list over a 6-URL mix: **722 ns/call with fast paths, 2,585 ns/call
all-Ant (3.6x, +1.9 µs per request)**, scaling with pattern-list length.

## 6. Deferred-destroy shutdown half (`src/agent.cpp`, `SharedDeleter`)

The `weak_from_this().lock()` keep-alive cannot cover the bounded-shutdown
contract alone: by the time `SharedDeleter` runs the strong count is zero and
`weak_from_this().lock()` is guaranteed to return nullptr. The
final-release-without-`Shutdown()` case — promised bounded in `tracer.h` — is
covered *only* by the `delete_when_done` ownership transfer. Without it,
`agent.reset()` (or `pt_agent_destroy` without a prior shutdown) blocks
unboundedly on a wedged worker and
`AgentShutdownDeadlineTest.ReleaseWithoutShutdownDefersDestructionWhenWedged`
fails immediately.

A correctness review of the handoff races, the no-member-access-after-handover
discipline, double-shutdown, the three non-`createShared` fallbacks and fork
inheritance found no defect. A leak-forever alternative (~20 more lines saved)
was rejected: stragglers that finish late would permanently leak caches that can
reach tens of MB.

## Reproduction

Build with `-DBUILD_BENCHMARKS=ON`; binaries land in `<build>/benchmark/`.

- **1**: `atomic_shared_ptr_benchmark` (the `uncached load` column).
- **2**: `span_queue_benchmark --mode all --producers 1,4,32`; for the fixed-
  quota variant change `borrowable_shards_(initial_bitmap(shard_count_))` to
  `borrowable_shards_(0)` in `sharded_bounded_queue.h` and compare the
  `drop-old` column (paused rows) and Mops/s.
- **3**: `raw_sql_cache_benchmark` for the hit cost; time
  `std::hash<std::string>` at the key sizes above for the extra-hash cost.
- **4**: full-cache hot-set micro-bench over `ApiIdCache` (warm past capacity,
  then hit 64 hot keys from N threads); for the variant change
  `if (age < promote_age_threshold_)` to `if (false)` in `cache.h`.
- **5**: micro-bench `HttpUrlFilter::isFiltered` over the pattern/URL mix
  above; for the variant disable the three early-return classifications in
  `HttpUrlFilter::compilePattern` so everything compiles to `PatternKind::Ant`.
