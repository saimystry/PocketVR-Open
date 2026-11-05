# Register a built PocketVR driver in the current user's OpenVR paths.
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)][string]$DriverPath,
    [string]$SteamVRPath,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
$driverRoot = (Resolve-Path -LiteralPath $DriverPath).Path
$manifestPath = Join-Path $driverRoot 'driver.vrdrivermanifest'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw 'PocketVR manifest is missing.' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.name -ne 'pocketvr') { throw 'The selected directory is not a PocketVR package.' }
if (-not $Remove -and -not (Test-Path -LiteralPath (Join-Path $driverRoot 'bin/win64/driver_pocketvr.dll') -PathType Leaf)) {
    throw 'Build the Windows driver before registering this package.'
}
$modulePath = Join-Path $PSScriptRoot 'Registration.psm1'
if (!(Test-Path -LiteralPath $modulePath)) { $modulePath = Join-Path $PSScriptRoot '../pc/installer/Registration.psm1' }
Import-Module $modulePath -Force
$SteamVRPath = Resolve-PocketVRRuntime $SteamVRPath
Assert-PocketVRStopped
$null = Get-PocketVRDriverRegistered $driverRoot -RejectOtherPackage:(!$Remove)
$pathTool = Join-Path (Resolve-Path -LiteralPath $SteamVRPath).Path 'bin/win64/vrpathreg.exe'
if (-not (Test-Path -LiteralPath $pathTool -PathType Leaf)) { throw 'SteamVR vrpathreg.exe is missing.' }
$operation = if ($Remove) { 'removedriver' } else { 'adddriver' }
if ($PSCmdlet.ShouldProcess($driverRoot, "OpenVR $operation")) {
    & $pathTool $operation $driverRoot
    if ($LASTEXITCODE) { throw "SteamVR registration failed with exit code $LASTEXITCODE." }
    Write-Output "PocketVR registration updated. Restart SteamVR to apply the change."
}
