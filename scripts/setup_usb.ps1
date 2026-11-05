# SPDX-License-Identifier: Apache-2.0
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$AdbPath,
    [string]$Serial,
    [ValidateRange(1,65535)][int]$ControlPort = 9943,
    [ValidateRange(1,65535)][int]$MediaPort = 9944,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
if ($ControlPort -eq $MediaPort) { throw 'USB control and media need different TCP ports.' }
if (-not $AdbPath) {
    $adbCommand = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($adbCommand) { $AdbPath = $adbCommand.Source }
}
if (-not $AdbPath -or -not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw 'Supply -AdbPath or put Android platform-tools adb.exe on PATH.'
}
$AdbPath = (Resolve-Path -LiteralPath $AdbPath).Path
$listing = & $AdbPath devices
if ($LASTEXITCODE) { throw 'adb devices failed.' }
$authorized = @($listing | ForEach-Object { if ($_ -match '^(\S+)\s+device\s*$') { $Matches[1] } })
if ($Serial) {
    if ($Serial -notin $authorized) { throw 'The selected phone is not connected and adb-authorized.' }
} elseif ($authorized.Count -eq 1) { $Serial = $authorized[0] }
else { throw 'Connect and authorize one phone, or select an authorized device with -Serial.' }
$existing = @{}
$reverse = & $AdbPath -s $Serial reverse --list
if ($LASTEXITCODE) { throw 'Could not list adb reverse mappings.' }
foreach ($line in $reverse) {
    if ($line -match '^\S+\s+(tcp:\d+)\s+(tcp:\d+)\s*$') { $existing[$Matches[1]] = $Matches[2] }
}
$ports = @("tcp:$ControlPort", "tcp:$MediaPort")
# Validate both before changing either. Never replace another app's destination.
foreach ($port in $ports) {
    if ($existing.ContainsKey($port) -and $existing[$port] -ne $port) {
        throw "Existing reverse mapping for $port targets $($existing[$port]); preserve it or choose other PocketVR ports."
    }
}
$created = [System.Collections.Generic.List[string]]::new()
try {
    foreach ($port in $ports) {
        if ($Remove) {
            if ($existing.ContainsKey($port) -and $PSCmdlet.ShouldProcess($Serial, "Remove PocketVR adb reverse $port")) {
                & $AdbPath -s $Serial reverse --remove $port
                if ($LASTEXITCODE) { throw "Could not remove $port." }
            }
        } elseif ($existing.ContainsKey($port)) { Write-Output "$port already maps to this PC's $port." }
        elseif ($PSCmdlet.ShouldProcess($Serial, "adb reverse $port to this PC's $port")) {
            & $AdbPath -s $Serial reverse $port $port
            if ($LASTEXITCODE) { throw "Could not create $port." }
            $created.Add($port)
        }
    }
} catch {
    foreach ($port in $created) { & $AdbPath -s $Serial reverse --remove $port | Out-Null }
    throw
}
if (-not $WhatIfPreference) {
    Write-Output $(if ($Remove) { 'PocketVR USB mappings removed; other mappings and pairing are retained.' }
                  else { "USB ready. Enable USB (adb reverse) in PocketVR and connect to 127.0.0.1:$ControlPort. Repeat setup after unplug/replug if mappings disappear." })
}
