#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise compiled setup's no-SteamVR refusal without installing the product."""
import argparse
import json
import os
import subprocess
import uuid
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--installer', type=Path, required=True)
parser.add_argument('--overlay', type=Path, required=True)
parser.add_argument('--artifacts', type=Path, required=True)
args = parser.parse_args()
probe = subprocess.run([str(args.overlay.resolve()), '--probe-runtime'], capture_output=True, text=True, check=True, timeout=15)
if json.loads(probe.stdout)['runtime_installed']:
    print('SteamVR is installed: negative preflight check skipped; no installer launched.')
    raise SystemExit(0)
root = args.artifacts.resolve() / uuid.uuid4().hex
root.mkdir(parents=True)
temp = root / 'temp'
temp.mkdir()
target = root / 'Uninstalled PocketVR'
log = root / 'setup.log'
env = dict(os.environ, TEMP=str(temp), TMP=str(temp))
result = subprocess.run([str(args.installer.resolve()), '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-',
                         f'/DIR={target}', f'/LOG={log}', '/TASKS='], cwd=root, env=env, timeout=45)
text = log.read_text(encoding='utf-8-sig', errors='replace')
assert result.returncode != 0, 'Setup unexpectedly succeeded without SteamVR'
assert 'SteamVR was not found' in text, f'Missing specific preflight error; inspect {log}'
assert not target.exists(), f'Setup left an installation directory: {target}'
print(f'Compiled installer refused missing SteamVR before copying files (exit {result.returncode}); log: {log}')
