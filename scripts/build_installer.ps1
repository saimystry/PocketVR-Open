# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$BuildDirectory, [Parameter(Mandatory)][string]$Iscc,
      [Parameter(Mandatory)][string]$OutputDirectory, [Parameter(Mandatory)][string]$PhoneApk)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$compiler = (Resolve-Path -LiteralPath $Iscc).Path
# The package audit inspects the release binaries with dumpbin, which lives in
# the Visual Studio developer environment. Enter it when it is not already
# active, so the audit cannot silently pass or fail on a missing tool.
if (-not (Get-Command dumpbin.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { throw 'Visual Studio C++ Build Tools were not found; dumpbin is required.' }
    Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}
$toolLock = Get-Content -LiteralPath "$repo/third_party/installer-tools.lock.json" -Raw | ConvertFrom-Json
foreach ($file in $toolLock.files.PSObject.Properties) {
    $path = Join-Path (Split-Path $compiler -Parent) $file.Name
    if (!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $file.Value) {
        throw "Unreviewed installer compiler component: $($file.Name)"
    }
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
if ($output.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Keep installer output outside source.' }
& cmake --build $build --target pocketvr_package_apps
if ($LASTEXITCODE) { throw 'Release staging failed.' }
& ctest --test-dir $build --output-on-failure
if ($LASTEXITCODE) { throw 'Release tests failed.' }
& python -B "$repo/scripts/license_audit.py" --installed "$env:POCKETVR_BUILD_ROOT/vcpkg-installed" --sdk-root "$env:POCKETVR_BUILD_ROOT/sdks"
if ($LASTEXITCODE) { throw 'License audit failed.' }
# Bundled so nobody has to install Android tools or find the phone app.
$stage = Join-Path $build 'package/pocketvr'
$tools = Join-Path $env:ANDROID_HOME 'platform-tools'
New-Item -ItemType Directory -Force -Path "$stage/adb", "$stage/phone" | Out-Null
foreach ($file in 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'NOTICE.txt') { Copy-Item -LiteralPath (Join-Path $tools $file) -Destination "$stage/adb/" -Force }
Copy-Item -LiteralPath $PhoneApk -Destination "$stage/phone/PocketVR.apk" -Force
& python -B "$repo/scripts/package_windows.py" --build $build
if ($LASTEXITCODE) { throw 'Release package audit failed.' }
& $compiler "/DPackageRoot=$build/package/pocketvr" "/DOutputRoot=$output" "$repo/pc/installer/PocketVR.iss"
if ($LASTEXITCODE) { throw 'Installer compilation failed.' }
& python -B "$repo/scripts/check_installer_preflight.py" --installer "$output/PocketVR-0.1.0-x64-setup.exe" `
    --overlay "$build/package/pocketvr/overlay/pocketvr_overlay.exe" --artifacts "$build/test-artifacts/installer-preflight"
if ($LASTEXITCODE) { throw 'Compiled installer preflight check failed.' }
Get-ChildItem -LiteralPath $output -Filter '*-setup.exe' | Get-FileHash -Algorithm SHA256
