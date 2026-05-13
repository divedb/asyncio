#!/usr/bin/env python3
"""Benchmark select vs epoll and plot results with matplotlib.

This script creates many non-blocking pipes, triggers readability events, and
measures the time to observe and consume those events using two polling APIs.

Outputs:
- benchmarks/select_vs_epoll_results.csv
- benchmarks/select_vs_epoll_plot.png
"""

from __future__ import annotations

import argparse
import csv
import os
import random
import select
import statistics
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import matplotlib.pyplot as plt


@dataclass
class BenchmarkRow:
    backend: str
    fds: int
    iterations: int
    mean_us: float
    median_us: float
    p95_us: float


def percentile(sorted_values: list[float], p: float) -> float:
    if not sorted_values:
        return 0.0
    if p <= 0:
        return sorted_values[0]
    if p >= 100:
        return sorted_values[-1]
    k = (len(sorted_values) - 1) * (p / 100.0)
    lower = int(k)
    upper = min(lower + 1, len(sorted_values) - 1)
    frac = k - lower
    return sorted_values[lower] * (1.0 - frac) + sorted_values[upper] * frac


def _close_pipes(pairs: list[tuple[int, int]]) -> None:
    for rfd, wfd in pairs:
        for fd in (rfd, wfd):
            try:
                os.close(fd)
            except OSError:
                pass


def _create_pipe_pairs(n: int) -> tuple[list[tuple[int, int]], list[int], dict[int, int]]:
    pairs: list[tuple[int, int]] = []
    read_fds: list[int] = []
    writer_by_reader: dict[int, int] = {}

    for _ in range(n):
        rfd, wfd = os.pipe()
        os.set_blocking(rfd, False)
        os.set_blocking(wfd, False)
        pairs.append((rfd, wfd))
        read_fds.append(rfd)
        writer_by_reader[rfd] = wfd

    return pairs, read_fds, writer_by_reader


def run_select(nfds: int, iterations: int, timeout_s: float) -> list[float]:
    pairs, read_fds, writer_by_reader = _create_pipe_pairs(nfds)
    latencies_us: list[float] = []

    try:
        for _ in range(iterations):
            target = random.choice(read_fds)
            os.write(writer_by_reader[target], b"x")
            start_ns = time.perf_counter_ns()
            ready, _, _ = select.select(read_fds, [], [], timeout_s)
            elapsed_us = (time.perf_counter_ns() - start_ns) / 1000.0

            if not ready:
                raise RuntimeError("select() timed out waiting for readiness event")

            for rfd in ready:
                try:
                    os.read(rfd, 1)
                except BlockingIOError:
                    pass

            latencies_us.append(elapsed_us)

    finally:
        _close_pipes(pairs)

    return latencies_us


def run_epoll(nfds: int, iterations: int, timeout_s: float) -> list[float]:
    pairs, read_fds, writer_by_reader = _create_pipe_pairs(nfds)
    latencies_us: list[float] = []
    ep = select.epoll()

    try:
        for rfd in read_fds:
            ep.register(rfd, select.EPOLLIN)

        for _ in range(iterations):
            target = random.choice(read_fds)
            os.write(writer_by_reader[target], b"x")
            start_ns = time.perf_counter_ns()
            ready = ep.poll(timeout_s, maxevents=nfds)
            elapsed_us = (time.perf_counter_ns() - start_ns) / 1000.0

            if not ready:
                raise RuntimeError("epoll.poll() timed out waiting for readiness event")

            for rfd, _event_mask in ready:
                try:
                    os.read(rfd, 1)
                except BlockingIOError:
                    pass

            latencies_us.append(elapsed_us)

    finally:
        try:
            ep.close()
        except Exception:
            pass
        _close_pipes(pairs)

    return latencies_us


def summarize(backend: str, nfds: int, samples: list[float]) -> BenchmarkRow:
    sorted_samples = sorted(samples)
    return BenchmarkRow(
        backend=backend,
        fds=nfds,
        iterations=len(samples),
        mean_us=statistics.fmean(samples),
        median_us=statistics.median(sorted_samples),
        p95_us=percentile(sorted_samples, 95),
    )


def write_csv(rows: list[BenchmarkRow], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["backend", "fds", "iterations", "mean_us", "median_us", "p95_us"])
        for row in rows:
            writer.writerow(
                [
                    row.backend,
                    row.fds,
                    row.iterations,
                    f"{row.mean_us:.3f}",
                    f"{row.median_us:.3f}",
                    f"{row.p95_us:.3f}",
                ]
            )


def read_csv(path: Path) -> list[BenchmarkRow]:
    rows: list[BenchmarkRow] = []
    with path.open("r", newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for record in reader:
            rows.append(
                BenchmarkRow(
                    backend=record["backend"],
                    fds=int(record["fds"]),
                    iterations=int(record["iterations"]),
                    mean_us=float(record["mean_us"]),
                    median_us=float(record["median_us"]),
                    p95_us=float(record["p95_us"]),
                )
            )
    return rows


def plot(rows: list[BenchmarkRow], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)

    by_backend: dict[str, list[BenchmarkRow]] = {}
    for row in rows:
        by_backend.setdefault(row.backend, []).append(row)

    plt.figure(figsize=(9, 5.5), dpi=150)
    for backend, backend_rows in sorted(by_backend.items()):
        backend_rows.sort(key=lambda r: r.fds)
        x = [r.fds for r in backend_rows]
        y = [r.median_us for r in backend_rows]
        plt.plot(x, y, marker="o", linewidth=2, label=f"{backend} median")

    plt.title("select vs epoll readiness latency")
    plt.xlabel("Number of monitored file descriptors")
    plt.ylabel("Latency per poll call (microseconds)")
    plt.grid(alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(path)
    plt.close()


def default_fd_sizes() -> list[int]:
    return [32, 64, 128, 256, 512]


def run_backend(
    name: str,
    runner: Callable[[int, int, float], list[float]],
    fd_sizes: list[int],
    iterations: int,
    timeout_s: float,
) -> list[BenchmarkRow]:
    rows: list[BenchmarkRow] = []
    for nfds in fd_sizes:
        try:
            samples = runner(nfds, iterations, timeout_s)
            rows.append(summarize(name, nfds, samples))
        except (ValueError, OSError) as exc:
            print(f"Skipping {name} nfds={nfds}: {exc}", file=sys.stderr)
    return rows


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Benchmark select vs epoll and plot the results.")
    parser.add_argument(
        "--iterations",
        type=int,
        default=5000,
        help="Number of readiness events to sample per file descriptor size.",
    )
    parser.add_argument(
        "--timeout-ms",
        type=float,
        default=100.0,
        help="Poll timeout in milliseconds for each sample.",
    )
    parser.add_argument(
        "--fds",
        type=int,
        nargs="+",
        default=default_fd_sizes(),
        help="List of FD counts to benchmark (example: --fds 32 64 128 256).",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="Random seed for deterministic run ordering.",
    )
    parser.add_argument(
        "--csv-out",
        type=Path,
        default=Path("benchmarks/select_vs_epoll_results.csv"),
        help="Output CSV path.",
    )
    parser.add_argument(
        "--plot-out",
        type=Path,
        default=Path("benchmarks/select_vs_epoll_plot.png"),
        help="Output PNG path.",
    )
    parser.add_argument(
        "--plot-only-from-csv",
        type=Path,
        default=None,
        help="Skip Python benchmark execution and only plot rows from an existing CSV.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    random.seed(args.seed)

    if args.plot_only_from_csv is not None:
        if not args.plot_only_from_csv.exists():
            print(f"CSV file does not exist: {args.plot_only_from_csv}", file=sys.stderr)
            return 4

        rows = read_csv(args.plot_only_from_csv)
        if not rows:
            print(f"CSV has no rows: {args.plot_only_from_csv}", file=sys.stderr)
            return 5

        plot(rows, args.plot_out)
        print(f"CSV:  {args.plot_only_from_csv}")
        print(f"Plot: {args.plot_out}")
        return 0

    if not hasattr(select, "epoll"):
        print("epoll is not available on this platform.", file=sys.stderr)
        return 1

    timeout_s = max(args.timeout_ms, 1.0) / 1000.0
    fd_sizes = sorted(set(x for x in args.fds if x > 0))
    if not fd_sizes:
        print("At least one positive FD count is required.", file=sys.stderr)
        return 2

    rows: list[BenchmarkRow] = []
    rows.extend(run_backend("select", run_select, fd_sizes, args.iterations, timeout_s))
    rows.extend(run_backend("epoll", run_epoll, fd_sizes, args.iterations, timeout_s))

    if not rows:
        print("No benchmark results were produced.", file=sys.stderr)
        return 3

    write_csv(rows, args.csv_out)
    plot(rows, args.plot_out)

    print("Benchmark summary (microseconds):")
    for row in sorted(rows, key=lambda r: (r.backend, r.fds)):
        print(
            f"{row.backend:>6} nfds={row.fds:>4} "
            f"mean={row.mean_us:8.3f} median={row.median_us:8.3f} p95={row.p95_us:8.3f}"
        )
    print(f"CSV:  {args.csv_out}")
    print(f"Plot: {args.plot_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
