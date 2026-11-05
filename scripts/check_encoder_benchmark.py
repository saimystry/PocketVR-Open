#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate benchmark reporting with two tiny WARP/software frames, not GPU performance."""
import argparse
import json
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()
    common = [args.binary, "--software", "--warp", "--width", "256", "--height", "128",
              "--frames", "2", "--fps", "60", "--bitrate", "1000000", "--preset", "fast"]
    refused = subprocess.run(common + ["--require-hardware"], capture_output=True, text=True, timeout=30)
    assert refused.returncode != 0, "Hardware-only mode accepted a software/WARP request"
    result = subprocess.run(common + ["--quality", "--machine-readable"],
                            capture_output=True, text=True, check=True, timeout=30)
    marker = "POCKETVR_BENCHMARK_JSON:"
    rows = [line[len(marker):] for line in result.stdout.splitlines() if line.startswith(marker)]
    assert len(rows) == 1, f"Missing or ambiguous machine-readable report: stdout={result.stdout!r}, stderr={result.stderr!r}"
    report = json.loads(rows[0])
    assert report["report_version"] == 1 and report["preset"] == "fast"
    assert report["hardware"] is False and report["warp"] is True
    assert report["width"] == 256 and report["height"] == 128 and report["frames"] == 2
    assert report["target_bitrate_mbps"] == 1 and report["actual_bitrate_mbps"] > 0
    assert 0 <= report["encode_ms_p50"] <= report["encode_ms_p95"]
    assert 0 <= report["sampled_luma_psnr_db_p50_capped_100"] <= 100
    assert report["software_decode_ms_p95"] >= 0
    assert report["phone_decode_measured"] is False and report["headset_latency_measured"] is False
    print("Tiny software fixture verified CLI report/quality fields and hardware-only rejection; no GPU performance claim.")


if __name__ == "__main__":
    main()
