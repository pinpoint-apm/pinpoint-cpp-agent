#!/usr/bin/env python3
# Copyright 2020-present NAVER Corp.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Analyzes an api_benchmark --trace dump for the *time structure* of latency
spikes, which percentile summaries erase.

The discriminating questions:
- Are spikes periodic in operation count? A period equal to the span batch size
  points at per-batch work (unary call setup, serialization hand-off) stalling
  the producer once per batch.
- Are spikes periodic in wall time? A ~1000 ms period points at the flush
  timer; ~100 ms at some other timer wheel.
- Are they aperiodic? Then allocator or scheduler interference is more likely.
"""

import argparse
import statistics
from collections import Counter


def load(path):
    """Returns list of dicts; phase columns are 0 when the scenario has none."""
    samples = []
    with open(path, encoding="utf-8") as handle:
        header = next(handle).split()
        for line in handle:
            fields = [int(f) for f in line.split()]
            row = dict(zip(header, fields))
            samples.append(row)
    return samples


def percentile(sorted_values, fraction):
    index = int(fraction * (len(sorted_values) - 1))
    return sorted_values[index]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("trace")
    parser.add_argument("--spike-factor", type=float, default=4.0,
                        help="a spike is latency > factor * median")
    parser.add_argument("--modulo", type=int, default=20,
                        help="op-index modulo to test for batch alignment "
                             "(default 20 = SpanBatch.Size default)")
    args = parser.parse_args()

    samples = load(args.trace)
    latencies = sorted(row["latency_ns"] for row in samples)
    median = statistics.median(latencies)
    threshold = median * args.spike_factor

    print(f"ops={len(samples)} median={median:.0f}ns "
          f"p90={percentile(latencies, 0.90)}ns "
          f"p99={percentile(latencies, 0.99)}ns "
          f"p99.9={percentile(latencies, 0.999)}ns "
          f"max={latencies[-1]}ns")
    print(f"spike threshold: >{threshold:.0f}ns")

    spikes = [(i, row["offset_ns"], row["latency_ns"]) for i, row in enumerate(samples)
              if row["latency_ns"] > threshold]
    if not spikes:
        print("no spikes above threshold")
        return
    total_time_s = samples[-1]["offset_ns"] / 1e9
    print(f"spikes: {len(spikes)} ({len(spikes) / len(samples) * 100:.2f}% of ops, "
          f"{len(spikes) / total_time_s:.1f}/s over {total_time_s:.2f}s)")

    # Spike mass: how much of the total tail time the spikes account for.
    spike_time = sum(latency for _, _, latency in spikes)
    total_time = sum(latencies)
    print(f"spike time share: {spike_time / total_time * 100:.1f}% of total latency")

    # Periodicity in op count.
    gaps = [spikes[i + 1][0] - spikes[i][0] for i in range(len(spikes) - 1)]
    if gaps:
        gap_counts = Counter(gaps)
        common = gap_counts.most_common(8)
        print(f"op-gap median={statistics.median(gaps):.0f} "
              f"most common: {common}")

    # Alignment with the batch cadence: if per-batch work stalls the producer,
    # spike op-indices cluster at a fixed residue modulo the batch size.
    residues = Counter(op_index % args.modulo for op_index, _, _ in spikes)
    top = residues.most_common(5)
    expected = len(spikes) / args.modulo
    print(f"op-index mod {args.modulo}: top residues {top} "
          f"(uniform would be ~{expected:.1f} per residue)")

    # Periodicity in wall time.
    time_gaps_ms = [(spikes[i + 1][1] - spikes[i][1]) / 1e6
                    for i in range(len(spikes) - 1)]
    if time_gaps_ms:
        time_gaps_ms.sort()
        print(f"time-gap ms: median={statistics.median(time_gaps_ms):.3f} "
              f"p90={percentile(time_gaps_ms, 0.90):.3f} "
              f"max={time_gaps_ms[-1]:.3f}")

    # Phase attribution: for spike ops, where did the *excess* time (beyond the
    # phase's own median) go? Concentration in one phase names the mechanism.
    phase_keys = ("new_span_ns", "events_ns", "end_span_ns")
    if any(samples[0].get(key, 0) for key in phase_keys):
        normal = [row for row in samples if row["latency_ns"] <= threshold]
        print("phase medians (normal ops):", end=" ")
        phase_median = {}
        for key in phase_keys:
            phase_median[key] = statistics.median(row[key] for row in normal)
            print(f"{key}={phase_median[key]:.0f}", end=" ")
        print()
        excess = {key: 0 for key in phase_keys}
        for op_index, _, _ in spikes:
            row = samples[op_index]
            for key in phase_keys:
                excess[key] += max(0, row[key] - phase_median[key])
        total_excess = sum(excess.values()) or 1
        print("spike excess time by phase: " + "  ".join(
            f"{key}={excess[key] / total_excess * 100:.1f}%" for key in phase_keys))

        # Allocation sanity: spike ops doing the same allocations as normal ops
        # rule out an extra-work explanation (drop destruction, cache miss
        # registration) and leave a stall as the only reading.
        spike_allocs = statistics.median(samples[i]["allocs"] for i, _, _ in spikes)
        normal_allocs = statistics.median(row["allocs"] for row in normal)
        print(f"allocs/op: spikes={spike_allocs:.0f} normal={normal_allocs:.0f}")

    # The largest spikes, with context.
    biggest = sorted(spikes, key=lambda s: -s[2])[:10]
    print("largest spikes (op_index, t_ms, latency_us, phases_us):")
    for op_index, offset, latency in biggest:
        row = samples[op_index]
        phases = "/".join(f"{row.get(key, 0) / 1e3:.1f}" for key in phase_keys)
        print(f"  op={op_index:6d} t={offset / 1e6:9.3f}ms {latency / 1e3:8.1f}us  [{phases}]")


if __name__ == "__main__":
    main()
