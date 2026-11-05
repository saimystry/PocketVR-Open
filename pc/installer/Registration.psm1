# SPDX-License-Identifier: Apache-2.0
# Windows PowerShell 5.1 compatible; no module import performs mutations.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-PocketVRFileHash {
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}

function Resolve-PocketVRRuntime {
    param([string]$SteamVRPath)
    $candidates = @()
    if ($SteamVRPath) { $candidates = @($SteamVRPath) }
    else {
        $paths = Join-Path $env:LOCALAPPDATA 'openvr/openvrpaths.vrpath'
        if (Test-Path -LiteralPath $paths) {
            $data = Get-Content -LiteralPath $paths -Raw | ConvertFrom-Json
            $candidates += @($data.runtime)
        }
        $steam = Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue
        if ($steam -and $steam.PSObject.Properties.Name -contains 'SteamPath' -and $steam.SteamPath) {
            $libraries = @($steam.SteamPath)
            $vdf = Join-Path $steam.SteamPath 'steamapps/libraryfolders.vdf'
            if (Test-Path -LiteralPath $vdf) {
                foreach ($match in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                    $libraries += $match.Groups[1].Value.Replace('\\', '\')
                }
            }
            foreach ($library in $libraries) { $candidates += Join-Path $library 'steamapps/common/SteamVR' }
        }
    }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'bin/win64/vrpathreg.exe') -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $candidate 'bin/win64/vrserver.exe') -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw 'SteamVR was not found. Install and run SteamVR once, or supply -SteamVRPath.'
}

function Assert-PocketVRStopped {
    # Do not terminate the user's applications, or replace a loaded driver.
    $names = @('vrserver', 'vrmonitor', 'vrdashboard', 'pocketvr_overlay', 'pocketvr_control',
               'pocketvr_input', 'pocketvr_usb', 'pocketvr_benchmark', 'pocketvr_encoder_bench')
    $running = @(Get-Process -Name $names -ErrorAction SilentlyContinue)
    if ($running.Count) { throw 'Exit SteamVR and all PocketVR windows, then retry setup/uninstall.' }
}

function Stop-PocketVRAdb {
    # The adb server outlives the app that started it and locks its own files, which
    # breaks uninstall and upgrade. Only PocketVR's own copy is stopped, never another adb.
    param([string]$PackagePath)
    $path = Join-Path $PackagePath 'adb\adb.exe'
    foreach ($process in @(Get-CimInstance Win32_Process -Filter "Name='adb.exe'" -ErrorAction SilentlyContinue)) {
        if ($process.ExecutablePath -ieq $path) {
            Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
            Wait-Process -Id $process.ProcessId -Timeout 5 -ErrorAction SilentlyContinue
        }
    }
}

function Get-PocketVRDriverRegistered {
    param([string]$PackagePath, [switch]$RejectOtherPackage)
    $paths = Join-Path $env:LOCALAPPDATA 'openvr/openvrpaths.vrpath'
    if (!(Test-Path -LiteralPath $paths)) { return $false }
    $data = Get-Content -LiteralPath $paths -Raw | ConvertFrom-Json
    $found = $false
    if ($data.PSObject.Properties.Name -notcontains 'external_drivers') { return $false }
    foreach ($driver in @($data.external_drivers)) {
        if (!$driver) { continue }
        if ([IO.Path]::GetFullPath($driver).TrimEnd('\','/') -ieq [IO.Path]::GetFullPath($PackagePath).TrimEnd('\','/')) {
            $found = $true; continue
        }
        $manifest = Join-Path $driver 'driver.vrdrivermanifest'
        if ($RejectOtherPackage -and (Test-Path -LiteralPath $manifest)) {
            $item = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
            if ($item.name -eq 'pocketvr') {
                # The installed layout keeps register_driver.ps1 next to this
                # module; the repository keeps it under scripts.
                $unregister = Join-Path $PSScriptRoot 'register_driver.ps1'
                if (!(Test-Path -LiteralPath $unregister)) {
                    $unregister = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\scripts\register_driver.ps1'))
                }
                throw ("Another PocketVR package is registered at $driver. Unregister it first:`r`n" +
                       "Close SteamVR, then run:`r`n" +
                       "  powershell -ExecutionPolicy Bypass -File `"$unregister`" -DriverPath `"$driver`" -Remove`r`n" +
                       "Then run this installer again.")
            }
        }
    }
    return $found
}

function Assert-PocketVRPackage {
    param([string]$PackagePath)
    $manifest = Get-Content -LiteralPath (Join-Path $PackagePath 'driver.vrdrivermanifest') -Raw | ConvertFrom-Json
    if ($manifest.name -ne 'pocketvr') { throw 'Not a PocketVR driver package.' }
    $requiredFiles = @('driver.vrdrivermanifest','bin/win64/driver_pocketvr.dll', 'control/pocketvr_control.exe',
        'control/pocketvr_input.exe', 'control/pocketvr_usb.exe', 'control/pocketvr_benchmark.exe',
        'tools/pocketvr_encoder_bench.exe', 'overlay/pocketvr_overlay.exe', 'overlay/openvr_api.dll',
        'overlay/app.vrmanifest', 'LICENSE', 'NOTICE', 'THIRD_PARTY_NOTICES.md')
    foreach ($file in $requiredFiles) {
        if (!(Test-Path -LiteralPath (Join-Path $PackagePath $file) -PathType Leaf)) { throw "Package file missing: $file" }
    }
    $hashes = Get-Content -LiteralPath (Join-Path $PackagePath 'package-manifest.json') -Raw | ConvertFrom-Json
    if ($hashes.configuration -ne 'Release' -or $hashes.schema -ne 1) { throw 'A verified Release package is required.' }
    foreach ($file in $requiredFiles) {
        if (@($hashes.files | Where-Object { $_.path -ceq $file }).Count -ne 1) { throw "Missing/duplicate integrity entry: $file" }
    }
    foreach ($entry in $hashes.files) {
        $root = [IO.Path]::GetFullPath($PackagePath).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
        $path = [IO.Path]::GetFullPath((Join-Path $root $entry.path))
        if (!$path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe package manifest path.' }
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-PocketVRFileHash $path) -ine $entry.sha256) { throw "Package integrity check failed: $($entry.path)" }
    }
}

function Invoke-PocketVRProcess {
    param([string]$FilePath, [string[]]$Arguments)
    # Arguments are paths or fixed verbs, never a shell command.
    $quoted = @($Arguments | ForEach-Object {
        if ($_ -match '["\r\n]' -or $_.EndsWith('\')) { throw 'Unsupported process argument.' }
        '"' + $_ + '"'
    }) -join ' '
    $process = Start-Process -FilePath $FilePath -ArgumentList $quoted -WindowStyle Hidden -PassThru
    try {
        if (!$process.WaitForExit(30000)) {
            $process.Kill(); throw "UNCERTAIN: Timed out: $([IO.Path]::GetFileName($FilePath)); keep files and check registration before retrying."
        }
        if ($process.ExitCode -eq 2) { throw 'UNCERTAIN: Native registration rollback failed. Keep files and check SteamVR registration manually.' }
        if ($process.ExitCode -ne 0) { throw "$([IO.Path]::GetFileName($FilePath)) failed ($($process.ExitCode))." }
    } finally { $process.Dispose() }
}

function Invoke-PocketVRRegistration {
    param([string]$PackagePath, [string]$RuntimePath, [switch]$Remove)
    $tool = Join-Path $RuntimePath 'bin/win64/vrpathreg.exe'
    $overlay = Join-Path $PackagePath 'overlay/pocketvr_overlay.exe'
    $registered = Get-PocketVRDriverRegistered $PackagePath -RejectOtherPackage:(!$Remove)
    if ($Remove) {
        # Failure retains files and lets the user retry; operations are idempotent.
        Invoke-PocketVRProcess $overlay @('--unregister','--quiet')
        if ($registered) { Invoke-PocketVRProcess $tool @('removedriver', $PackagePath) }
        return
    }
    $added = $false
    try {
        if (!$registered) {
            Invoke-PocketVRProcess $tool @('adddriver', $PackagePath)
            $added = $true
        }
        Invoke-PocketVRProcess $overlay @('--register','--quiet')
    } catch {
        $failure = $_
        if ($added) {
            try { Invoke-PocketVRProcess $tool @('removedriver', $PackagePath) }
            catch { throw "UNCERTAIN: Registration failed and rollback failed. Keep package files and unregister manually. $failure $_" }
        }
        throw $failure
    }
}

function Invoke-PocketVRInstallState {
    param([string]$PackagePath, [string]$RuntimePath, [string]$OwnerSid)
    $stateFile = Join-Path $PackagePath 'setup-state.json'
    $previous = if (Test-Path -LiteralPath $stateFile) { [IO.File]::ReadAllBytes($stateFile) } else { $null }
    try {
        @{runtime=$RuntimePath; owner_sid=$OwnerSid} | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
        Invoke-PocketVRRegistration $PackagePath $RuntimePath
    } catch {
        $failure = $_
        if ($failure.Exception.Message -notlike '*UNCERTAIN:*') {
            try {
                if ($null -ne $previous) { [IO.File]::WriteAllBytes($stateFile, [byte[]]$previous) }
                elseif (Test-Path -LiteralPath $stateFile) { Remove-Item -LiteralPath $stateFile }
            } catch { throw "UNCERTAIN: Cannot restore setup state. Keep files for repair. $failure $_" }
        }
        throw $failure
    }
}

function Get-PocketVRFirewallProfile {
    # The streaming ports must be reachable on the network category that is actually
    # in use. A machine whose LAN is classified as "Public" (the Windows default for
    # an unidentified Ethernet link) drops every inbound packet from the phone, so
    # a Private-only rule makes the phone time out with no visible cause.
    # The rule stays narrow: one program, two ports, LocalSubnet remote addresses.
    $profiles = [System.Collections.Generic.List[string]]::new()
    $profiles.Add('Private')
    try {
        $categories = @(Get-NetConnectionProfile -ErrorAction Stop |
            ForEach-Object { [string]$_.NetworkCategory } |
            Where-Object { $_ -in @('Domain', 'Private', 'Public') })
    } catch { $categories = @() }
    foreach ($category in $categories) {
        if (!$profiles.Contains($category)) { $profiles.Add($category) }
    }
    return ($profiles -join ',')
}

function ConvertTo-PocketVRProfileList {
    param([object]$Value)
    $items = @()
    foreach ($entry in @($Value)) {
        foreach ($part in ([string]$entry -split ',')) {
            $trimmed = $part.Trim()
            if ($trimmed) { $items += $trimmed }
        }
    }
    return @($items | Sort-Object -Unique)
}

function Get-PocketVRFirewallPlan {
    param([string]$RuntimePath, [string]$OwnerSid, [ValidateRange(1,65535)][int]$ControlPort = 9943,
          [ValidateRange(1,65535)][int]$MediaPort = 9944)
    if ($OwnerSid -notmatch '^S-1-5-21-\d+-\d+-\d+-\d+$') { throw 'Invalid installing-user SID.' }
    if ($ControlPort -eq $MediaPort) { throw 'Control and media ports must differ.' }
    $program = Join-Path ([IO.Path]::GetFullPath($RuntimePath)) 'bin/win64/vrserver.exe'
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $id = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($OwnerSid + '|' + $program.ToLowerInvariant())))).Replace('-','').Substring(0,24) }
    finally { $sha.Dispose() }
    $profile = Get-PocketVRFirewallProfile
    # TCP media is needed for adb; UDP media carries discovery, clock and pose.
    foreach ($spec in @(@('TCP', "$ControlPort,$MediaPort"), @('UDP', "$MediaPort"))) {
        @{
            Name = "PocketVR-$id-$($spec[0])"; DisplayName = "PocketVR $($spec[0]) ($OwnerSid)"
            Group = "PocketVR-$id"; Description = 'PocketVR owned private LAN streaming rule v1'
            Direction = 'Inbound'; Action = 'Allow'; Enabled = 'True'; Profile = $profile
            RemoteAddress = 'LocalSubnet'; EdgeTraversalPolicy = 'Block'; Program = $program
            Protocol = $spec[0]; LocalPort = @($spec[1].Split(',')); PolicyStore = 'PersistentStore'
        }
    }
}

function Set-PocketVRFirewall {
    param([hashtable[]]$Plan, [switch]$Remove)
    $created = @()
    try {
        foreach ($rule in $Plan) {
            $existing = Get-NetFirewallRule -PolicyStore PersistentStore -Name $rule.Name -ErrorAction SilentlyContinue
            if ($existing) {
                if ($existing.Group -ne $rule.Group -or $existing.Description -ne $rule.Description) {
                    throw "Firewall name collision: $($rule.Name). No foreign rule was modified."
                }
                if ($Remove) { Remove-NetFirewallRule -PolicyStore PersistentStore -Name $rule.Name -ErrorAction Stop }
                else {
                    $app = $existing | Get-NetFirewallApplicationFilter
                    $ports = $existing | Get-NetFirewallPortFilter
                    $address = $existing | Get-NetFirewallAddressFilter
                    $protocol = if ($rule.Protocol -eq 'TCP') { '6' } else { '17' }
                    if ($existing.Direction.ToString() -ne 'Inbound' -or
                        $existing.Action.ToString() -ne 'Allow' -or $existing.Enabled.ToString() -ne 'True' -or
                        $existing.EdgeTraversalPolicy.ToString() -ne 'Block' -or $app.Program -ine $rule.Program -or
                        $ports.Protocol.ToString() -notin @($rule.Protocol,$protocol) -or
                        (@($ports.LocalPort | Sort-Object) -join ',') -ne (@($rule.LocalPort | Sort-Object) -join ',') -or
                        (@($address.RemoteAddress) -join ',') -ne 'LocalSubnet') {
                        throw 'Owned firewall rule differs. Remove the old rules explicitly before changing ports.'
                    }
                    $existingProfiles = (ConvertTo-PocketVRProfileList $existing.Profile) -join ','
                    $wantedProfiles = (ConvertTo-PocketVRProfileList $rule.Profile) -join ','
                    if ($existingProfiles -ne $wantedProfiles) {
                        # The active network category changed since the last install.
                        # Recreate our own rule so the phone can reach the PC again.
                        Remove-NetFirewallRule -PolicyStore PersistentStore -Name $rule.Name -ErrorAction Stop
                        New-NetFirewallRule @rule -ErrorAction Stop | Out-Null
                        $created += $rule.Name
                    }
                }
            } elseif (!$Remove) {
                New-NetFirewallRule @rule -ErrorAction Stop | Out-Null
                $created += $rule.Name
            }
        }
    } catch {
        $failure = $_
        foreach ($name in $created) { Remove-NetFirewallRule -PolicyStore PersistentStore -Name $name -ErrorAction Stop }
        throw $failure
    }
}

Export-ModuleMember -Function *-PocketVR*
