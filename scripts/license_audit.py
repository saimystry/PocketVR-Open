#!/usr/bin/env python3
"""Fail-closed audit of reviewed dependency metadata, license texts and installed packages."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
# Inno's captured four-condition permissive license is reviewed explicitly;
# this does not authorize arbitrary LicenseRef identifiers.
ALLOWED = {"Apache-2.0", "MIT", "BSD-2-Clause", "BSD-3-Clause", "ISC", "Zlib", "CC0-1.0", "BSL-1.0",
           "LicenseRef-Inno-Setup", "LicenseRef-RemObjects-Pascal-Script", "LicenseRef-LZMA-PublicDomain"}


def allowed_expression(expression: str) -> bool:
    """Parse SPDX AND/OR/parentheses; reject unknown identifiers and all exceptions."""
    tokens = re.findall(r"[A-Za-z0-9.+-]+|[()]", expression)
    if "".join(tokens) != re.sub(r"\s", "", expression) or not tokens:
        return False
    index = 0

    def atom() -> bool:
        nonlocal index
        if index >= len(tokens):
            raise ValueError("Missing license")
        token = tokens[index]
        index += 1
        if token == "(":
            result = expr()
            if index >= len(tokens) or tokens[index] != ")":
                raise ValueError("Unclosed license expression")
            index += 1
            return result
        if token not in ALLOWED:
            raise ValueError(f"Unreviewed or prohibited license: {token}")
        return True

    def expr() -> bool:
        nonlocal index
        result = atom()
        while index < len(tokens) and tokens[index] in {"AND", "OR"}:
            index += 1
            result = atom() and result
        return result

    try:
        return expr() and index == len(tokens)
    except ValueError:
        return False


def audit(root: Path, installed: Path | None = None, sdk_root: Path | None = None) -> list[str]:
    errors: list[str] = []
    if not (root / "LICENSE").is_file() or not (root / "NOTICE").is_file():
        errors.append("Project LICENSE/NOTICE missing")
    lock = json.loads((root / "third_party/dependencies.lock.json").read_text(encoding="utf-8"))
    manifest = json.loads((root / "vcpkg.json").read_text(encoding="utf-8"))
    if lock["vcpkg_baseline"] != manifest["builtin-baseline"]:
        errors.append("vcpkg baseline differs from reviewed lock")
    approved = {}
    for dep in lock["dependencies"]:
        name = dep["name"]
        if name in approved:
            errors.append(f"Duplicate dependency: {name}")
        approved[name] = dep
        if not allowed_expression(dep["license"]):
            errors.append(f"{name}: unapproved license {dep['license']}")
        if not re.fullmatch(r"[0-9a-f]{40}", dep["revision"]):
            errors.append(f"{name}: dependency must have an immutable revision")
        for item in dep.get("license_files", []):
            path = (root / item["path"]).resolve()
            if not path.is_relative_to(root.resolve()) or not path.is_file():
                errors.append(f"{name}: missing/unsafe license file {item['path']}")
            elif hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]:
                errors.append(f"{name}: license text changed: {item['path']}")
        for item in dep.get("vendored_files", []):
            path = (root / item["path"]).resolve()
            source_prefix = dep["repository"].replace("https://github.com/", "https://raw.githubusercontent.com/") + "/" + dep["revision"] + "/"
            if (not item["url"].startswith(source_prefix) or not path.is_relative_to(root.resolve())
                    or not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]):
                errors.append(f"{name}: vendored source differs from immutable lock: {item['path']}")
        if not dep.get("license_files"):
            errors.append(f"{name}: no reviewed license text")
    for item in manifest["dependencies"]:
        name = item if isinstance(item, str) else item["name"]
        if name not in approved:
            errors.append(f"Unreviewed direct dependency: {name}")
    sdk_lock = root / "third_party/sdk-files.lock.json"
    if sdk_lock.is_file():
        for item in json.loads(sdk_lock.read_text(encoding="utf-8")):
            dep = approved.get(item["sdk"])
            if not dep or item["sdk"] not in ("amf", "nvenc"):
                errors.append("Unreviewed SDK: " + item["sdk"])
                continue
            expected = dep["repository"].replace("https://github.com/", "https://raw.githubusercontent.com/") + "/" + dep["revision"] + ("/amf/public/include/" if item["sdk"] == "amf" else "/include/ffnvcodec/") + item["path"]
            if item["url"] != expected or not re.fullmatch(r"[0-9a-f]{64}", item["sha256"]):
                errors.append("SDK source differs from pinned revision: " + item["path"])
            if sdk_root:
                path = (sdk_root / item["sdk"] / item["path"]).resolve()
                if not path.is_relative_to(sdk_root.resolve()) or not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]:
                    errors.append("SDK content differs from lock: " + item["path"])
    if installed:
        if not installed.is_dir():
            errors.append("Installed package directory does not exist")
        else:
            status = installed / "vcpkg/status"
            if not status.is_file():
                errors.append("vcpkg installation status manifest missing")
                return errors
            packages = {}
            for paragraph in status.read_text(encoding="utf-8").split("\n\n"):
                fields = dict(line.split(": ", 1) for line in paragraph.splitlines() if ": " in line)
                if fields.get("Status") == "install ok installed" and "Feature" not in fields:
                    version = fields["Version"]
                    if fields.get("Port-Version", "0") != "0":
                        version += "#" + fields["Port-Version"]
                    packages[(fields["Package"], fields["Architecture"])] = version
            if not packages:
                errors.append("No installed packages found")
            for (name, architecture), version in packages.items():
                copyright_file = installed / architecture / "share" / name / "copyright"
                dep = approved.get(name)
                if not dep:
                    errors.append(f"Unreviewed installed transitive dependency: {name}")
                    continue
                expected = dep.get("installed_license_sha256")
                if not copyright_file.is_file() or not expected or hashlib.sha256(copyright_file.read_bytes()).hexdigest() != expected:
                    errors.append(f"{name}: installed copyright differs from reviewed package")
                expected_version = dep.get("version", "")
                if dep.get("port_version"):
                    expected_version += "#" + str(dep["port_version"])
                if version != expected_version:
                    errors.append(f"{name}: installed version {version} differs from reviewed {expected_version}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--installed", type=Path)
    parser.add_argument("--sdk-root", type=Path)
    args = parser.parse_args()
    try:
        errors = audit(ROOT, args.installed, args.sdk_root)
    except (OSError, ValueError, KeyError) as exc:
        errors = [str(exc)]
    if errors:
        print("License audit FAILED:\n" + "\n".join(errors), file=sys.stderr)
        return 1
    print("License audit passed: immutable dependency pins and reviewed permissive license texts.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
