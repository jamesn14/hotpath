#!/usr/bin/env python3
"""Compare latency distributions across multiple benchmark configurations,
each backed by several repeated runs (see scripts/run_repeated.sh).

Usage:
    python3 plot_latencies.py

Expects a directory per configuration, each containing one CSV per run:
    runs/rx_bench/*.csv           -> "DPDK (rx_bench)"
    runs/socket_single/*.csv      -> "Socket, single recv()"
    runs/socket_batch/*.csv       -> "Socket, recvmmsg batch"

All runs within a directory are pooled for the histogram/CDF (a larger
sample count gives a more stable tail-percentile estimate). Per-run p99.9
values are also shown side by side to make run-to-run tail noise visible
instead of hiding it behind a single number.
"""

import csv
import glob
import os

import matplotlib
matplotlib.use("Agg")

import numpy as np
import matplotlib.pyplot as plt

CANDIDATES = [
    ("runs/rx_bench", "DPDK (rx_bench)", "tab:blue"),
    ("runs/socket_single", "Socket, single recv()", "tab:orange"),
    ("runs/socket_batch", "Socket, recvmmsg batch", "tab:green"),
]


def load_latencies(path):
    with open(path) as f:
        return np.array([float(line) for line in f if line.strip()], dtype=float)


def load_runs(dir_path):
    """Return a list of per-run sample arrays found in dir_path."""
    files = sorted(glob.glob(os.path.join(dir_path, "*.csv")))
    files = [f for f in files if os.path.basename(f) != "drop_stats.csv"]
    return [load_latencies(f) for f in files]


def load_drop_stats(dir_path):
    """Sum received/dropped across runs from run_repeated.sh's drop_stats.csv.

    Returns (total_received, total_dropped), or None if not present.
    """
    path = os.path.join(dir_path, "drop_stats.csv")
    if not os.path.exists(path):
        return None
    total_received = 0
    total_dropped = 0
    with open(path) as f:
        for row in csv.DictReader(f):
            total_received += int(row["received"])
            total_dropped += int(row["dropped"])
    if total_received + total_dropped == 0:
        return None
    return total_received, total_dropped


def format_percentiles(data, label, drop_stats=None):
    p50, p99, p999 = np.percentile(data, [50, 99, 99.9])
    line = f"{label}: p50={p50:,.0f}ns  p99={p99:,.0f}ns  p99.9={p999:,.0f}ns  (n={len(data):,})"
    if drop_stats is not None:
        received, dropped = drop_stats
        delivered_pct = 100.0 * received / (received + dropped)
        line += f"  delivered={delivered_pct:.1f}% (of {received + dropped:,} offered)"
    return line


def main():
    configs = []
    for dir_path, label, color in CANDIDATES:
        runs = load_runs(dir_path)
        if not runs:
            print(f"skipping {dir_path} (no CSVs found)")
            continue
        pooled = np.concatenate(runs)
        per_run_p999 = np.array([np.percentile(r, 99.9) for r in runs])
        drop_stats = load_drop_stats(dir_path)
        configs.append((pooled, per_run_p999, label, color, drop_stats))
        print(f"{label}: {len(runs)} runs, {len(pooled):,} pooled samples", end="")
        if drop_stats is not None:
            received, dropped = drop_stats
            print(f", delivered={100.0 * received / (received + dropped):.1f}%")
        else:
            print(" (no drop_stats.csv found)")

    if not configs:
        print("no run directories found, nothing to plot")
        return

    fig, (ax_hist, ax_cdf, ax_spread) = plt.subplots(1, 3, figsize=(20, 6))

    all_values = np.concatenate([pooled for pooled, _, _, _, _ in configs])
    bin_max = np.percentile(all_values, 99.99)
    bins = np.logspace(np.log10(all_values.min()), np.log10(bin_max), 80)

    for pooled, _, label, color, _ in configs:
        ax_hist.hist(pooled, bins=bins, alpha=0.5, label=label, color=color)
    ax_hist.set_xscale("log")
    ax_hist.set_xlabel("Latency (ns, log scale)")
    ax_hist.set_ylabel("Sample count")
    ax_hist.set_title("Latency distribution (pooled across runs)")
    ax_hist.legend()

    for pooled, _, label, color, _ in configs:
        sorted_data = np.sort(pooled)
        percentiles = np.linspace(0, 100, len(sorted_data))
        ax_cdf.plot(percentiles, sorted_data, label=label, color=color)
    ax_cdf.set_yscale("log")
    ax_cdf.set_xlabel("Percentile")
    ax_cdf.set_ylabel("Latency (ns, log scale)")
    ax_cdf.set_title("Latency by percentile (pooled across runs)")
    ax_cdf.legend(loc="upper left")
    for p in (50, 99, 99.9):
        ax_cdf.axvline(p, color="gray", linestyle="--", linewidth=0.5)

    summary = "\n".join(
        format_percentiles(pooled, label, drop_stats)
        for pooled, _, label, _, drop_stats in configs
    )
    ax_cdf.text(
        0.02, 0.78, summary,
        transform=ax_cdf.transAxes,
        verticalalignment="top",
        horizontalalignment="left",
        fontsize=8,
        family="monospace",
        bbox=dict(boxstyle="round", facecolor="white", alpha=0.8),
    )

    box_data = [per_run_p999 for _, per_run_p999, _, _, _ in configs]
    box_labels = [label for _, _, label, _, _ in configs]
    box_colors = [color for _, _, _, color, _ in configs]
    bp = ax_spread.boxplot(box_data, tick_labels=box_labels, patch_artist=True, showfliers=False)
    for patch, color in zip(bp["boxes"], box_colors):
        patch.set_facecolor(color)
        patch.set_alpha(0.5)
    for i, per_run_p999 in enumerate(box_data, start=1):
        jitter = np.random.default_rng(0).normal(0, 0.04, size=len(per_run_p999))
        ax_spread.scatter(np.full(len(per_run_p999), i) + jitter, per_run_p999,
                           color="black", s=15, zorder=3)
    ax_spread.set_yscale("log")
    ax_spread.set_ylabel("p99.9 latency (ns, log scale)")
    ax_spread.set_title(f"Per-run p99.9 spread ({len(box_data[0])} runs each, dots = individual runs)")
    ax_spread.tick_params(axis="x", rotation=15)

    range_summary = "\n".join(
        f"{label}: min={np.min(v):,.0f} median={np.median(v):,.0f} max={np.max(v):,.0f}ns"
        for v, label in zip(box_data, box_labels)
    )

    fig.tight_layout(rect=(0, 0.14, 1, 1))
    spread_pos = ax_spread.get_position()
    spread_center_x = (spread_pos.x0 + spread_pos.x1) / 2
    fig.text(
        spread_center_x, 0.01, range_summary,
        ha="center", va="bottom",
        fontsize=8,
        family="monospace",
    )
    fig.savefig("latency_comparison.png", dpi=150)
    print("Saved latency_comparison.png")


if __name__ == "__main__":
    main()
