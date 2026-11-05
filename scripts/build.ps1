param([ValidateSet('Debug','Release')][string]$Configuration='Debug', [switch]$Clean)
$ErrorActionPreference='Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$toolEnvironment = $env:POCKETVR_TOOL_ENV
if ($toolEnvironment) {
    if (-not (Test-Path -LiteralPath $toolEnvironment -PathType Leaf)) {throw 'POCKETVR_TOOL_ENV must identify an existing environment script.'}
    . $toolEnvironment
}
if (-not $env:POCKETVR_BUILD_ROOT) {
    $env:POCKETVR_BUILD_ROOT=Join-Path $repoRoot 'build'
}
New-Item -ItemType Directory -Path $env:POCKETVR_BUILD_ROOT -Force | Out-Null
if (-not $env:VCPKG_DOWNLOADS) {
    $env:VCPKG_DOWNLOADS=Join-Path $env:POCKETVR_BUILD_ROOT 'downloads'
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { throw 'Visual Studio C++ Build Tools were not found.' }
    Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}
$preset = 'windows-' + $Configuration.ToLowerInvariant()
Push-Location $repoRoot
try {
    if (Test-Path 'third_party/sdk-files.lock.json') {
        & python scripts/fetch_sdks.py --cache (Join-Path $env:POCKETVR_BUILD_ROOT 'sdks')
        if ($LASTEXITCODE) { throw 'SDK verification failed.' }
    }
    & cmake --preset $preset -DVCPKG_MANIFEST_INSTALL=ON
    if ($LASTEXITCODE) { throw 'CMake configure failed.' }
    if ($Clean) { & cmake --build --preset $preset --target clean; if ($LASTEXITCODE) { throw 'Clean failed.' } }
    & cmake --build --preset $preset --parallel 4
    if ($LASTEXITCODE) { throw 'Build failed.' }
    & ctest --preset $preset
    if ($LASTEXITCODE) { throw 'Tests failed.' }
    & python scripts/license_audit.py --installed (Join-Path $env:POCKETVR_BUILD_ROOT 'vcpkg-installed') --sdk-root (Join-Path $env:POCKETVR_BUILD_ROOT 'sdks')
    if ($LASTEXITCODE) { throw 'License audit failed.' }
} finally { Pop-Location }
