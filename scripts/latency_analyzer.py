#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Summarize a PocketVR latency log: where each frame's time goes, stage by stage."""
import argparse
import csv
from pathlib import Path

STAGES = [
    ("before_send_ms", "Game, composite and encode"),
    ("encode_ms", "  of which encoding"),
    ("send_ms", "Sending on the PC"),
    ("network_ms", "Network"),
    ("queue_ms", "Phone queue"),
    ("decode_ms", "Phone decode"),
    ("display_ms", "Wait for display"),
    ("total_ms", "Total"),
]


def percentile(values, share):
    ordered = sorted(values)
    return ordered[int(share * (len(ordered) - 1))]


def summarize(path):
    with Path(path).open(newline="", encoding="utf-8") as source:
        rows = [row for row in csv.DictReader(source) if row.get("frame_id") != "frame_id"]
    if not rows:
        raise SystemExit("No frames in " + str(path))
    result = {"frames": len(rows), "average_kilobits": sum(int(r["bytes"]) for r in rows) * 8 / len(rows) / 1000}
    for key, label in STAGES:
        values = [float(r[key]) for r in rows]
        result[key] = (label, percentile(values, 0.5), percentile(values, 0.95))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path, help="latency.csv from the PocketVR logs folder")
    summary = summarize(parser.parse_args().csv)
    print(f"{summary['frames']} frames, {summary['average_kilobits']:.0f} kilobits per frame on average")
    print(f"{'Stage':32}{'p50 ms':>9}{'p95 ms':>9}")
    for key, _ in STAGES:
        label, p50, p95 = summary[key]
        print(f"{label:32}{p50:9.2f}{p95:9.2f}")


if __name__ == "__main__":
    main()
