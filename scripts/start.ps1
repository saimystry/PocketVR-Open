# SPDX-License-Identifier: Apache-2.0
param([string]$BuildRoot = $env:POCKETVR_BUILD_ROOT)
if (-not $BuildRoot) { $BuildRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'build' }
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$ErrorActionPreference = 'Stop'
$package = Join-Path $BuildRoot 'windows-debug\package\pocketvr'
$control = Join-Path $BuildRoot 'windows-debug\pc\control\pocketvr_control.exe'
$diagnostics = Join-Path $BuildRoot 'windows-debug\pc\tools\pocketvr_diagnostics.exe'
try {
    $paths = Get-Content -LiteralPath (Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath') -Raw | ConvertFrom-Json
    if (@($paths.external_drivers) -notcontains $package) {
        throw 'The latest PocketVR package is not registered. See RUN-AND-TEST.md.'
    }
    if (!(Test-Path -LiteralPath $control) -or !(Test-Path -LiteralPath $diagnostics)) {
        throw 'Build the latest Windows apps first. See RUN-AND-TEST.md.'
    }
    if (!(Get-Process vrserver -ErrorAction SilentlyContinue)) {
        $runtime = @($paths.runtime) | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'bin\win64\vrmonitor.exe') } | Select-Object -First 1
        if (!$runtime) { throw 'SteamVR runtime was not found. Run SteamVR once and retry.' }
        Write-Host 'Starting SteamVR and the PocketVR connection server...'
        Start-Process -FilePath (Join-Path $runtime 'bin\win64\vrmonitor.exe') -WindowStyle Hidden
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    $ready = $false
    do {
        $raw = & $diagnostics
        try {
            $status = $raw | ConvertFrom-Json
            $ready = $status.ok -and @($status.endpoints).Count -gt 0
        } catch { $ready = $false }
        if (!$ready) { Start-Sleep -Milliseconds 500 }
    } while (!$ready -and [DateTime]::UtcNow -lt $deadline)
    if (!$ready) { throw 'SteamVR started but the PocketVR server did not become ready. Check SteamVR add-on errors and driver logs.' }
    $usbHelper = Join-Path $BuildRoot 'windows-debug\pc\control\pocketvr_usb.exe'
    if (Test-Path -LiteralPath $usbHelper) {
        try {
            $usbStatus = (& $usbHelper --maintain --once | Out-String) | ConvertFrom-Json
            Write-Host $usbStatus.state
            # A single-instance helper repairs the selected phone's mappings
            # after unplug/replug or an adb restart while SteamVR is available.
            Start-Process -FilePath $usbHelper -ArgumentList '--maintain' -WindowStyle Hidden
        } catch {
            Write-Host ('USB setup needs attention: ' + $_.Exception.Message)
        }
    }
    $running = @(Get-Process pocketvr_control -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $control })
    if (!$running.Count) { Start-Process -FilePath $control -WindowStyle Normal }
    Write-Host 'PocketVR server is ready. Use USB 127.0.0.1:9943 or a LAN address shown in setup. First use of an address requires pairing.'
} catch {
    Write-Host ('PocketVR startup failed: ' + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
