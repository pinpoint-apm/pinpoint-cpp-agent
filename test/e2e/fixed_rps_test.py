#!/usr/bin/env python3
"""Constant-arrival-rate load test for the e2e integration server."""

import argparse
import concurrent.futures
import http.client
import json
import math
import os
import sys
import threading
import time
from collections import Counter
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple
from urllib.parse import urlsplit


@dataclass(frozen=True)
class Endpoint:
    path: str
    expected_status: int = 200


WORKLOADS: Dict[str, Tuple[Endpoint, ...]] = {
    "simple": (Endpoint("/simple"),),
    "deep": (
        Endpoint("/deep?depth=10"),
        Endpoint("/deep?depth=30"),
        Endpoint("/deep?depth=50"),
    ),
    "wide": (
        Endpoint("/wide?width=20"),
        Endpoint("/wide?width=100"),
        Endpoint("/wide?width=300"),
    ),
    "annotated": (Endpoint("/annotated"),),
    "features": (Endpoint("/features"),),
    "http": (
        Endpoint("/http-client"),
        Endpoint("/http-client?error=1"),
    ),
    "limits": (
        Endpoint("/deep?depth=32"),
        Endpoint("/wide?width=256"),
    ),
    "mixed": (
        Endpoint("/simple"),
        Endpoint("/deep?depth=10"),
        Endpoint("/deep?depth=30"),
        Endpoint("/wide?width=20"),
        Endpoint("/wide?width=100"),
        Endpoint("/annotated"),
        Endpoint("/features"),
        Endpoint("/mixed"),
        Endpoint("/error", expected_status=500),
    ),
    "stress": (),  # Populated from mixed below; RPS controls stress intensity.
    "db-crud": (Endpoint("/db-crud"),),
    "db-batch": (
        Endpoint("/db-batch?size=10"),
        Endpoint("/db-batch?size=50"),
        Endpoint("/db-batch?size=100"),
    ),
    "db-complex": (Endpoint("/db-complex"),),
    "db-all": (
        Endpoint("/db-crud"),
        Endpoint("/db-batch?size=10"),
        Endpoint("/db-batch?size=50"),
        Endpoint("/db-complex"),
    ),
    "grpc-unary": (Endpoint("/grpc-unary"),),
    "grpc-stream": (Endpoint("/grpc-stream"),),
    "grpc-client-stream": (Endpoint("/grpc-client-stream?count=5"),),
    "grpc-bidi": (
        Endpoint("/grpc-bidi?count=3"),
        Endpoint("/grpc-bidi?count=10"),
    ),
    "grpc-all": (
        Endpoint("/grpc-unary"),
        Endpoint("/grpc-stream"),
        Endpoint("/grpc-client-stream?count=5"),
        Endpoint("/grpc-bidi?count=3"),
        Endpoint("/grpc-all"),
    ),
    "full": (
        Endpoint("/simple"),
        Endpoint("/deep?depth=10"),
        Endpoint("/deep?depth=30"),
        Endpoint("/wide?width=20"),
        Endpoint("/wide?width=100"),
        Endpoint("/annotated"),
        Endpoint("/features"),
        Endpoint("/mixed"),
        Endpoint("/error", expected_status=500),
        Endpoint("/http-client"),
        Endpoint("/grpc-unary"),
        Endpoint("/grpc-stream"),
        Endpoint("/grpc-client-stream?count=5"),
        Endpoint("/grpc-bidi?count=3"),
        Endpoint("/grpc-all"),
        Endpoint("/db-crud"),
        Endpoint("/db-batch?size=20"),
        Endpoint("/db-complex"),
    ),
}
WORKLOADS["stress"] = WORKLOADS["mixed"]


def positive_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or parsed <= 0:
        raise argparse.ArgumentTypeError("must be a finite number greater than zero")
    return parsed


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def percentage(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or not 0 <= parsed <= 100:
        raise argparse.ArgumentTypeError("must be between 0 and 100")
    return parsed


@dataclass(frozen=True)
class ServerAddress:
    scheme: str
    host: str
    port: int
    base_path: str

    def target(self, endpoint_path: str) -> str:
        return self.base_path.rstrip("/") + endpoint_path


def parse_server_address(base_url: str) -> ServerAddress:
    parsed = urlsplit(base_url)
    if parsed.scheme not in ("http", "https"):
        raise ValueError("base URL scheme must be http or https")
    if not parsed.hostname:
        raise ValueError("base URL must include a host")
    if parsed.query or parsed.fragment:
        raise ValueError("base URL must not include a query or fragment")
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    return ServerAddress(parsed.scheme, parsed.hostname, port, parsed.path.rstrip("/"))


class HttpClient:
    """One reusable HTTP connection per worker thread."""

    def __init__(
        self,
        server: ServerAddress,
        timeout: float,
        user_agent: str = "pinpoint-e2e-load-test/1.0",
    ) -> None:
        self.server = server
        self.timeout = timeout
        self.user_agent = user_agent
        self.local = threading.local()

    def _new_connection(self) -> http.client.HTTPConnection:
        connection_type = (
            http.client.HTTPSConnection
            if self.server.scheme == "https"
            else http.client.HTTPConnection
        )
        return connection_type(self.server.host, self.server.port, timeout=self.timeout)

    def _connection(self) -> http.client.HTTPConnection:
        connection = getattr(self.local, "connection", None)
        if connection is None:
            connection = self._new_connection()
            self.local.connection = connection
        return connection

    def get(self, path: str) -> int:
        connection = self._connection()
        try:
            connection.request(
                "GET",
                self.server.target(path),
                headers={"User-Agent": self.user_agent},
            )
            response = connection.getresponse()
            response.read()
            status = response.status
            if response.will_close:
                connection.close()
                self.local.connection = None
            return status
        except Exception:
            connection.close()
            self.local.connection = None
            raise


def get_json(server: ServerAddress, path: str, timeout: float) -> dict:
    connection_type = (
        http.client.HTTPSConnection
        if server.scheme == "https"
        else http.client.HTTPConnection
    )
    connection = connection_type(server.host, server.port, timeout=timeout)
    try:
        connection.request(
            "GET",
            server.target(path),
            headers={"User-Agent": "pinpoint-e2e-load-test/1.0"},
        )
        response = connection.getresponse()
        body = response.read().decode("utf-8", errors="replace")
        if not 200 <= response.status < 300:
            raise RuntimeError(f"GET {path} returned HTTP {response.status}: {body}")
        decoded = json.loads(body)
        if not isinstance(decoded, dict):
            raise RuntimeError(f"GET {path} did not return a JSON object")
        return decoded
    finally:
        connection.close()


class Results:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.started = 0
        self.completed = 0
        self.succeeded = 0
        self.failed = 0
        self.dropped = Counter()  # type: Counter[str]
        self.status_codes = Counter()  # type: Counter[int]
        self.latencies_ms = []  # type: List[float]
        self.schedule_lags_ms = []  # type: List[float]
        self.error_samples = []  # type: List[str]

    def record_started(self, lag_seconds: float) -> None:
        with self.lock:
            self.started += 1
            self.schedule_lags_ms.append(max(lag_seconds, 0.0) * 1000.0)

    def record_completed(
        self,
        latency_seconds: float,
        status: Optional[int],
        expected_status: int,
        error: Optional[str],
    ) -> None:
        with self.lock:
            self.completed += 1
            self.latencies_ms.append(latency_seconds * 1000.0)
            if status is not None:
                self.status_codes[status] += 1
            if error is None and status == expected_status:
                self.succeeded += 1
            else:
                self.failed += 1
                if len(self.error_samples) < 5:
                    if error is not None:
                        self.error_samples.append(error)
                    else:
                        self.error_samples.append(
                            f"expected HTTP {expected_status}, received HTTP {status}"
                        )

    def record_dropped(self, reason: str) -> None:
        with self.lock:
            self.dropped[reason] += 1

    def snapshot(self) -> Tuple[int, int, int]:
        with self.lock:
            return self.started, self.completed, sum(self.dropped.values())


def percentile(values: Sequence[float], percent: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, math.ceil(percent / 100.0 * len(ordered)) - 1)
    return ordered[index]


def execute_request(
    client: HttpClient,
    endpoint: Endpoint,
    deadline: float,
    max_schedule_lag: float,
    results: Results,
    capacity: threading.Semaphore,
) -> None:
    started_at = time.monotonic()
    schedule_lag = started_at - deadline
    # Thread-pool startup or host scheduling can delay work after submission.
    # Skip a request that is already a full arrival period late so it cannot
    # become part of a catch-up burst.
    if schedule_lag >= max_schedule_lag:
        results.record_dropped("worker_lag")
        capacity.release()
        return

    results.record_started(schedule_lag)
    status = None  # type: Optional[int]
    error = None  # type: Optional[str]
    try:
        status = client.get(endpoint.path)
    except Exception as exc:  # The exception type is included in the report.
        error = f"{endpoint.path}: {type(exc).__name__}: {exc}"
    finally:
        results.record_completed(
            time.monotonic() - started_at,
            status,
            endpoint.expected_status,
            error,
        )
        capacity.release()


def report_progress(
    done: threading.Event,
    started_at: float,
    duration: float,
    interval: float,
    target_rps: float,
    server: ServerAddress,
    results: Results,
) -> None:
    previous_started = 0
    previous_time = started_at
    print(
        "Elapsed | Target RPS | Started RPS | Completed | "
        "In flight | Dropped | Server active"
    )
    print(
        "--------|------------|-------------|-----------|"
        "-----------|---------|--------------"
    )
    while not done.wait(interval):
        now = time.monotonic()
        started, completed, dropped = results.snapshot()
        sample_duration = max(now - previous_time, 1e-9)
        sample_rps = (started - previous_started) / sample_duration
        server_active = "?"
        try:
            server_active = str(
                get_json(server, "/stats", min(interval, 2.0)).get(
                    "active_requests", "?"
                )
            )
        except Exception:
            pass
        print(
            f"{min(now - started_at, duration):7.1f} | {target_rps:10.2f} | "
            f"{sample_rps:11.2f} | {completed:9d} | {started - completed:9d} | "
            f"{dropped:7d} | {server_active:>13}",
            flush=True,
        )
        previous_started = started
        previous_time = now


def build_parser() -> argparse.ArgumentParser:
    default_base_url = os.environ.get(
        "BASE_URL",
        "http://{}:{}".format(
            os.environ.get("HOST", "localhost"), os.environ.get("PORT", "8090")
        ),
    )
    parser = argparse.ArgumentParser(
        description=(
            "Send requests to e2e_server.cpp endpoints at a constant arrival rate. "
            "Requests are paced by monotonic deadlines rather than completion rate."
        )
    )
    parser.add_argument("--base-url", default=default_base_url)
    parser.add_argument("-r", "--rps", type=positive_float, default=10.0)
    parser.add_argument("-d", "--duration", type=positive_float, default=60.0)
    parser.add_argument(
        "-c",
        "--max-in-flight",
        type=positive_int,
        default=100,
        help="maximum concurrent requests; saturated arrivals are dropped (default: 100)",
    )
    parser.add_argument("-m", "--mode", choices=sorted(WORKLOADS), default="mixed")
    parser.add_argument("--timeout", type=positive_float, default=30.0)
    parser.add_argument("--report-interval", type=positive_float, default=1.0)
    parser.add_argument(
        "--max-error-rate",
        type=percentage,
        default=0.0,
        metavar="PERCENT",
        help="fail when completed-request errors exceed this percentage (default: 0)",
    )
    parser.add_argument(
        "--rps-tolerance",
        type=percentage,
        default=5.0,
        metavar="PERCENT",
        help="allowed percentage of planned arrivals that may be dropped (default: 5)",
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
        initial_stats = get_json(server, "/stats", min(args.timeout, 5.0))
        missing_stats = {"total_requests", "active_requests"} - initial_stats.keys()
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
    interval = 1.0 / args.rps
    planned = math.floor(args.duration * args.rps + 1e-9)
    if planned < 1:
        print(
            "ERROR: duration and RPS produce no scheduled requests; increase either value",
            file=sys.stderr,
        )
        return 2

    print("=" * 64)
    print(" Pinpoint C++ Agent - Fixed RPS Load Test")
    print("=" * 64)
    print(f"Server:        {args.base_url.rstrip('/')}")
    print(f"Mode:          {args.mode}")
    print(f"Target RPS:    {args.rps:.2f}")
    print(f"Duration:      {args.duration:.2f}s")
    print(f"Planned:       {planned} requests")
    print(f"Max in flight: {args.max_in_flight}")
    print(f"Endpoints:     {len(endpoints)} (deterministic round-robin)")
    print("=" * 64)

    client = HttpClient(server, args.timeout)
    results = Results()
    capacity = threading.BoundedSemaphore(args.max_in_flight)
    reporter_done = threading.Event()
    interrupted = False
    executor = concurrent.futures.ThreadPoolExecutor(max_workers=args.max_in_flight)
    # Avoid charging the first scheduled arrival for lazy worker-thread startup.
    executor.submit(lambda: None).result()
    start = time.monotonic()
    reporter = threading.Thread(
        target=report_progress,
        args=(
            reporter_done,
            start,
            args.duration,
            args.report_interval,
            args.rps,
            server,
            results,
        ),
        daemon=True,
    )
    reporter.start()

    try:
        for request_index in range(planned):
            deadline = start + (request_index + 1) * interval
            delay = deadline - time.monotonic()
            if delay > 0:
                time.sleep(delay)

            # Do not catch up by emitting a burst when an entire arrival period
            # has already elapsed. Such a request is reported as scheduler lag.
            if time.monotonic() - deadline >= interval:
                results.record_dropped("scheduler_lag")
                continue
            if not capacity.acquire(blocking=False):
                results.record_dropped("max_in_flight")
                continue

            endpoint = endpoints[request_index % len(endpoints)]
            try:
                executor.submit(
                    execute_request,
                    client,
                    endpoint,
                    deadline,
                    interval,
                    results,
                    capacity,
                )
            except Exception:
                capacity.release()
                results.record_dropped("submit_error")
                raise
    except KeyboardInterrupt:
        interrupted = True
        print("\nInterrupted; waiting for in-flight requests...", file=sys.stderr)
    finally:
        executor.shutdown(wait=True)
        reporter_done.set()
        reporter.join(timeout=args.report_interval + 1.0)

    finished = time.monotonic()
    try:
        final_stats = get_json(server, "/stats", min(args.timeout, 5.0))
    except Exception:
        final_stats = {}

    started, completed, dropped = results.snapshot()
    error_rate = (results.failed / completed * 100.0) if completed else 100.0
    drop_rate = dropped / planned * 100.0
    achieved_rps = started / args.duration
    elapsed = finished - start
    server_delta = None
    if "total_requests" in initial_stats and "total_requests" in final_stats:
        server_delta = final_stats["total_requests"] - initial_stats["total_requests"]

    print("\n" + "=" * 64)
    print(" Results")
    print("=" * 64)
    print(f"Planned arrivals:   {planned}")
    print(f"Started requests:   {started}")
    print(f"Completed requests: {completed}")
    print(f"Successful:         {results.succeeded}")
    print(f"Failed:             {results.failed} ({error_rate:.2f}%)")
    print(f"Dropped:            {dropped} ({drop_rate:.2f}%)")
    print(f"Achieved start RPS: {achieved_rps:.2f}")
    print(f"Total wall time:    {elapsed:.2f}s")
    if server_delta is not None:
        print(f"Server request delta: {server_delta}")

    if results.dropped:
        print(
            "Dropped by reason:  "
            + ", ".join(
                f"{reason}={count}"
                for reason, count in sorted(results.dropped.items())
            )
        )
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
        f"p50={percentile(results.latencies_ms, 50):.2f}, "
        f"p95={percentile(results.latencies_ms, 95):.2f}, "
        f"p99={percentile(results.latencies_ms, 99):.2f}, "
        f"max={max(results.latencies_ms, default=0.0):.2f}"
    )
    print(
        "Schedule lag (ms): "
        f"p50={percentile(results.schedule_lags_ms, 50):.2f}, "
        f"p95={percentile(results.schedule_lags_ms, 95):.2f}, "
        f"p99={percentile(results.schedule_lags_ms, 99):.2f}, "
        f"max={max(results.schedule_lags_ms, default=0.0):.2f}"
    )
    for sample in results.error_samples:
        print(f"  ERROR: {sample}")

    if interrupted:
        return 130

    failures = []
    if started == 0:
        failures.append("no workload requests were started")
    if error_rate > args.max_error_rate:
        failures.append(
            f"error rate {error_rate:.2f}% exceeds {args.max_error_rate:.2f}%"
        )
    if drop_rate > args.rps_tolerance:
        failures.append(
            f"dropped-arrival rate {drop_rate:.2f}% exceeds RPS tolerance "
            f"{args.rps_tolerance:.2f}%"
        )
    if completed != started:
        failures.append(f"only {completed} of {started} started requests completed")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("PASS: fixed-RPS load test met its error and arrival-rate thresholds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
