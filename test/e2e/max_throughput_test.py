#!/usr/bin/env python3
"""Unthrottled maximum-throughput load test for the e2e server."""

import argparse
import concurrent.futures
import math
import os
import sys
import threading
import time
from collections import Counter
from dataclasses import dataclass
from typing import List, Optional, Sequence

from fixed_rps_test import (
    WORKLOADS,
    Endpoint,
    HttpClient,
    ServerAddress,
    get_json,
    parse_server_address,
    percentage,
    positive_float,
    positive_int,
)


def non_negative_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or parsed < 0:
        raise argparse.ArgumentTypeError(
            "must be a finite number greater than or equal to zero"
        )
    return parsed


@dataclass
class TestWindow:
    warmup_start: float = 0.0
    measurement_start: float = 0.0
    end: float = 0.0


class WorkerResults:
    """Single-writer counters; live reporting only reads scalar fields."""

    def __init__(self) -> None:
        self.completed = 0
        self.succeeded = 0
        self.failed = 0
        self.total_latency_ms = 0.0
        self.status_codes = Counter()  # type: Counter[int]
        self.latency_buckets = Counter()  # type: Counter[int]
        self.error_samples = []  # type: List[str]

    def record(
        self,
        endpoint: Endpoint,
        latency_seconds: float,
        status: Optional[int],
        error: Optional[str],
    ) -> None:
        latency_ms = latency_seconds * 1000.0
        self.completed += 1
        self.total_latency_ms += latency_ms
        # A 0.1 ms histogram avoids retaining one float per request during
        # long, high-throughput runs while keeping percentiles useful.
        self.latency_buckets[int(latency_ms * 10.0)] += 1
        if status is not None:
            self.status_codes[status] += 1
        if error is None and status == endpoint.expected_status:
            self.succeeded += 1
            return

        self.failed += 1
        if len(self.error_samples) < 3:
            if error is not None:
                self.error_samples.append(error)
            else:
                self.error_samples.append(
                    f"{endpoint.path}: expected HTTP {endpoint.expected_status}, "
                    f"received HTTP {status}"
                )


def merge_results(worker_results: Sequence[WorkerResults]) -> WorkerResults:
    merged = WorkerResults()
    for result in worker_results:
        merged.completed += result.completed
        merged.succeeded += result.succeeded
        merged.failed += result.failed
        merged.total_latency_ms += result.total_latency_ms
        merged.status_codes.update(result.status_codes)
        merged.latency_buckets.update(result.latency_buckets)
        for sample in result.error_samples:
            if len(merged.error_samples) >= 5:
                break
            merged.error_samples.append(sample)
    return merged


def scalar_snapshot(worker_results: Sequence[WorkerResults]) -> tuple:
    return (
        sum(result.completed for result in worker_results),
        sum(result.failed for result in worker_results),
    )


def histogram_percentile(buckets: Counter, total: int, percent: float) -> float:
    if total == 0:
        return 0.0
    threshold = math.ceil(total * percent / 100.0)
    observed = 0
    for bucket, count in sorted(buckets.items()):
        observed += count
        if observed >= threshold:
            return bucket / 10.0
    return max(buckets, default=0) / 10.0


def wait_until(deadline: float, stop: threading.Event) -> bool:
    while not stop.is_set():
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return True
        if stop.wait(remaining):
            return False
    return False


def worker(
    worker_id: int,
    endpoints: Sequence[Endpoint],
    client: HttpClient,
    window: TestWindow,
    start: threading.Event,
    stop: threading.Event,
    results: WorkerResults,
) -> None:
    start.wait()
    if stop.is_set() or not wait_until(window.warmup_start, stop):
        return

    request_index = 0
    while not stop.is_set():
        started_at = time.monotonic()
        if started_at >= window.end:
            return

        endpoint = endpoints[(worker_id + request_index) % len(endpoints)]
        request_index += 1
        status = None  # type: Optional[int]
        error = None  # type: Optional[str]
        try:
            status = client.get(endpoint.path)
        except Exception as exc:
            error = f"{endpoint.path}: {type(exc).__name__}: {exc}"

        if started_at >= window.measurement_start:
            results.record(endpoint, time.monotonic() - started_at, status, error)


def build_parser() -> argparse.ArgumentParser:
    default_base_url = os.environ.get(
        "BASE_URL",
        "http://{}:{}".format(
            os.environ.get("HOST", "localhost"), os.environ.get("PORT", "8090")
        ),
    )
    parser = argparse.ArgumentParser(
        description=(
            "Saturate e2e_server.cpp endpoints with reusable connections and no "
            "RPS pacing. Concurrency is the only request-generation bound."
        )
    )
    parser.add_argument("--base-url", default=default_base_url)
    parser.add_argument("-d", "--duration", type=positive_float, default=60.0)
    parser.add_argument("-c", "--concurrency", type=positive_int, default=100)
    parser.add_argument("-m", "--mode", choices=sorted(WORKLOADS), default="mixed")
    parser.add_argument(
        "--warmup",
        type=non_negative_float,
        default=2.0,
        metavar="SEC",
        help="unmeasured full-load warm-up duration (default: 2)",
    )
    parser.add_argument("--timeout", type=positive_float, default=30.0)
    parser.add_argument("--report-interval", type=positive_float, default=1.0)
    parser.add_argument(
        "--max-error-rate",
        type=percentage,
        default=0.0,
        metavar="PERCENT",
        help="fail when response errors exceed this percentage (default: 0)",
    )
    parser.add_argument(
        "--min-rps",
        type=non_negative_float,
        default=0.0,
        help="optional minimum average throughput required to pass",
    )
    parser.add_argument(
        "--no-require-agent",
        action="store_true",
        help="do not require the server's /ready endpoint to report an enabled agent",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        server = parse_server_address(args.base_url)
    except ValueError as exc:
        print(f"ERROR: invalid --base-url: {exc}", file=sys.stderr)
        return 2

    try:
        preflight_stats = get_json(server, "/stats", min(args.timeout, 5.0))
        missing_stats = {"total_requests", "active_requests"} - preflight_stats.keys()
        if missing_stats:
            raise RuntimeError(
                "/stats is missing required fields: " + ", ".join(sorted(missing_stats))
            )
        if not args.no_require_agent:
            get_json(server, "/ready", min(args.timeout, 5.0))
    except Exception as exc:
        print(f"ERROR: e2e server pre-flight check failed: {exc}", file=sys.stderr)
        return 2

    endpoints = WORKLOADS[args.mode]
    print("=" * 68)
    print(" Pinpoint C++ Agent - Maximum Throughput Load Test")
    print("=" * 68)
    print(f"Server:       {args.base_url.rstrip('/')}")
    print(f"Mode:         {args.mode}")
    print(f"Concurrency:  {args.concurrency}")
    print(f"Warm-up:      {args.warmup:.2f}s (excluded from results)")
    print(f"Duration:     {args.duration:.2f}s")
    print(f"Endpoints:    {len(endpoints)} (deterministic rotation)")
    print("Rate limit:   none")
    print("=" * 68)

    client = HttpClient(
        server, args.timeout, user_agent="pinpoint-max-throughput-test/1.0"
    )
    start_event = threading.Event()
    stop_event = threading.Event()
    window = TestWindow()
    worker_results = [WorkerResults() for _ in range(args.concurrency)]
    executor = concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency)
    futures = []
    try:
        for worker_id in range(args.concurrency):
            futures.append(
                executor.submit(
                    worker,
                    worker_id,
                    endpoints,
                    client,
                    window,
                    start_event,
                    stop_event,
                    worker_results[worker_id],
                )
            )
    except Exception as exc:
        stop_event.set()
        start_event.set()
        executor.shutdown(wait=True)
        print(f"ERROR: failed to start load workers: {exc}", file=sys.stderr)
        return 2

    window.warmup_start = time.monotonic() + 0.1
    window.measurement_start = window.warmup_start + args.warmup
    window.end = window.measurement_start + args.duration
    start_event.set()
    interrupted = False
    measurement_stats = None

    try:
        if args.warmup > 0:
            print(f"Warming up at full load for {args.warmup:.2f}s...", flush=True)
        if not wait_until(window.measurement_start, stop_event):
            raise KeyboardInterrupt
        try:
            measurement_stats = get_json(server, "/stats", min(args.timeout, 2.0))
        except Exception:
            pass

        print("Elapsed | Interval RPS | Completed | Errors | Server active")
        print("--------|--------------|-----------|--------|--------------")
        previous_completed = 0
        previous_time = window.measurement_start
        next_report = min(
            window.measurement_start + args.report_interval, window.end
        )
        while not stop_event.is_set():
            if not wait_until(next_report, stop_event):
                break
            now = time.monotonic()
            completed, failed = scalar_snapshot(worker_results)
            sample_seconds = max(now - previous_time, 1e-9)
            sample_rps = (completed - previous_completed) / sample_seconds
            server_active = "?"
            try:
                server_active = str(
                    get_json(server, "/stats", min(args.report_interval, 2.0)).get(
                        "active_requests", "?"
                    )
                )
            except Exception:
                pass
            print(
                f"{min(now - window.measurement_start, args.duration):7.1f} | "
                f"{sample_rps:12.2f} | {completed:9d} | {failed:6d} | "
                f"{server_active:>13}",
                flush=True,
            )
            previous_completed = completed
            previous_time = now
            if now >= window.end:
                break
            next_report = min(next_report + args.report_interval, window.end)
    except KeyboardInterrupt:
        interrupted = True
        print("\nInterrupted; waiting for active requests...", file=sys.stderr)
    finally:
        stop_event.set()
        executor.shutdown(wait=True)

    for future in futures:
        try:
            future.result()
        except Exception as exc:
            print(f"ERROR: load worker failed: {exc}", file=sys.stderr)
            return 1

    try:
        final_stats = get_json(server, "/stats", min(args.timeout, 5.0))
    except Exception:
        final_stats = None

    results = merge_results(worker_results)
    achieved_rps = results.completed / args.duration
    error_rate = (
        results.failed / results.completed * 100.0 if results.completed else 100.0
    )
    average_latency = (
        results.total_latency_ms / results.completed if results.completed else 0.0
    )
    server_delta = None
    if measurement_stats is not None and final_stats is not None:
        server_delta = (
            final_stats.get("total_requests", 0)
            - measurement_stats.get("total_requests", 0)
        )

    print("\n" + "=" * 68)
    print(" Results (warm-up excluded)")
    print("=" * 68)
    print(f"Completed requests: {results.completed}")
    print(f"Successful:         {results.succeeded}")
    print(f"Failed:             {results.failed} ({error_rate:.2f}%)")
    print(f"Average RPS:        {achieved_rps:.2f}")
    if server_delta is not None:
        print(f"Server request delta (approx.): {server_delta}")
    if results.status_codes:
        print(
            "HTTP status codes:  "
            + ", ".join(
                f"{status}={count}"
                for status, count in sorted(results.status_codes.items())
            )
        )
    print(
        "Latency (ms):      "
        f"avg={average_latency:.2f}, "
        f"p50={histogram_percentile(results.latency_buckets, results.completed, 50):.2f}, "
        f"p95={histogram_percentile(results.latency_buckets, results.completed, 95):.2f}, "
        f"p99={histogram_percentile(results.latency_buckets, results.completed, 99):.2f}, "
        f"max={max(results.latency_buckets, default=0) / 10.0:.2f}"
    )
    for sample in results.error_samples:
        print(f"  ERROR: {sample}")

    if interrupted:
        return 130

    failures = []
    if results.completed == 0:
        failures.append("no workload requests completed")
    if error_rate > args.max_error_rate:
        failures.append(
            f"error rate {error_rate:.2f}% exceeds {args.max_error_rate:.2f}%"
        )
    if achieved_rps < args.min_rps:
        failures.append(
            f"average RPS {achieved_rps:.2f} is below minimum {args.min_rps:.2f}"
        )

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("PASS: maximum-throughput load test met its configured thresholds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
