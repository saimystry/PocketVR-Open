#!/usr/bin/env python3
"""Audit exact Android runtime JARs, native dependency notices and packaged APK libraries."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import zipfile
from license_audit import ALLOWED, ROOT, allowed_expression, audit


def check(root, runtime=None, apk=None):
    errors = []
    entries = json.loads((root / "third_party/android-runtime.lock.json").read_text())
    approved = {}
    for entry in entries:
        name = entry["coordinate"]
        if name in approved or not re.fullmatch(r"[\w.-]+:[\w.-]+:[\w.-]+", name):
            errors.append("Duplicate/invalid Android dependency: " + name)
        approved[name] = entry
        if not allowed_expression(entry["license"]):
            errors.append("Disallowed Android runtime license: " + name)
        if not entry.get("license_files"):
            errors.append("Missing runtime notices: " + name)
        group, artifact, version = name.split(":")
        expected = "https://repo.maven.apache.org/maven2/" + group.replace(".", "/") + "/" + artifact + "/" + version + "/" + artifact + "-" + version
        for suffix, url, digest in ((".jar", "url", "sha256"), (".pom", "pom_url", "pom_sha256"),
                                    ("-sources.jar", "sources_url", "sources_sha256")):
            if entry[url] != expected + suffix or not re.fullmatch(r"[0-9a-f]{64}", entry[digest]):
                errors.append("Unpinned Android runtime artifact: " + name)
        for notice in entry["license_files"]:
            path = (root / notice["path"]).resolve()
            if not path.is_relative_to(root.resolve()) or not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != notice["sha256"]:
                errors.append("Runtime notice differs from review: " + notice["path"])
    tools = json.loads((root / "third_party/android-tools.lock.json").read_text())
    wrapper = root / "android/gradle/wrapper/gradle-wrapper.jar"
    if not wrapper.is_file() or hashlib.sha256(wrapper.read_bytes()).hexdigest() != tools["wrapper_sha256"]:
        errors.append("Gradle wrapper differs from the pinned official binary")
    for name, checksum in tools["wrapper_scripts_sha256"].items():
        script = root / "android" / name
        if not script.is_file() or hashlib.sha256(script.read_bytes()).hexdigest() != checksum:
            errors.append("Gradle wrapper script differs from pinned source: " + name)
    properties = (root / "android/gradle/wrapper/gradle-wrapper.properties").read_text()
    if "distributionSha256Sum=" + tools["distribution_sha256"] not in properties:
        errors.append("Gradle distribution checksum is not enforced")
    if f'version "{tools["agp"]}"' not in (root / "android/build.gradle.kts").read_text():
        errors.append("AGP version differs from reviewed tool lock")
    native_manifest = json.loads((root / "android/native-deps/vcpkg.json").read_text())
    native_lock = json.loads((root / "third_party/dependencies.lock.json").read_text())
    if native_manifest["builtin-baseline"] != native_lock["vcpkg_baseline"]:
        errors.append("Android native baseline differs from reviewed lock")
    known_native = {entry["name"] for entry in native_lock["dependencies"]}
    if any((entry if isinstance(entry, str) else entry["name"]) not in known_native for entry in native_manifest["dependencies"]):
        errors.append("Unknown direct Android native dependency")
    if runtime:
        found = set()
        for line in runtime.read_text().splitlines():
            coordinate, path = line.split("\t")
            if coordinate in found:
                errors.append("Duplicate resolved runtime dependency: " + coordinate)
            found.add(coordinate)
            entry = approved.get(coordinate)
            if not entry or hashlib.sha256(Path(path).read_bytes()).hexdigest() != entry["sha256"]:
                errors.append("Unreviewed or modified resolved runtime: " + coordinate)
        if found != set(approved):
            errors.append("Resolved Android runtime set differs from the lock")
    if apk:
        with zipfile.ZipFile(apk) as archive:
            libraries = {name for name in archive.namelist() if name.endswith(".so")}
            if libraries != {"lib/arm64-v8a/libpocketvr_native.so"}:
                errors.append("APK contains missing/unreviewed native libraries: " + str(sorted(libraries)))
            notices = {"LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md"}
            notices.update(Path(item["path"]).name for entry in entries for item in entry["license_files"])
            notices.update({"libsodium.txt", "nlohmann-json.txt", "cardboard-profile-notice.txt"})
            for name in notices:
                packaged = "assets/licenses/" + name
                source = root / name if name in {"LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md"} else root / "third_party/licenses" / name
                if packaged not in archive.namelist() or archive.read(packaged) != source.read_bytes():
                    errors.append("APK notice missing or changed: " + name)
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path)
    parser.add_argument("--installed", type=Path)
    parser.add_argument("--apk", type=Path)
    args = parser.parse_args()
    try:
        errors = audit(ROOT, args.installed) + check(ROOT, args.runtime, args.apk)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        errors = [str(error)]
    if errors:
        print("Android license/artifact audit FAILED:\n" + "\n".join(errors), file=sys.stderr)
        return 1
    print("Android runtime pins, permissive component notices and requested artifacts verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
