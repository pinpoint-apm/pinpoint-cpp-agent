# Pinpoint C++ Agent - Sampling Policy

This document explains the sampling policy implementation in the Pinpoint C++ Agent, based on the analysis of `sampling.cpp`, `sampling.h`, and `agent.cpp`.

## Table of Contents
- [Overview](#overview)
  - [Key Concepts](#key-concepts)
  - [Why Transaction Sampling?](#why-transaction-sampling)
- [Sampling Decision Flow](#sampling-decision-flow)
- [Sampler Types](#sampler-types)
- [Trace Samplers](#trace-samplers)
- [Configuration](#configuration)
- [Implementation Details](#implementation-details)
- [Examples](#examples)

## Overview

The Pinpoint C++ Agent uses a flexible sampling strategy to control the amount of trace data collected. Sampling is essential for:

- **Performance**: Reducing overhead in high-traffic applications
- **Cost**: Managing collector resource usage
- **Relevance**: Focusing on representative transactions

### Key Concepts

#### Transaction in Distributed Tracing

In distributed tracing, a **transaction** refers to the entire flow of a user request across multiple microservices and systems, not a traditional database transaction (ACID properties). It represents a logical unit of work that may span:

- **Multiple Services**: An order request may flow through order service → inventory service → payment service
- **Span**: A unique ID representing the entire distributed transaction, propagated across all service calls
- **SpanEvent**: Individual operations within a trace (e.g., function execution, DB query, API call), with parent-child relationships

#### Sampling Concepts

1. **New Transaction**: A transaction without a parent trace context (root span)
2. **Continue Transaction**: A transaction that continues from a parent span (distributed tracing)
3. **Sampled**: The transaction will be traced and sent to the collector
4. **Unsampled**: The transaction will not be fully traced (minimal overhead)

### Why Transaction Sampling?

**Transaction Sampling** is a technique that selectively collects trace data from only a subset of transactions based on specific criteria, reducing the overhead and storage burden of collecting data from all requests in large-scale distributed systems.

#### Purpose and Necessity

In microservice architectures (MSA), a single request traverses multiple services, generating numerous spans. Tracing all requests can:

- **Impact Performance**: Create system overhead and latency
- **Increase Costs**: Generate massive amounts of data to store and analyze
- **Overwhelm Resources**: Burden collector and storage systems

Sampling aims to minimize overhead while collecting sufficient data to:
- Understand overall system health
- Identify bottlenecks and performance issues
- Detect anomalies and errors

#### Sampling Strategies

Distributed tracing systems support various sampling strategies:

**1. Constant Rate Sampling (Head-based Sampling)**
- **Decision Point**: Sampling decision is made when the first span (root span) of a trace is created
- **Propagation**: This decision is propagated throughout the entire trace
- **Consistency**: The entire trace is either kept or discarded - no partial trace collection
- Decides at the entry point whether to collect a trace based on a fixed rate (e.g., 10%)
- Once sampled, all spans in that transaction are collected
- Simple to implement but may miss important or error transactions
- **Pinpoint C++ Agent**: Uses this approach with CounterSampler and PercentSampler

**2. Probabilistic Sampling**
- Applies sampling probability randomly to each transaction
- Similar to constant rate but ensures more uniform distribution in high-traffic scenarios
- **Pinpoint C++ Agent**: PercentSampler implements this strategy

**3. Adaptive Sampling (Throughput-based)**
- Dynamically adjusts sampling rate based on current system load
- Lowers rate during high traffic, raises during low traffic
- Maintains consistent data collection volume
- **Pinpoint C++ Agent**: ThroughputLimitTraceSampler implements this with NewThroughput/ContinueThroughput settings

**4. Error-based Sampling (Tail-based Sampling)**
- Selectively collects data after transaction completes, only for errors or high-latency cases
- Most useful for troubleshooting but requires buffering all trace data initially
- Higher system overhead but captures the most relevant data
- **Pinpoint C++ Agent**: Can be partially achieved by setting sampling rate high and using error status codes

#### Sampling Considerations

**Balance is Key**:
- **Too Low**: Risk missing critical issues and anomalies
- **Too High**: Incur performance overhead and storage costs

**Choose Based On**:
- Monitoring objectives (debugging vs. production monitoring)
- System scale and traffic volume
- Available resources (collector, storage)
- Service criticality

## Sampling Decision Flow

### Head-based Sampling Principle

Pinpoint C++ Agent implements **head-based sampling**, where:

- **Decision Timing**: The sampling decision is made when the **root span** (first span of a trace) is created
- **Decision Propagation**: This decision is propagated to all downstream services via trace context headers
- **Trace Integrity**: The entire trace is either collected or discarded - there are no partial traces
- **Consistency**: All spans belonging to the same trace follow the same sampling decision

This ensures that distributed traces remain complete and useful for end-to-end analysis.

### NewSpan() Decision Logic

When `NewSpan()` is called, the sampling decision follows this flow:

```cpp
// From agent.cpp:207-241
SpanPtr AgentImpl::NewSpan(std::string_view operation, std::string_view rpc_point,
                           std::string_view method, TraceContextReader& reader) {
    // 1. Check if agent is enabled
    if (!enabled_) {
        return noopSpan();
    }
    
    // 2. Check URL exclusion filter
    if (http_url_filter_ && http_url_filter_->isFiltered(rpc_point)) {
        return noopSpan();
    }
    
    // 3. Check HTTP method exclusion filter
    if (!method.empty() && http_method_filter_ && http_method_filter_->isFiltered(method)) {
        return noopSpan();
    }
    
    // 4. Check parent sampling decision
    if (const auto parent_sampling = reader.Get(HEADER_SAMPLED); parent_sampling == "s0") {
        return std::make_shared<UnsampledSpan>(this);
    }
    
    // 5. Make sampling decision
    bool my_sampling = false;
    if (const auto tid = reader.Get(HEADER_TRACE_ID); tid.has_value()) {
        // Continue transaction - follows parent
        my_sampling = sampler_->isContinueSampled();
    } else {
        // New transaction - applies sampling rate
        my_sampling = sampler_->isNewSampled();
    }
    
    // 6. Create appropriate span
    SpanPtr span;
    if (my_sampling) {
        span = std::make_shared<SpanImpl>(this, operation, rpc_point);
    } else {
        span = std::make_shared<UnsampledSpan>(this);
    }
    span->ExtractContext(reader);
    return span;
}
```

### Decision Steps

1. **Agent Enabled Check**: If the agent is disabled, return a no-op span
2. **URL Filtering**: Check if the URL matches exclusion patterns
3. **Method Filtering**: Check if the HTTP method should be excluded
4. **Parent Sampling**: If parent explicitly said "don't sample" (`s0`), don't sample
5. **Trace Context Check**: 
   - If `HEADER_TRACE_ID` exists → **Continue Transaction**
   - If `HEADER_TRACE_ID` does not exist → **New Transaction**
6. **Sampling Decision**:
   - New: Apply `isNewSampled()` logic
   - Continue: Apply `isContinueSampled()` logic
7. **Span Creation**: Create either `SpanImpl` (sampled) or `UnsampledSpan` (unsampled)

## Sampler Types

The agent supports two base sampler types:

### 1. CounterSampler

Samples 1 out of every N transactions using a counter.

**Implementation** (from `sampling.cpp:22-30`):
```cpp
bool CounterSampler::isSampled() noexcept {
    if (rate_ == 0) {
        return false;  // Never sample
    }
    
    const auto count = sampling_count_.fetch_add(1) + 1;
    const uint64_t r = count % rate_;
    return r == 0;  // Sample when count is multiple of rate_
}
```

**Behavior**:
- Uses atomic counter for thread-safety
- Samples when `count % rate == 0`
- If `rate = 1`: samples **every** transaction
- If `rate = 10`: samples **1 in 10** transactions (10%)
- If `rate = 0`: samples **no** transactions

**Configuration**:
```yaml
Sampling:
  Type: "COUNTING"
  CounterRate: 10  # Sample 1 out of every 10 transactions
```

**Example**:
```
CounterRate: 3
Transaction:  1  2  3  4  5  6  7  8  9  10 11 12
Count:        1  2  3  4  5  6  7  8  9  10 11 12
Sampled:     No No YES No No YES No No YES No No YES
```

### 2. PercentSampler

Samples a percentage of transactions based on accumulated rate.

**Implementation** (from `sampling.cpp:32-40`):
```cpp
bool PercentSampler::isSampled() noexcept {
    if (rate_ == 0) {
        return false;  // Never sample
    }
    
    const auto count = sampling_count_.fetch_add(rate_) + rate_;
    const uint64_t r = count % MAX_PERCENT_RATE;  // MAX_PERCENT_RATE = 10000
    return static_cast<int>(r) < rate_;
}
```

**Behavior**:
- Uses atomic counter with rate accumulation
- Internal rate = `configured_rate * 100` (for precision)
- `MAX_PERCENT_RATE = 10000` (100.00%)
- More evenly distributed than counter sampling

**Configuration**:
```yaml
Sampling:
  Type: "PERCENT"
  PercentRate: 10.0  # Sample 10% of transactions
```

**Example**:
```
PercentRate: 10.0 (internal rate = 1000)
Each call adds 1000 to counter
Transaction: 1     2     3     4     5     6     7     8     9     10
Count:       1000  2000  3000  4000  5000  6000  7000  8000  9000  10000
Modulo:      1000  2000  3000  4000  5000  6000  7000  8000  9000  0
Sampled:     YES   NO    NO    NO    NO    NO    NO    NO    NO    YES
```

## Trace Samplers

Trace samplers wrap base samplers and handle new vs. continue transactions.

### 1. BasicTraceSampler

Simple trace sampler that applies the base sampler to new transactions.

**Implementation** (from `sampling.cpp:42-55`):
```cpp
bool BasicTraceSampler::isNewSampled() noexcept {
    const auto sampled = sampler_ ? sampler_->isSampled() : false;
    if (sampled) {
        incr_sample_new();    // Statistics: sampled new transaction
    } else {
        incr_unsample_new();  // Statistics: unsampled new transaction
    }
    return sampled;
}

bool BasicTraceSampler::isContinueSampled() noexcept {
    incr_sample_cont();  // Statistics: sampled continue transaction
    return true;  // Always sample continue transactions
}
```

**Behavior**:
- **New transactions**: Apply base sampler logic
- **Continue transactions**: **Always sample** (follow parent trace)

**Configuration**:
```yaml
Sampling:
  Type: "COUNTING"      # or "PERCENT"
  CounterRate: 10       # Only for COUNTING
  PercentRate: 10.0     # Only for PERCENT
```

### 2. ThroughputLimitTraceSampler

Advanced trace sampler with throughput limits using rate limiters.

**Implementation** (from `sampling.cpp:57-91`):
```cpp
bool ThroughputLimitTraceSampler::isNewSampled() noexcept {
    auto sampled = sampler_ ? sampler_->isSampled() : false;
    if (sampled) {
        if (new_limiter_) {
            sampled = new_limiter_->allow();  // Apply rate limiter
            if (sampled) {
                incr_sample_new();
            } else {
                incr_skip_new();  // Statistics: skipped by rate limiter
            }
        } else {
            incr_sample_new();
        }
    } else {
        incr_unsample_new();
    }
    return sampled;
}

bool ThroughputLimitTraceSampler::isContinueSampled() noexcept {
    auto sampled = true;
    if (cont_limiter_) {
        sampled = cont_limiter_->allow();  // Apply rate limiter
        if (sampled) {
            incr_sample_cont();
        } else {
            incr_skip_cont();  // Statistics: skipped by rate limiter
        }
    } else {
        incr_sample_cont();
    }
    return sampled;
}
```

**Behavior**:
- **New transactions**:
  1. First apply base sampler
  2. Then apply new throughput limiter (if configured)
- **Continue transactions**:
  1. Apply continue throughput limiter (if configured)
  2. Default is to always sample if no limiter

**Configuration**:
```yaml
Sampling:
  Type: "PERCENT"
  PercentRate: 100.0           # Base sampling: 100%
  NewThroughput: 100           # Limit new transactions to 100/sec
  ContinueThroughput: 200      # Limit continue transactions to 200/sec
```

**Constructor** (from `sampling.h:88-96`):
```cpp
ThroughputLimitTraceSampler(std::unique_ptr<Sampler> sampler, 
                           const int new_tps, 
                           const int continue_tps) {
    sampler_ = std::move(sampler);
    if (new_tps > 0) {
        new_limiter_ = std::make_unique<RateLimiter>(new_tps);
    }
    if (continue_tps > 0) {
        cont_limiter_ = std::make_unique<RateLimiter>(continue_tps);
    }
}
```

## Configuration

### Sampler Selection

The sampler is selected during agent initialization (from `agent.cpp:38-56`):

```cpp
AgentImpl::AgentImpl(const Config& options) : config_(options) {
    // Create base sampler
    std::unique_ptr<Sampler> sampler;
    if (compare_string(config_.sampling.type, PERCENT_SAMPLING)) {
        sampler = std::make_unique<PercentSampler>(config_.sampling.percent_rate);
    } else {
        sampler = std::make_unique<CounterSampler>(config_.sampling.counter_rate);
    }
    
    // Wrap with trace sampler
    if (config_.sampling.new_throughput > 0 || config_.sampling.cont_throughput > 0) {
        sampler_ = std::make_unique<ThroughputLimitTraceSampler>(
            std::move(sampler),
            config_.sampling.new_throughput,
            config_.sampling.cont_throughput
        );
    } else {
        sampler_ = std::make_unique<BasicTraceSampler>(std::move(sampler));
    }
}
```

### Configuration Options

#### Option 1: Counter Sampling

```yaml
Sampling:
  Type: "COUNTING"
  CounterRate: 10  # Sample 1 in 10 transactions
```

#### Option 2: Percent Sampling

```yaml
Sampling:
  Type: "PERCENT"
  PercentRate: 10.0  # Sample 10% of transactions
```

#### Option 3: Throughput-Limited Sampling

```yaml
Sampling:
  Type: "PERCENT"
  PercentRate: 100.0
  NewThroughput: 100      # Max 100 new transactions/sec
  ContinueThroughput: 200 # Max 200 continue transactions/sec
```

## Implementation Details

### Thread Safety

All samplers use atomic operations for thread-safe counter updates:

```cpp
std::atomic<uint64_t> sampling_count_;

// Atomic fetch-and-add
const auto count = sampling_count_.fetch_add(1) + 1;
```

### Statistics Tracking

Samplers track statistics through base class methods:
- `incr_sample_new()` - Sampled new transaction
- `incr_unsample_new()` - Unsampled new transaction
- `incr_sample_cont()` - Sampled continue transaction
- `incr_skip_new()` - Skipped by new rate limiter
- `incr_skip_cont()` - Skipped by continue rate limiter

### Rate Limiter

The `RateLimiter` class (from `limiter.h`) implements token bucket algorithm:
- Allows specified transactions per second
- Smooths burst traffic
- Thread-safe implementation

## Examples

### Example 1: Development Environment

Sample all transactions for debugging:

```yaml
Sampling:
  Type: "COUNTING"
  CounterRate: 1  # Sample every transaction
```

```cpp
// Result: All transactions sampled
// Transaction 1: Sampled ✓
// Transaction 2: Sampled ✓
// Transaction 3: Sampled ✓
```

### Example 2: Production Environment

Sample 10% of transactions:

```yaml
Sampling:
  Type: "PERCENT"
  PercentRate: 10.0  # 10% sampling
```

```cpp
// Result: Approximately 10% of transactions sampled
// Out of 100 transactions, ~10 will be sampled
```

### Example 3: High-Traffic Environment

Limit throughput while maintaining some sampling:

```yaml
Sampling:
  Type: "PERCENT"
  PercentRate: 50.0        # First pass: 50% sampling
  NewThroughput: 100       # Then limit to 100/sec
  ContinueThroughput: 200  # Limit continues to 200/sec
```

```cpp
// Result: 
// - Base sampling selects 50% of transactions
// - Rate limiter caps new transactions at 100/sec
// - Rate limiter caps continue transactions at 200/sec
```

### Example 4: Distributed Tracing Behavior

```yaml
Sampling:
  Type: "COUNTING"
  CounterRate: 10  # Sample 1 in 10 new transactions
```

**Scenario**:
```
Service A (NewSpan - no parent):
  Transaction 1: Not sampled (counter: 1)
  Transaction 2: Not sampled (counter: 2)
  ...
  Transaction 10: Sampled (counter: 10) ✓

Service B receives request from Transaction 10:
  NewSpan(with parent trace context):
  → isContinueSampled() called
  → Always returns true
  → Transaction sampled ✓ (follows parent)
```

### Example 5: Parent Sampling Override

```yaml
# Child service configuration
Sampling:
  Type: "COUNTING"
  CounterRate: 1  # Would sample all
```

**Scenario**:
```
Parent Service: Decides NOT to sample
  → Sets header: "Pinpoint-Sampled: s0"

Child Service receives request:
  NewSpan(reader) called
  → Reads "Pinpoint-Sampled: s0"
  → Returns UnsampledSpan immediately
  → Child's sampling config IGNORED ✓
```

## Sampling Decision Matrix

| Condition | New Transaction | Continue Transaction | Result |
|-----------|----------------|---------------------|---------|
| Agent disabled | - | - | NoopSpan |
| URL excluded | - | - | NoopSpan |
| Method excluded | - | - | NoopSpan |
| Parent says "s0" | - | Yes | UnsampledSpan |
| No trace ID | Yes | - | isNewSampled() |
| Has trace ID | - | Yes | isContinueSampled() |

## Best Practices

### 1. Development/Testing

Use high sampling rates or sample all:
```yaml
Sampling:
  Type: "COUNTING"
  CounterRate: 1
```

### 2. Production (Low Traffic)

Use percentage sampling:
```yaml
Sampling:
  Type: "PERCENT"
  PercentRate: 20.0  # 20% sampling
```

### 3. Production (High Traffic)

Use throughput limiting:
```yaml
Sampling:
  Type: "PERCENT"
  PercentRate: 100.0
  NewThroughput: 100
  ContinueThroughput: 200
```

### 4. Distributed System

- Let root service control sampling
- Continue transactions automatically follow parent
- Don't set parent sampling to "s0" unless intentional

### 5. Troubleshooting

Temporarily increase sampling:
```yaml
Sampling:
  Type: "COUNTING"
  CounterRate: 1  # Sample all temporarily
```

## Related Documentation

- [Configuration Guide](config.md#sampling-configuration) - Sampling configuration options
- [Instrumentation Guide](instrument.md) - How to create spans
- [Troubleshooting Guide](troubleshooting.md) - Sampling-related issues

## References

**Source Files**:
- `src/sampling.h` - Sampler interfaces and declarations
- `src/sampling.cpp` - Sampler implementations
- `src/agent.cpp:207-241` - NewSpan sampling decision logic
- `src/limiter.h` - RateLimiter implementation

## License

Apache License 2.0 - See [LICENSE](../LICENSE) for details.

