#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Independently verify the native export with Python's standard ZIP reader."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="diagnostics-", dir=args.output_dir) as temporary:
        archive = Path(temporary) / "report.zip"
        env = dict(os.environ, POCKETVR_DIAGNOSTICS_TEST_EXPORT=str(archive))
        subprocess.run([args.test_binary, "--gtest_filter=Diagnostics.ZipInteroperabilityFixture"],
                       env=env, check=True, timeout=30)
        with zipfile.ZipFile(archive) as bundle:
            assert bundle.testzip() is None, "ZIP CRC mismatch"
            assert set(bundle.namelist()) == {
                "diagnostics.json", "manifest.json", "README.txt", "logs/driver.log"}
            snapshot = json.loads(bundle.read("diagnostics.json"))
            assert snapshot["settings"]["video"]["bitrate_mbps"] == 60
            assert snapshot["status"]["state"] == "tracking"
            assert "pairing" not in snapshot
            manifest = json.loads(bundle.read("manifest.json"))
            assert manifest["logs"][0]["filtered_lines"] == 1
            assert b"Encoder ready: AMD AMF" in bundle.read("logs/driver.log")
            for entry in bundle.infolist():
                data = bundle.read(entry)
                assert entry.compress_type == zipfile.ZIP_STORED
                assert b"123456" not in data and b"DO_NOT_EXPORT_IDENTITY" not in data
    print("Native diagnostics ZIP independently decoded; CRC, contents and exclusions verified.")


if __name__ == "__main__":
    main()
