#!/usr/bin/env python3
"""Cross-compile shared sources for API29 arm64; does not claim Android execution."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
build_root = Path(os.environ.get("POCKETVR_BUILD_ROOT", str(root / "build")))
sdk = os.environ.get("ANDROID_HOME")
parser.add_argument("--ndk", type=Path, default=Path(os.environ["ANDROID_NDK_HOME"]) if os.environ.get("ANDROID_NDK_HOME") else Path(sdk) / "ndk/26.3.11579264" if sdk else None)
parser.add_argument("--sodium-include", type=Path, default=build_root / "vcpkg-installed/x64-windows-static-md/include")
parser.add_argument("--output", type=Path, default=build_root / "android-core-check")
args = parser.parse_args()
if args.ndk is None: parser.error("Supply --ndk or set ANDROID_NDK_HOME/ANDROID_HOME")
args.output.mkdir(parents=True, exist_ok=True)
compiler = args.ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe"


def compile_source(source):
    output = args.output / (source.stem + ".o")
    command = [str(compiler), "--target=aarch64-linux-android29", "-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fPIC", "-I", str(root / "pc/common/include"), "-isystem", str(args.sodium_include), "-c", str(source), "-o", str(output)]
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    print(source.name + " cross-compiled", flush=True)


with ThreadPoolExecutor(max_workers=4) as pool:
    list(pool.map(compile_source, (root / "pc/common/src").glob("*.cpp")))
print("Android source cross-compilation passed. Linking libsodium and device execution are separate checks.")
