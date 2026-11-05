#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fetch immutable, checksum-locked SDK headers into an external cache."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import urllib.request

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cache", type=Path, default=Path(os.environ.get("POCKETVR_BUILD_ROOT", "P:/pocketvr-builds")) / "sdks")
args = parser.parse_args()
args.cache.mkdir(parents=True, exist_ok=True)


def fetch(item):
    path = args.cache / item["sdk"] / item["path"]
    if not path.resolve().is_relative_to(args.cache.resolve()):
        raise ValueError("Unsafe SDK path")
    if path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == item["sha256"]:
        return
    request = urllib.request.Request(item["url"], headers={"User-Agent": "PocketVR-build"})
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read(2 * 1024 * 1024)
    if hashlib.sha256(data).hexdigest() != item["sha256"]:
        raise ValueError("SDK checksum mismatch: " + item["path"])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


with ThreadPoolExecutor(max_workers=8) as pool:
    list(pool.map(fetch, json.loads((root / "third_party/sdk-files.lock.json").read_text(encoding="utf-8"))))
print("SDK header checksums verified in " + str(args.cache))
