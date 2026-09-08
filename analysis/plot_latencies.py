#!/usr/bin/env python3
"""Compare latency distributions across multiple benchmark runs.

Usage:
    python3 plot_latencies.py

Reads whichever of the following CSVs are present in the current directory
and plots them together:
    rx_bench_latencies.csv              -> "DPDK (rx_bench)"
    socket_bench_single_latencies.csv   -> "Socket, single recv()"
    socket_bench_batch_latencies.csv    -> "Socket, recvmmsg batch"
"""

import os

import matplotlib
matplotlib.use("Agg")

import numpy as np
import matplotlib.pyplot as plt

CANDIDATES = [
    ("rx_bench_latencies.csv", "DPDK (rx_bench)", "tab:blue"),
    ("socket_bench_single_latencies.csv", "Socket, single recv()", "tab:orange"),
    ("socket_bench_batch_latencies.csv", "Socket, recvmmsg batch", "tab:green"),
]


def load_latencies(path):
    with open(path) as f:
        return np.array([float(line) for line in f if line.strip()], dtype=float)


def format_percentiles(data, label):
    p50, p99, p999 = np.percentile(data, [50, 99, 99.9])
    return f"{label}: p50={p50:,.0f}ns  p99={p99:,.0f}ns  p99.9={p999:,.0f}ns"


def main():
    datasets = []
    for path, label, color in CANDIDATES:
        if os.path.exists(path):
            datasets.append((load_latencies(path), label, color))
        else:
            print(f"skipping {path} (not found)")

    if not datasets:
        print("no CSV files found, nothing to plot")
        return

    fig, (ax_hist, ax_cdf) = plt.subplots(1, 2, figsize=(14, 6))

    all_values = np.concatenate([data for data, _, _ in datasets])
    bin_max = np.percentile(all_values, 99.99)
    bins = np.logspace(np.log10(all_values.min()), np.log10(bin_max), 80)

    for data, label, color in datasets:
        ax_hist.hist(data, bins=bins, alpha=0.5, label=label, color=color)
    ax_hist.set_xscale("log")
    ax_hist.set_xlabel("Latency (ns, log scale)")
    ax_hist.set_ylabel("Sample count")
    ax_hist.set_title("Latency distribution")
    ax_hist.legend()

    for data, label, color in datasets:
        sorted_data = np.sort(data)
        percentiles = np.linspace(0, 100, len(sorted_data))
        ax_cdf.plot(percentiles, sorted_data, label=label, color=color)
    ax_cdf.set_yscale("log")
    ax_cdf.set_xlabel("Percentile")
    ax_cdf.set_ylabel("Latency (ns, log scale)")
    ax_cdf.set_title("Latency by percentile")
    ax_cdf.legend(loc="upper left")
    for p in (50, 99, 99.9):
        ax_cdf.axvline(p, color="gray", linestyle="--", linewidth=0.5)

    summary = "\n".join(format_percentiles(data, label) for data, label, _ in datasets)
    ax_cdf.text(
        0.02, 0.78, summary,
        transform=ax_cdf.transAxes,
        verticalalignment="top",
        horizontalalignment="left",
        fontsize=9,
        family="monospace",
        bbox=dict(boxstyle="round", facecolor="white", alpha=0.8),
    )

    fig.tight_layout()
    fig.savefig("latency_comparison.png", dpi=150)
    print("Saved latency_comparison.png")


if __name__ == "__main__":
    main()