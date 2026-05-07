#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path

import matplotlib.pyplot as plt


def load_summary(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def plot_distribution(values: list[float], title: str, xlabel: str, output_path: Path) -> None:
    plt.figure(figsize=(8, 4.5))
    plt.hist(values, bins=min(30, max(5, len(values) // 2)), color="#1f77b4", edgecolor="black", alpha=0.8)
    plt.title(title)
    plt.xlabel(xlabel)
    plt.ylabel("count")
    plt.tight_layout()
    plt.savefig(output_path, dpi=140)
    plt.close()


def plot_bar(labels: list[str], values: list[float], title: str, ylabel: str, output_path: Path) -> None:
    plt.figure(figsize=(7, 4.5))
    plt.bar(labels, values, color=["#2ca02c", "#ff7f0e"])
    plt.title(title)
    plt.ylabel(ylabel)
    plt.tight_layout()
    plt.savefig(output_path, dpi=140)
    plt.close()


def plot_percentiles(stream_values: list[float], datagram_values: list[float], output_path: Path) -> None:
    def pct(values: list[float], q: float) -> float:
        if not values:
            return 0.0
        ordered = sorted(values)
        idx = int(round((len(ordered) - 1) * q))
        return ordered[idx]

    labels = ["p50", "p95", "p99"]
    stream = [pct(stream_values, 0.50), pct(stream_values, 0.95), pct(stream_values, 0.99)]
    datagram = [pct(datagram_values, 0.50), pct(datagram_values, 0.95), pct(datagram_values, 0.99)]
    x = range(len(labels))

    plt.figure(figsize=(8, 4.5))
    plt.bar([i - 0.18 for i in x], stream, width=0.36, label="stream", color="#2ca02c")
    plt.bar([i + 0.18 for i in x], datagram, width=0.36, label="datagram", color="#ff7f0e")
    plt.xticks(list(x), labels)
    plt.ylabel("latency (us)")
    plt.title("Stream vs Datagram send latency percentiles")
    plt.legend()
    plt.tight_layout()
    plt.savefig(output_path, dpi=140)
    plt.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate PNG plots from summary.json.")
    parser.add_argument("summary", type=Path)
    parser.add_argument("--out-dir", type=Path, default=Path("plots"))
    args = parser.parse_args()

    summary = load_summary(args.summary)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    telemetry_plot = summary.get("telemetry", {}).get("plot_data", {})
    tx_plot = summary.get("tx", {}).get("plot_data", {})

    rtt_samples = telemetry_plot.get("rtt_ms_samples", [])
    freshness_samples = telemetry_plot.get("freshness_us_samples", [])
    stream_send_samples = tx_plot.get("stream_send_us_samples", [])
    datagram_send_samples = tx_plot.get("datagram_send_us_samples", [])
    throughput = tx_plot.get("mode_throughput_bps", {})

    if rtt_samples:
        plot_distribution(rtt_samples, "RTT distribution", "RTT (ms)", args.out_dir / "rtt_distribution.png")
    if freshness_samples:
        plot_distribution(freshness_samples, "Freshness distribution", "Freshness (us)", args.out_dir / "freshness_distribution.png")
    if stream_send_samples or datagram_send_samples:
        plot_percentiles(stream_send_samples, datagram_send_samples, args.out_dir / "stream_vs_datagram_latency.png")
    if throughput:
        plot_bar(
            ["stream", "datagram"],
            [float(throughput.get("stream", 0.0)), float(throughput.get("datagram", 0.0))],
            "Stream vs Datagram throughput",
            "throughput (bps)",
            args.out_dir / "stream_vs_datagram_throughput.png",
        )

    print(args.out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
