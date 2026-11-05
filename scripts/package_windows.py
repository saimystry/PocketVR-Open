#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fail closed before compiling an installer; no downloads or machine mutations."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXES = {
    'control': ['pocketvr_control', 'pocketvr_input', 'pocketvr_usb', 'pocketvr_benchmark'],
    'tools': ['pocketvr_encoder_bench', 'pocketvr_loopback', 'pocketvr_netsim', 'pocketvr_diagnostics', 'pocketvr_stereo_scene'],
    'overlay': ['pocketvr_overlay'],
}

def package(build: Path, dumpbin: str) -> None:
    cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
    if not re.search(r'^CMAKE_BUILD_TYPE:STRING=Release$', cache, re.M):
        raise ValueError('Installer requires a Release build, never Debug CRT')
    stage = build / 'package/pocketvr'
    binaries = [stage / folder / (exe + '.exe') for folder, exes in EXES.items() for exe in exes]
    binaries += [stage / 'bin/win64/driver_pocketvr.dll', stage / 'overlay/openvr_api.dll', stage / 'tools/openvr_api.dll']
    for binary in binaries:
        if not binary.is_file():
            raise ValueError(f'Missing package binary: {binary}')
        imports = subprocess.run([dumpbin, '/DEPENDENTS', str(binary)], capture_output=True, text=True, check=True).stdout
        if re.search(r'\b(?:msvcp\d+d|vcruntime\d+(?:_\d+)?d|ucrtbased)\.dll', imports, re.I):
            raise ValueError(f'Debug runtime in {binary.name}')
        for dll in re.findall(r'^\s+([\w.-]+\.dll)\s*$', imports, re.M | re.I):
            if dll.lower() == 'openvr_api.dll':
                if not (binary.parent / dll.lower()).is_file():
                    raise ValueError(f'Missing adjacent {dll}')
            elif not re.fullmatch(r'(?:api-ms-win-[\w-]+|ext-ms-[\w-]+|kernel32|user32|gdi32|imm32|xinput9_1_0|advapi32|shell32|ole32|oleaut32|shlwapi|ws2_32|iphlpapi|crypt32|bcrypt|dnsapi|winmm|comdlg32|comctl32|d3d11|d3dcompiler_47|dxgi|dwmapi|windowscodecs|mf|mfplat|mfreadwrite|msvcp140(?:_\d+|_atomic_wait)?|vcruntime140(?:_\d+)?|ucrtbase|secur32|version|propsys|runtimeobject|setupapi|cfgmgr32)\.dll', dll, re.I):
                raise ValueError(f'Unreviewed runtime import {dll} in {binary.name}')
    for name in ['LICENSE', 'NOTICE', 'THIRD_PARTY_NOTICES.md']:
        if (stage / name).read_bytes() != (ROOT / name).read_bytes():
            raise ValueError(f'Packaged notice differs: {name}')
    for source in (ROOT / 'third_party/licenses').rglob('*'):
        if source.is_file() and source.read_bytes() != (stage / source.relative_to(ROOT)).read_bytes():
            raise ValueError(f'Packaged license differs: {source.name}')
    allowed = {p.relative_to(stage).as_posix() for p in binaries}
    allowed |= {'driver.vrdrivermanifest', 'LICENSE', 'NOTICE', 'THIRD_PARTY_NOTICES.md', 'overlay/app.vrmanifest'}
    allowed |= {'adb/' + name for name in ['adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'NOTICE.txt']}
    allowed.add('phone/PocketVR.apk')
    allowed |= {'control/ui/' + p.name for p in (ROOT / 'pc/control/ui').iterdir()}
    for folder in ['docs', 'resources', 'third_party/licenses']:
        source_root = ROOT / ('pc/driver/resources' if folder == 'resources' else folder)
        allowed |= {folder + '/' + p.relative_to(source_root).as_posix() for p in source_root.rglob('*') if p.is_file()}
    allowed |= {'setup/' + name for name in ['setup.ps1', 'Registration.psm1', 'install_driver.ps1', 'uninstall_driver.ps1', 'register_driver.ps1']}
    actual = {p.relative_to(stage).as_posix() for p in stage.rglob('*') if p.is_file() and p.name != 'package-manifest.json'}
    if actual != allowed:
        raise ValueError(f'Package inventory mismatch: extra={sorted(actual-allowed)}, missing={sorted(allowed-actual)}')
    records = [{'path': name, 'sha256': hashlib.sha256((stage / name).read_bytes()).hexdigest()} for name in sorted(actual)]
    (stage / 'package-manifest.json').write_text(json.dumps({'schema': 1, 'configuration': 'Release', 'files': records}, indent=2) + '\n', encoding='utf-8')
    print(f'Verified Release package: {len(records)} files; exact notices and non-Debug runtime imports.')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--dumpbin', default='dumpbin.exe')
    args = parser.parse_args()
    package(args.build.resolve(), args.dumpbin)
