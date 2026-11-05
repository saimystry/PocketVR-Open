# SPDX-License-Identifier: Apache-2.0
[CmdletBinding(SupportsShouldProcess)]
param([Parameter(Mandatory)][string]$DriverPath, [string]$SteamVRPath)
& (Join-Path $PSScriptRoot 'register_driver.ps1') -DriverPath $DriverPath -SteamVRPath $SteamVRPath -Remove -WhatIf:$WhatIfPreference
