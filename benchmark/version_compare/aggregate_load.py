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

"""Aggregates run_load_compare.sh output into a comparison report.

The interesting quantity is not any variant's absolute latency — loopback HTTP
through a Python generator has its own floor — but the *agent overhead*: the
same server binary measured with the agent enabled minus with it disabled, and
that difference compared across versions at the same offered load.
"""

import argparse
import json
import re
import statistics
from pathlib import Path


LATENCY_RE = re.compile(
    r"Latency \(ms\):\s+p50=([\d.]+), p95=([\d.]+), p99=([\d.]+), max=([\d.]+)")
ACHIEVED_RE = re.compile(r"Achieved start RPS:\s+([\d.]+)")


def parse_load(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    lat = LATENCY_RE.search(text)
    ach = ACHIEVED_RE.search(text)
    if not lat or not ach:
        return None
    return {
        "p50": float(lat.group(1)),
        "p95": float(lat.group(2)),
        "p99": float(lat.group(3)),
        "max": float(lat.group(4)),
        "achieved_rps": float(ach.group(1)),
        "passed": "PASS:" in text and "LOADFAIL" not in text,
    }


def parse_ps(path):
    rss_kib = []
    pcpu = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        fields = line.split()
        if len(fields) != 2:
            continue
        try:
            rss_kib.append(float(fields[0]))
            pcpu.append(float(fields[1]))
        except ValueError:
            continue
    if not rss_kib:
        return None
    return {
        "rss_max_mib": max(rss_kib) / 1024.0,
        # Skip the first samples: they still include the warm-up ramp.
        "cpu_mean": statistics.mean(pcpu[2:]) if len(pcpu) > 4 else statistics.mean(pcpu),
    }


def parse_collector(path):
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("COUNTERS"):
            fields = line.split("\t")
            # COUNTERS agent_infos metadata span_messages span_batches spans_in_batches stats
            return int(fields[3]) + int(fields[5])
    return None


def med(values):
    return statistics.median(values) if values else 0.0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--rps-list", required=True)
    parser.add_argument("--repeats", type=int, required=True)
    parser.add_argument("--duration", type=int, default=0)
    parser.add_argument("--mode", default="mixed")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    out_dir = Path(args.dir)
    rps_values = args.rps_list.split()
    variants = [
        args.baseline,
        args.candidate,
        f"{args.baseline}-noagent",
        f"{args.candidate}-noagent",
    ]

    # data[variant][rps] = list of per-rep dicts
    data = {v: {r: [] for r in rps_values} for v in variants}
    delivery = {v: [] for v in variants}
    requests_served = {v: [] for v in variants}

    for variant in variants:
        for rep in range(1, args.repeats + 1):
            tag = f"{variant}-rep{rep}"
            for rps in rps_values:
                load_path = out_dir / f"{tag}-rps{rps}.load.txt"
                ps_path = out_dir / f"{tag}-rps{rps}.ps.txt"
                if not load_path.exists():
                    continue
                entry = parse_load(load_path)
                if entry is None:
                    continue
                ps_entry = parse_ps(ps_path) if ps_path.exists() else None
                if ps_entry:
                    entry.update(ps_entry)
                data[variant][rps].append(entry)
            collector_path = out_dir / f"{tag}.collector.txt"
            if collector_path.exists():
                delivered = parse_collector(collector_path)
                if delivered is not None:
                    delivery[variant].append(delivered)
            stats_path = out_dir / f"{tag}.stats.json"
            if stats_path.exists():
                try:
                    requests_served[variant].append(
                        json.loads(stats_path.read_text())["total_requests"])
                except Exception:
                    pass

    lines = []
    lines.append(f"# {args.baseline} vs {args.candidate} — HTTP load comparison")
    lines.append("")
    lines.append(f"`fixed_rps_test.py --mode {args.mode}`, {args.duration}s per pass, "
                 f"{args.repeats} interleaved repetitions, medians across repetitions. "
                 f"Latency is end-to-end from the Python client on loopback; CPU% and RSS "
                 f"are the server process sampled every 0.5 s during the pass.")
    lines.append("")

    lines.append("## Span delivery")
    lines.append("")
    lines.append("| variant | workload requests served (median/run) | spans delivered (median/run) |")
    lines.append("|---|---:|---:|")
    for variant in variants:
        served = med(requests_served[variant])
        delivered = med(delivery[variant])
        lines.append(f"| {variant} | {served:,.0f} | {delivered:,.0f} |")
    lines.append("")
    lines.append("A `-noagent` variant delivering ~0 confirms the baseline really ran without "
                 "an agent. An enabled variant must deliver at least its request count "
                 "(spans can exceed requests: `/features` adds an async span per hit).")
    lines.append("")

    for rps in rps_values:
        lines.append(f"## {rps} RPS")
        lines.append("")
        lines.append("| variant | achieved RPS | p50 ms | p95 ms | p99 ms | max ms | CPU % | RSS MiB | passes |")
        lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
        for variant in variants:
            entries = data[variant][rps]
            if not entries:
                lines.append(f"| {variant} | (no data) | | | | | | | |")
                continue
            passes = sum(1 for e in entries if e["passed"])
            lines.append(
                f"| {variant} "
                f"| {med([e['achieved_rps'] for e in entries]):,.0f} "
                f"| {med([e['p50'] for e in entries]):.3f} "
                f"| {med([e['p95'] for e in entries]):.3f} "
                f"| {med([e['p99'] for e in entries]):.3f} "
                f"| {med([e['max'] for e in entries]):.2f} "
                f"| {med([e.get('cpu_mean', 0) for e in entries]):.1f} "
                f"| {med([e.get('rss_max_mib', 0) for e in entries]):.1f} "
                f"| {passes}/{len(entries)} |")
        lines.append("")

        lines.append("### Agent overhead (enabled − disabled, same binary)")
        lines.append("")
        lines.append("| version | Δp50 ms | Δp99 ms | ΔCPU % |")
        lines.append("|---|---:|---:|---:|")
        for version in (args.baseline, args.candidate):
            on = data[version][rps]
            off = data[f"{version}-noagent"][rps]
            if not on or not off:
                lines.append(f"| {version} | (no data) | | |")
                continue
            dp50 = med([e["p50"] for e in on]) - med([e["p50"] for e in off])
            dp99 = med([e["p99"] for e in on]) - med([e["p99"] for e in off])
            dcpu = med([e.get("cpu_mean", 0) for e in on]) - med([e.get("cpu_mean", 0) for e in off])
            lines.append(f"| {version} | {dp50:+.3f} | {dp99:+.3f} | {dcpu:+.1f} |")
        lines.append("")

    report = "\n".join(lines)
    Path(args.out).write_text(report + "\n", encoding="utf-8")
    print(report)


if __name__ == "__main__":
    main()
