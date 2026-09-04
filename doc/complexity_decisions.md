# Measured Complexity Decisions

Seven mechanisms an over-engineering audit flagged as *possibly* removable, each
adjudicated — six by benchmark A/B, one by correctness review. **All seven stay.**
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
| `OwnedHandleRegistry` 32-way sharding | One `shared_mutex` over one map | **Keep** | 38x slower per C-API span at 8 threads |

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
| 64 KB (raw SQL cache limit) | 2.4 µs | 96x |

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

## 7. `OwnedHandleRegistry` sharding (`src/tracer_c.cpp`)

The C API keeps every live `pt_agent_t`/`pt_span_t` in a registry behind a
`shared_mutex`. Lookups take a shared lock, so a single mutex looks sufficient —
concurrent readers never block each other. But the registry's real traffic is
not read-only: a span handle is **inserted once and erased once** around its
5-15 lookups, and those two take the *exclusive* lock, which blocks every
reader on that mutex. One cycle below is that whole unit (insert + 10 finds +
erase), i.e. one traced request's worth of registry traffic.

| threads | shards=1 ns/cycle | shards=4 | shards=8 | shards=32 (current) |
|---|---|---|---|---|
| 1 | 176 | 175 | 177 | 177 |
| 4 | 1,509 | 718 | 122 | 128 |
| 8 | 9,084 | 4,071 | 2,369 | 237 |
| 14 | 31,407 | 10,123 | 5,165 | 1,185 |

Un-sharded costs **38x at 8 threads** and 27x at 14. The count is not
over-provisioned either: 8 shards is still 10x worse than 32 at 8 threads,
because an exclusive lock on a colliding shard stalls that shard's readers too,
and collisions stay common until the shard count is well above the thread count.

The sharding is not free, and the benchmark keeps the cost visible in its
second table. With a **pure-lookup** workload — the population pre-inserted, no
insert/erase traffic at all — sharding is *slower*, 111 vs 45 ns/find at 14
threads (2.5x), because a thread's handles scatter across many shard cache
lines instead of reusing one resident map. That workload does not exist in
production (a handle that is never inserted or erased cannot be looked up), but
if the C API ever grows long-lived handles that are looked up far more than they
are created, this is the number that would change the decision.

One caveat the numbers do not show: the **agent** registry normally holds a
single handle, so every lookup lands on one shard whatever the shard count.
Sharding neither helps nor hurts there; the span registry is what it buys.

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
- **7**: `owned_handle_registry_benchmark`. It carries the shard-count sweep
  permanently (the registry is copied into it verbatim, with `kShardCount`
  lifted to a template parameter, so `shards=1` is the un-sharded candidate
  running otherwise identical code), so this comparison re-runs with the
  benchmark.
