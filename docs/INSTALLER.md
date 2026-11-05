# Windows installer and uninstall

PocketVR has a per-user Inno Setup installer. It is an unsigned development
release package, not evidence of completed RX 590/headset acceptance. Install
SteamVR and run it once using the same Windows account, install Microsoft's
Visual C++ 2015-2022 **x64** Redistributable 14.44.35112 or newer, then exit
SteamVR and all PocketVR windows before setup. The VC runtime is a prerequisite,
not bundled or downloaded by PocketVR. Microsoft provides it at
https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist.

## What setup changes

- Installs under `%LOCALAPPDATA%\Programs\PocketVR` by default. Another permanent
  directory can be selected. The source folder and development build stay separate.
- Detects the active OpenVR runtime, with Steam library-folder fallback. A conflicting
  external driver named `pocketvr` is rejected; unregister the old development copy first.
- Registers this exact external driver path with SteamVR's `vrpathreg.exe`.
  SteamVR loads it during normal startup; there is no Windows background service.
- Registers the adjacent dashboard manifest and enables its SteamVR autolaunch.
  Installation rejects a dashboard key owned by another executable; uninstall
  leaves that other package's dashboard intact. Startup remains passive.
- Adds current-user Start menu shortcuts for setup, the guide and uninstall.
- The selected **private local networks** task requests administrator approval
  separately. It allows inbound TCP 9943/9944 and UDP 9944 for the detected
  `vrserver.exe`, Private profile, LocalSubnet peers, blocked edge traversal.
  It adds no Public/Domain rule and changes no network category. UDP discovery
  shares the media port; Windows owns mDNS service handling.

Registration runs as the SteamVR user, even when the firewall needs another
administrator's approval. Firewall rule identifiers include the installing user's
SID and runtime path; uninstall does not remove another user's rules. Setup does
not add a login task or a Windows Run entry, install Steam, adb, an APK or codecs,
open ports on the router, or change streaming settings. Restart SteamVR afterward.

The package contains `bin/win64` and resources at its driver root, plus `control`,
`overlay`, `tools`, `setup`, `docs` and exact legal assets. Keep that layout intact.
Settings, identities, pairing records and logs live outside the installation.

## Failures, repair and uninstall

Missing prerequisites, running applications or conflicting drivers stop setup
before copying files. Every Release payload file is hashed; registration validates
the staged inventory. A rejected dashboard registration rolls back only a driver
registration created by that attempt. Inno's installation-stage callback can then
roll back the files. Existing driver registration survives a failed upgrade.
Timeout/rollback failures are explicit. In those uncertain cases, setup retains
the installed package and skips firewall creation for manual repair, rather than
deleting a potentially registered path. It does not claim the registration succeeded.

Firewall creation rolls back only rules newly created by that operation. Existing
owned rules must match the requested scope/ports; collisions or altered rules
are rejected. Firewall failure leaves the installed app available, with a visible
repair message. Retry from ordinary Windows PowerShell (the helper prompts for
firewall elevation only):

```powershell
$app = "$env:LOCALAPPDATA\Programs\PocketVR"
& "$app\setup\setup.ps1" -Action FirewallInstall -PackagePath $app -ElevateFirewall
```

For custom ports, add `-ControlPort 9943 -MediaPort 9944` using the ports saved
in the driver. Remove existing owned rules first with `-Action FirewallRemove`;
the helper refuses to silently widen or replace changed rules. Use `-WhatIf`
to validate paths and preview operations without mutation or an elevation prompt.
`-SteamVRPath` overrides runtime detection. `setup-state.json` records only the
runtime path and installing SID, no credentials.

Use Windows **Installed apps → PocketVR → Uninstall** after closing SteamVR
and PocketVR. Uninstall removes owned firewall rules, unregisters the dashboard
and exact driver path, and deletes only installer-owned files/shortcuts. Unknown
files and all external settings/pairing/logs are retained. It stops if cleanup fails,
so registration can be retried while the binaries still exist. If SteamVR was
removed/moved, restore it or provide the correct path to the helper before retrying.
Do not move a registered package or manually delete its files first.

Developer-only driver registration also has `scripts/install_driver.ps1` and
`scripts/uninstall_driver.ps1` wrappers around the existing validated helper.
These do not install an overlay, payload, shortcuts or firewall rules.

## Build and signing

Load the documented tool/MSVC environment. Configure/build the Release preset
or an external Release build directory, then run:

```powershell
pwsh -File scripts/build_installer.ps1 -BuildDirectory D:\PocketVR-builds\windows-release `
  -Iscc D:\Tools\inno-6.7.4\ISCC.exe -OutputDirectory D:\PocketVR-builds\installers
```

Inno 6.7.4 must match `third_party/installer-tools.lock.json`. The original download
was Authenticode-verified against publisher Pyrsys B.V.; its SHA-256 and exact
compiler/stub inputs are pinned. The script stages product binaries, runs the
Release CTests and installed-dependency/SDK audit, rejects Debug CRT imports and
unreviewed DLL imports, checks the exact payload allowlist/notices, and writes a
SHA-256 manifest before compiling. No compiler, tests, PDBs, caches, identities or
Android signing files are shipped. Debug builds may stage for development but
cannot become a release installer.

Signing is optional and uses an externally managed certificate. For a signed
distribution: sign product binaries first, regenerate/audit the payload manifest,
configure Inno `SignTool`/`SignedUninstaller` with the external signing command,
compile, verify the resulting signatures and record the final installer hash.
Never put certificate files/passwords in source. The current build script makes
an unsigned installer; signing and public distribution are not performed here.

## Acceptance still required

Host tests inject process/firewall adapters to check discovery, argument boundaries,
registration ordering, fresh-install rollback, preservation on upgrade, firewall
ownership/scope, partial-create rollback and idempotence. They never add a real rule.
On the home SteamVR PC, verify install, first pairing, dashboard autostart, upgrade,
controlled failure/repair and uninstall; compare OpenVR external paths and firewall
rules before/after and verify settings remain. Test paths with spaces and a standard
user who supplies a different administrator for firewall consent. A PC without SteamVR
cannot establish those acceptance results.
