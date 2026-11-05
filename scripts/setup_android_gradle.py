#!/usr/bin/env python3
"""Install the pinned Gradle distribution outside the source tree and verify its wrapper."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import urllib.request
import zipfile

repo = Path(__file__).resolve().parents[1]
lock = json.loads((repo / "third_party/android-tools.lock.json").read_text())
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--tools", type=Path, default=Path(os.environ.get("POCKETVR_TOOLS_ROOT", str(Path(os.environ.get("POCKETVR_BUILD_ROOT", str(repo / "build"))) / "tools"))))
args = parser.parse_args()
tools = args.tools.resolve()
tools.mkdir(parents=True, exist_ok=True)


def download(url, path, checksum=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or (checksum and hashlib.sha256(path.read_bytes()).hexdigest() != checksum):
        temporary = path.with_suffix(path.suffix + ".part")
        print("Downloading " + url, flush=True)
        with urllib.request.urlopen(url, timeout=90) as source, temporary.open("wb") as output:
            shutil.copyfileobj(source, output)
        if checksum and hashlib.sha256(temporary.read_bytes()).hexdigest() != checksum:
            temporary.unlink()
            raise RuntimeError("Checksum mismatch for " + url)
        temporary.replace(path)


gradle_dir = tools / ("gradle-" + lock["gradle"])
if not (gradle_dir / "bin/gradle.bat").exists():
    archive = tools / ("gradle-" + lock["gradle"] + "-bin.zip")
    download(lock["distribution_url"], archive, lock["distribution_sha256"])
    with zipfile.ZipFile(archive) as package:
        for member in package.infolist():
            if not (tools / member.filename).resolve().is_relative_to(gradle_dir):
                raise RuntimeError("Unexpected Gradle archive path")
        package.extractall(tools)
    archive.unlink()
base = "https://raw.githubusercontent.com/gradle/gradle/" + lock["gradle_revision"] + "/"
for relative in ("gradlew", "gradlew.bat", "gradle/wrapper/gradle-wrapper.jar"):
    download(base + relative, repo / "android" / relative,
             lock["wrapper_sha256"] if relative.endswith(".jar") else lock["wrapper_scripts_sha256"][relative])
properties = repo / "android/gradle/wrapper/gradle-wrapper.properties"
properties.write_text("distributionBase=GRADLE_USER_HOME\ndistributionPath=wrapper/dists\n"
                      "distributionUrl=" + lock["distribution_url"].replace(":", "\\:") + "\n"
                      "distributionSha256Sum=" + lock["distribution_sha256"] + "\n"
                      "networkTimeout=90000\nvalidateDistributionUrl=true\n"
                      "zipStoreBase=GRADLE_USER_HOME\nzipStorePath=wrapper/dists\n")
print("Gradle and wrapper verified. Distribution archive removed.", flush=True)
