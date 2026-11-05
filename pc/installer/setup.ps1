# SPDX-License-Identifier: Apache-2.0
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)][ValidateSet('Check','CheckUninstall','Install','Uninstall','FirewallInstall','FirewallRemove')][string]$Action,
    [Parameter(Mandatory)][string]$PackagePath,
    [string]$SteamVRPath,
    [string]$OwnerSid,
    [ValidateRange(1,65535)][int]$ControlPort = 9943,
    [ValidateRange(1,65535)][int]$MediaPort = 9944,
    [string]$ResultFile,
    [switch]$ElevateFirewall
)
$ErrorActionPreference = 'Stop'
try {
    Import-Module (Join-Path $PSScriptRoot 'Registration.psm1') -Force
    $package = [IO.Path]::GetFullPath($PackagePath).TrimEnd('\','/')
    $stateFile = Join-Path $package 'setup-state.json'
    if ($Action -in @('CheckUninstall','Uninstall','FirewallInstall','FirewallRemove') -and (Test-Path -LiteralPath $stateFile)) {
        $state = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
        if (!$SteamVRPath) { $SteamVRPath = $state.runtime }
        if (!$OwnerSid) { $OwnerSid = $state.owner_sid }
    }
    $runtime = Resolve-PocketVRRuntime $SteamVRPath
    if ($Action.StartsWith('Firewall')) {
        Import-Module "$env:SystemRoot\System32\WindowsPowerShell\v1.0\Modules\NetSecurity\NetSecurity.psd1" -ErrorAction Stop
        if (!$OwnerSid) { $OwnerSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value }
        $plan = @(Get-PocketVRFirewallPlan $runtime $OwnerSid $ControlPort $MediaPort)
        if ($PSCmdlet.ShouldProcess(($plan.Name -join ', '), $Action)) {
            $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
            $admin = ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
            if (!$admin -and $ElevateFirewall) {
                # Encode quoted data as a PowerShell literal; no shell interpolation of paths.
                $literal = { param($value) "'" + $value.Replace("'", "''") + "'" }
                $command = '& ' + (& $literal $PSCommandPath) + ' -Action ' + $Action +
                    ' -PackagePath ' + (& $literal $package) + ' -SteamVRPath ' + (& $literal $runtime) +
                    ' -OwnerSid ' + (& $literal $OwnerSid) + " -ControlPort $ControlPort -MediaPort $MediaPort"
                $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
                $child = Start-Process "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
                    -Verb RunAs -WindowStyle Hidden -ArgumentList "-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $encoded" -PassThru -Wait
                if ($child.ExitCode -ne 0) { throw 'Firewall operation failed. Retry the firewall helper as administrator.' }
            } elseif (!$admin) {
                throw 'Run this firewall-only command as administrator. Registration must run as the SteamVR user.'
            } else { Set-PocketVRFirewall $plan -Remove:($Action -eq 'FirewallRemove') }
        }
    } else {
        Assert-PocketVRStopped
        Stop-PocketVRAdb $package
        $null = Get-PocketVRDriverRegistered $package -RejectOtherPackage:($Action -notin @('Uninstall','CheckUninstall'))
        if ($Action -eq 'Install') { Assert-PocketVRPackage $package }
        if ($Action -in @('Check','CheckUninstall')) {
            # Release binaries use the installed Windows VC runtime, never Debug CRT or a bundled proprietary runtime.
            $crt = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64' -ErrorAction SilentlyContinue
            if ($Action -eq 'Check' -and (!$crt -or !$crt.Installed -or [version]$crt.Version.TrimStart('v') -lt [version]'14.44.35112.0')) {
                throw 'Install Microsoft Visual C++ 2015-2022 x64 Redistributable 14.44.35112 or newer, then retry.'
            }
            Write-Output "SteamVR: $runtime; preflight passed."
        } elseif ($PSCmdlet.ShouldProcess($package, $Action)) {
            if ($Action -eq 'Install') {
                Invoke-PocketVRInstallState $package $runtime ([Security.Principal.WindowsIdentity]::GetCurrent().User.Value)
            } else { Invoke-PocketVRRegistration $package $runtime -Remove }
        }
    }
    if ($ResultFile) { [IO.File]::WriteAllText($ResultFile, 'OK') }
} catch {
    if ($ResultFile) { [IO.File]::WriteAllText($ResultFile, $_.Exception.Message) }
    Write-Error $_ -ErrorAction Continue
    exit 1
}
