# SPDX-License-Identifier: Apache-2.0
# No Pester download and no real registration/firewall mutation.
param([Parameter(Mandatory)][string]$WorkRoot)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'Registration.psm1') -Force
$module = Get-Module Registration
$root = Join-Path $WorkRoot ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
function Assert($condition, $message) { if (!$condition) { throw $message } }
function MustFail([scriptblock]$test, [string]$pattern) {
    $failed = $false
    try { & $test } catch { $failed = $_.Exception.Message -like "*$pattern*" }
    Assert $failed "Expected failure containing: $pattern"
}
$runtime = Join-Path $root 'Steam VR'
New-Item -ItemType Directory -Path (Join-Path $runtime 'bin/win64') -Force | Out-Null
foreach ($exe in @('vrpathreg.exe','vrserver.exe')) { Set-Content -LiteralPath (Join-Path $runtime "bin/win64/$exe") -Value 'fixture' }
Assert ((Resolve-PocketVRRuntime $runtime) -eq $runtime) 'Explicit runtime path lost spaces'
MustFail { Resolve-PocketVRRuntime (Join-Path $root 'absent') } 'SteamVR was not found'
$package = Join-Path $root "User's Pocket VR"
New-Item -ItemType Directory -Path $package -Force | Out-Null
$savedLocal = $env:LOCALAPPDATA
try {
    $env:LOCALAPPDATA = $root
    New-Item -ItemType Directory -Path (Join-Path $root 'openvr') | Out-Null
    $foreign = Join-Path $root 'other driver'
    New-Item -ItemType Directory -Path $foreign | Out-Null
    Set-Content -LiteralPath (Join-Path $foreign 'driver.vrdrivermanifest') -Value '{"name":"pocketvr"}'
    @{runtime=@($runtime); external_drivers=@($foreign)} | ConvertTo-Json | Set-Content (Join-Path $root 'openvr/openvrpaths.vrpath')
    MustFail { Get-PocketVRDriverRegistered $package -RejectOtherPackage } 'Another PocketVR package'
    @{external_drivers=@($package,$foreign)} | ConvertTo-Json | Set-Content (Join-Path $root 'openvr/openvrpaths.vrpath')
    Assert (Get-PocketVRDriverRegistered $package) 'Exact registered package not found'
} finally { $env:LOCALAPPDATA = $savedLocal }

# Package validation rejects a tampered payload and unsafe manifest paths.
Set-Content -LiteralPath (Join-Path $package 'driver.vrdrivermanifest') -Value '{"name":"pocketvr"}'
$required = @('bin/win64/driver_pocketvr.dll','control/pocketvr_control.exe','control/pocketvr_input.exe',
    'control/pocketvr_usb.exe','control/pocketvr_benchmark.exe','tools/pocketvr_encoder_bench.exe',
    'overlay/pocketvr_overlay.exe','overlay/openvr_api.dll','overlay/app.vrmanifest','LICENSE','NOTICE','THIRD_PARTY_NOTICES.md')
foreach ($file in $required) {
    $path = Join-Path $package $file
    New-Item -ItemType Directory -Path (Split-Path $path -Parent) -Force | Out-Null
    Set-Content -LiteralPath $path -Value 'test payload'
}
$payload = Join-Path $package 'control/pocketvr_control.exe'
$entries = @(@('driver.vrdrivermanifest') + $required | ForEach-Object { @{path=$_;sha256=(Get-PocketVRFileHash (Join-Path $package $_))} })
@{schema=1;configuration='Release';files=$entries} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $package 'package-manifest.json')
Assert-PocketVRPackage $package
Set-Content -LiteralPath $payload -Value 'tampered'
MustFail { Assert-PocketVRPackage $package } 'integrity check failed'
Set-Content -LiteralPath $payload -Value 'test payload'
$entries += @{path='../outside.txt';sha256='bad'}
@{schema=1;configuration='Release';files=$entries} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $package 'package-manifest.json')
MustFail { Assert-PocketVRPackage $package } 'Unsafe package'

# Inject adapters only in this module instance, never into production helpers.
& $module {
    $script:registered = $false; $script:failOverlay = $false; $script:uncertain=$false; $script:calls = @()
    function script:Get-PocketVRDriverRegistered { param($PackagePath,[switch]$RejectOtherPackage) return $script:registered }
    function script:Invoke-PocketVRProcess {
        param($FilePath,$Arguments)
        $script:calls += ($Arguments -join '|')
        if ($script:failOverlay -and $Arguments[0] -eq '--register') {
            if ($script:uncertain) { throw 'UNCERTAIN: timed out' }
            throw 'overlay rejected'
        }
    }
}
Invoke-PocketVRRegistration $package $runtime
Assert (((& $module { $script:calls }) -join ',') -eq "adddriver|$package,--register|--quiet") 'Install ordering or argument boundaries changed'
& $module { $script:calls=@(); $script:failOverlay=$true }
MustFail { Invoke-PocketVRRegistration $package $runtime } 'overlay rejected'
Assert (((& $module { $script:calls }) -join ',') -eq "adddriver|$package,--register|--quiet,removedriver|$package") 'Failed fresh install did not roll back only new driver'
& $module { $script:calls=@(); $script:registered=$true }
MustFail { Invoke-PocketVRRegistration $package $runtime } 'overlay rejected'
Assert (((& $module { $script:calls }) -join ',') -eq '--register|--quiet') 'Failed upgrade removed an existing driver'
& $module { $script:calls=@(); $script:failOverlay=$false }
Invoke-PocketVRRegistration $package $runtime -Remove
Assert (((& $module { $script:calls }) -join ',') -eq "--unregister|--quiet,removedriver|$package") 'Uninstall ordering changed'
& $module { $script:calls=@(); $script:registered=$false }
Invoke-PocketVRRegistration $package $runtime -Remove
Assert (((& $module { $script:calls }) -join ',') -eq '--unregister|--quiet') 'Absent driver removal is not idempotent'

$statePath = Join-Path $package 'setup-state.json'
& $module { $script:failOverlay=$true }
MustFail { Invoke-PocketVRInstallState $package $runtime 'S-1-5-21-1-2-3-1001' } 'overlay rejected'
Assert (!(Test-Path -LiteralPath $statePath)) 'Failed fresh install leaked setup state'
[IO.File]::WriteAllText($statePath, 'original state bytes')
MustFail { Invoke-PocketVRInstallState $package $runtime 'S-1-5-21-1-2-3-1001' } 'overlay rejected'
Assert ([IO.File]::ReadAllText($statePath) -ceq 'original state bytes') 'Failed upgrade changed previous setup state'
& $module { $script:uncertain=$true }
MustFail { Invoke-PocketVRInstallState $package $runtime 'S-1-5-21-1-2-3-1001' } 'UNCERTAIN:'
Assert ((Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json).runtime -eq $runtime) 'Unknown outcome lost repair state'

$plan = @(Get-PocketVRFirewallPlan $runtime 'S-1-5-21-1-2-3-1001')
$other = @(Get-PocketVRFirewallPlan $runtime 'S-1-5-21-1-2-3-1002')
Assert ($plan.Count -eq 2 -and $plan[0].Name -ne $other[0].Name) 'Firewall ownership must distinguish users'
foreach ($rule in $plan) {
    Assert (($rule.Profile -split ',') -contains 'Private' -and $rule.RemoteAddress -eq 'LocalSubnet' -and
        $rule.EdgeTraversalPolicy -eq 'Block' -and $rule.Program -eq (Join-Path $runtime 'bin/win64/vrserver.exe')) 'Firewall rule too broad'
}
Assert (($plan[0].LocalPort -join ',') -eq '9943,9944' -and ($plan[1].LocalPort -join ',') -eq '9944') 'TCP media or UDP discovery port missing'
MustFail { Get-PocketVRFirewallPlan $runtime 'bad' } 'SID'
MustFail { Get-PocketVRFirewallPlan $runtime 'S-1-5-21-1-2-3-1001' 9943 9943 } 'must differ'
& $module {
    $script:rules=@{}; $script:created=@(); $script:removed=@(); $script:failNew=0
    function script:Get-NetFirewallRule { param($PolicyStore,$Name,$ErrorAction) return $script:rules[$Name] }
    function script:New-NetFirewallRule {
        [CmdletBinding()]param($Name,$DisplayName,$Group,$Description,$Direction,$Action,$Enabled,$Profile,
            $RemoteAddress,$EdgeTraversalPolicy,$Program,$Protocol,$LocalPort,$PolicyStore)
        if ($script:failNew -and $script:created.Count -eq $script:failNew) { throw 'injected firewall failure' }
        $script:created += $Name
        $script:rules[$Name]=[pscustomobject]@{Name=$Name;Group=$Group;Description=$Description;Profile=$Profile;
            Direction=$Direction;Action=$Action;Enabled=$Enabled;EdgeTraversalPolicy=$EdgeTraversalPolicy;
            Program=$Program;Protocol=$Protocol;LocalPort=$LocalPort;RemoteAddress=$RemoteAddress}
    }
    function script:Remove-NetFirewallRule { param($PolicyStore,$Name,$ErrorAction) $script:removed+=$Name; $script:rules.Remove($Name) }
    function script:Get-NetFirewallApplicationFilter { param([Parameter(ValueFromPipeline)]$InputObject) process { $InputObject } }
    function script:Get-NetFirewallPortFilter { param([Parameter(ValueFromPipeline)]$InputObject) process { $InputObject } }
    function script:Get-NetFirewallAddressFilter { param([Parameter(ValueFromPipeline)]$InputObject) process { $InputObject } }
}
Set-PocketVRFirewall $plan
Set-PocketVRFirewall $plan
Assert ((& $module { $script:created.Count }) -eq 2) 'Repeated firewall installation duplicated rules'
Set-PocketVRFirewall $other
Set-PocketVRFirewall $plan -Remove
Assert ((& $module { $script:rules.Count }) -eq 2) 'Uninstall removed another user firewall rules'
& $module { $script:rules=@{}; $script:created=@(); $script:removed=@(); $script:failNew=1 }
MustFail { Set-PocketVRFirewall $plan } 'injected firewall failure'
Assert ((& $module { $script:rules.Count }) -eq 0) 'Partial firewall creation leaked a rule'
& $module { param($name) $script:rules[$name]=[pscustomobject]@{Group='foreign';Description='foreign'} } $plan[0].Name
MustFail { Set-PocketVRFirewall $plan -Remove } 'collision'
Assert ((& $module { $script:rules.Count }) -eq 1) 'Foreign firewall rule was removed'

# Reload real process adapter: verify a failed native child is not reported as success.
Import-Module (Join-Path $PSScriptRoot 'Registration.psm1') -Force
MustFail { Invoke-PocketVRProcess "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" @('-NoProfile','-NonInteractive','-Command','exit 7') } 'failed (7)'
Write-Output 'Installer behavior checks passed: discovery/conflicts, integrity/traversal, ordered registration/rollback, idempotence, scoped firewall/rollback, native failure.'
