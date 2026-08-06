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

"""Aggregates interleaved api_benchmark runs into a comparison report.

Medians, not means: a single scheduling hiccup on a laptop moves a mean by more
than most of the differences being measured. The spread across repetitions is
reported next to each median so a difference smaller than the noise is visible
as such rather than being read as a result.
"""

import argparse
import statistics
from collections import defaultdict


def parse_raw(path):
    """Returns (results, peak_rss, created, delivered).

    results: {(variant, scenario): {"threads": int, "ns": [...], "p50": [...],
                                    "p99": [...], "max": [...], "alloc": [...]}}
    peak_rss: {variant: [kib, ...]}
    created:  {variant: [recording spans handed to the agent, ...]}
    delivered:{variant: [spans the collector received, ...]}
    """
    results = defaultdict(lambda: {"threads": 1, "ns": [], "p50": [],
                                   "p99": [], "max": [], "alloc": []})
    peak_rss = defaultdict(list)
    created = defaultdict(list)
    delivered = defaultdict(list)

    with open(path, encoding="utf-8") as handle:
        for line in handle:
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 3:
                continue
            kind = fields[1]
            if kind == "RESULT" and len(fields) >= 11:
                _, _, variant, scenario, threads, ns, p50, p99, mx, alloc, ops = fields[:11]
                entry = results[(variant, scenario)]
                entry["threads"] = int(threads)
                entry["ns"].append(float(ns))
                entry["p50"].append(float(p50))
                entry["p99"].append(float(p99))
                entry["max"].append(float(mx))
                entry["alloc"].append(float(alloc))
            elif kind == "PEAKRSS" and len(fields) >= 4:
                peak_rss[fields[2]].append(float(fields[3]))
            elif kind == "SPANS" and len(fields) >= 4:
                created[fields[2]].append(float(fields[3]))
            elif kind == "DELIVERED" and len(fields) >= 4:
                delivered[fields[2]].append(float(fields[3]))

    return results, peak_rss, created, delivered


def spread(values):
    """Half the min-max range as a percentage of the median."""
    if len(values) < 2:
        return 0.0
    median = statistics.median(values)
    if median == 0:
        return 0.0
    return (max(values) - min(values)) / 2.0 / median * 100.0


def change(baseline, candidate):
    """Percentage change from baseline to candidate; negative is faster."""
    if baseline == 0:
        return 0.0
    return (candidate - baseline) / baseline * 100.0


def format_change(value):
    if value <= -1.0:
        return f"**{value:+.1f}%**"
    if value >= 1.0:
        return f"`{value:+.1f}%`"
    return f"{value:+.1f}%"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw", required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--ops", type=int, default=0)
    parser.add_argument("--repeats", type=int, default=0)
    args = parser.parse_args()

    results, peak_rss, created, delivered = parse_raw(args.raw)

    scenarios = []
    for (variant, scenario) in results:
        if variant == args.baseline and (args.candidate, scenario) in results:
            scenarios.append(scenario)
    scenarios.sort()

    lines = []
    lines.append(f"# {args.baseline} vs {args.candidate} — public API benchmark")
    lines.append("")
    lines.append(f"{args.repeats} interleaved repetitions, {args.ops} measured ops per thread "
                 f"per scenario, medians across repetitions.")
    lines.append("")
    lines.append("`ns/op` is wall time divided by ops *per thread*, so for the threaded rows a "
                 "flat value across thread counts means the path is not accumulating cross-core "
                 "contention. `±` is half the min-max range across repetitions; a change smaller "
                 "than the spread is noise, not a result.")
    lines.append("")

    # Validity gate, before any timing is shown. If one version's sender falls
    # behind, its EndSpan takes a cheap drop path instead of serializing and
    # sending, and it measures as faster while delivering less. Unequal delivery
    # makes the per-span comparison meaningless, so it is reported first.
    lines.append("## Span delivery (validity gate)")
    lines.append("")
    lines.append("`messages delivered` counts what the collector received. It legitimately exceeds "
                 "the span count, because a span carrying many events is split into a span plus "
                 "chunk messages — which also means `delivered >= created` alone leaves slack: a "
                 "run could drop a share of its spans and still clear 100% on message inflation. "
                 "The gate is therefore applied per run, and on top of it the two variants must "
                 "deliver the same message count — equal offered work must produce equal messages.")
    lines.append("")
    lines.append("| variant | spans created (median) | messages delivered (median) | ratio "
                 "| runs below 100% |")
    lines.append("|---|---:|---:|---:|---:|")
    delivery_warning = []
    delivered_medians = {}
    for variant in (args.baseline, args.candidate):
        if variant not in created or variant not in delivered:
            continue
        created_runs = created[variant]
        delivered_runs = delivered[variant]
        created_median = statistics.median(created_runs)
        delivered_median = statistics.median(delivered_runs)
        delivered_medians[variant] = delivered_median
        rate = (delivered_median / created_median * 100.0) if created_median else 0.0
        # Per run, not median-of-runs: one dropping repetition poisons every
        # scenario median it contributed to, and a cross-rep median would let
        # it pass unnoticed.
        failing_runs = sum(1 for c, d in zip(created_runs, delivered_runs) if d < c)
        lines.append(f"| {variant} | {created_median:,.0f} | {delivered_median:,.0f} "
                     f"| {rate:.1f}% | {failing_runs}/{len(delivered_runs)} |")
        if rate < 100.0 or failing_runs:
            delivery_warning.append((variant, rate, failing_runs))
    lines.append("")
    mismatch = None
    if len(delivered_medians) == 2:
        base_delivered = delivered_medians[args.baseline]
        cand_delivered = delivered_medians[args.candidate]
        if base_delivered != cand_delivered:
            mismatch = (base_delivered, cand_delivered)
    if delivery_warning:
        detail = ", ".join(f"{variant} at {rate:.1f}% with {failing} dropping run(s)"
                           for variant, rate, failing in delivery_warning)
        lines.append(f"> **The timings below are not comparable.** {detail}. A version that drops "
                     f"spans skips the serialize-and-send work the other version performs, so it "
                     f"measures as faster while delivering less. Lower `--ops` or raise "
                     f"`--drain-ms` until every run of both versions is at or above 100%.")
    elif mismatch:
        lines.append(f"> **Treat the timings below with suspicion: the variants delivered "
                     f"different message counts** ({args.baseline}: {mismatch[0]:,.0f}, "
                     f"{args.candidate}: {mismatch[1]:,.0f}) for identical offered work. Either "
                     f"the lower side dropped spans inside the slack that message inflation "
                     f"leaves under `delivered >= created`, or the two versions chunk span "
                     f"events differently — determine which before reading the deltas as "
                     f"per-span costs.")
    else:
        lines.append("Every run delivered at least its created count and both variants delivered "
                     "identical message counts, so the per-span timings below reflect equal work.")
    lines.append("")

    header = (f"| scenario | thr | {args.baseline} ns/op | {args.candidate} ns/op | Δ ns/op | "
              f"{args.baseline} p99 | {args.candidate} p99 | Δ p99 | "
              f"{args.baseline} alloc/op | {args.candidate} alloc/op |")
    lines.append(header)
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")

    regressions = []
    for scenario in scenarios:
        base = results[(args.baseline, scenario)]
        cand = results[(args.candidate, scenario)]

        base_ns = statistics.median(base["ns"])
        cand_ns = statistics.median(cand["ns"])
        base_p99 = statistics.median(base["p99"])
        cand_p99 = statistics.median(cand["p99"])
        base_alloc = statistics.median(base["alloc"])
        cand_alloc = statistics.median(cand["alloc"])

        ns_change = change(base_ns, cand_ns)
        p99_change = change(base_p99, cand_p99)
        noise = max(spread(base["ns"]), spread(cand["ns"]))

        lines.append(
            f"| {scenario} | {base['threads']} "
            f"| {base_ns:,.0f} ±{spread(base['ns']):.0f}% "
            f"| {cand_ns:,.0f} ±{spread(cand['ns']):.0f}% "
            f"| {format_change(ns_change)} "
            f"| {base_p99:,.0f} | {cand_p99:,.0f} | {format_change(p99_change)} "
            f"| {base_alloc:.1f} | {cand_alloc:.1f} |")

        if ns_change > 0 and ns_change > noise:
            regressions.append((scenario, ns_change, noise))

    lines.append("")
    if peak_rss:
        lines.append("## Peak RSS")
        lines.append("")
        lines.append("| variant | peak RSS (MiB) |")
        lines.append("|---|---:|")
        for variant in (args.baseline, args.candidate):
            if variant in peak_rss:
                lines.append(f"| {variant} | {statistics.median(peak_rss[variant]) / 1024:,.1f} |")
        lines.append("")

    lines.append("## Regressions")
    lines.append("")
    if regressions:
        lines.append("Scenarios where the candidate is slower by more than the measured noise:")
        lines.append("")
        for scenario, ns_change, noise in sorted(regressions, key=lambda r: -r[1]):
            lines.append(f"- `{scenario}`: {ns_change:+.1f}% ns/op (run-to-run spread ±{noise:.0f}%)")
    else:
        lines.append("None: no scenario is slower by more than its run-to-run spread.")
    lines.append("")

    report = "\n".join(lines)
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write(report + "\n")
    print(report)


if __name__ == "__main__":
    main()
