; SPDX-License-Identifier: Apache-2.0
#ifndef PackageRoot
  #error PackageRoot must point to the verified Release package
#endif
#ifndef OutputRoot
  #error OutputRoot must point outside the source tree
#endif
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif

[Setup]
AppId={{850421EA-F68A-40C3-AE6A-D1C552D89715}
AppName=PocketVR
AppVersion={#AppVersion}
AppPublisher=PocketVR contributors
DefaultDirName={localappdata}\Programs\PocketVR
DefaultGroupName=PocketVR
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputRoot}
OutputBaseFilename=PocketVR-{#AppVersion}-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile={#SourcePath}\..\control\pocketvr.ico
DisableWelcomePage=yes
DisableDirPage=yes
DisableReadyPage=yes
UninstallDisplayIcon={app}\control\pocketvr_control.exe
CloseApplications=no
RestartApplications=no
SetupLogging=yes
DisableProgramGroupPage=yes

[Files]
Source: "{#PackageRoot}\*"; DestDir: "{app}"; Excludes: "package-manifest.json"; Flags: ignoreversion recursesubdirs createallsubdirs
; Last file callback runs inside the installation transaction, before ssPostInstall.
Source: "{#PackageRoot}\package-manifest.json"; DestDir: "{app}"; Flags: ignoreversion; AfterInstall: RegisterPackage
Source: "{#PackageRoot}\setup\setup.ps1"; Flags: dontcopy
Source: "{#PackageRoot}\setup\Registration.psm1"; Flags: dontcopy

[Icons]
Name: "{group}\PocketVR"; Filename: "{app}\control\pocketvr_control.exe"
Name: "{userdesktop}\PocketVR"; Filename: "{app}\control\pocketvr_control.exe"

[Run]
Filename: "{app}\control\pocketvr_control.exe"; Description: "Open PocketVR"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\setup-state.json"

[Code]
var RegistrationReady: Boolean;

function RunHelper(Script, Action: String; Elevate: Boolean): String;
var
  Args, Output: String;
  Detail: AnsiString;
  ExitCode: Integer;
begin
  Output := ExpandConstant('{tmp}\pocketvr-setup-result.txt');
  DeleteFile(Output);
  Args := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + Script +
    '" -Action ' + Action + ' -PackagePath "' + ExpandConstant('{app}') +
    '" -ResultFile "' + Output + '"';
  if Elevate then Args := Args + ' -ElevateFirewall';
  if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
      Args, '', SW_HIDE, ewWaitUntilTerminated, ExitCode) then begin
    Result := 'Cannot start the PocketVR setup helper.';
    exit;
  end;
  if ExitCode <> 0 then begin
    if not LoadStringFromFile(Output, Detail) then Detail := 'See the setup log and retry.';
    Result := UTF8Decode(Detail);
  end else Result := '';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  ExtractTemporaryFile('Registration.psm1');
  ExtractTemporaryFile('setup.ps1');
  Result := RunHelper(ExpandConstant('{tmp}\setup.ps1'), 'Check', False);
end;

procedure RegisterPackage;
var Error: String;
begin
  Error := RunHelper(ExpandConstant('{app}\setup\setup.ps1'), 'Install', False);
  RegistrationReady := Error = '';
  if Pos('UNCERTAIN:', Error) > 0 then
    SuppressibleMsgBox('PocketVR files were retained for repair. ' + Error, mbError, MB_OK, IDOK)
  else if Error <> '' then RaiseException(Error);
end;

function WebViewInstalled: Boolean;
var Version: String;
begin
  Result := (RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Version) and (Version <> '') and (Version <> '0.0.0.0')) or
            (RegQueryStringValue(HKCU, 'Software\Microsoft\EdgeUpdate\Clients\{{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Version) and (Version <> '') and (Version <> '0.0.0.0'));
end;

// The desktop app draws its window with Microsoft's WebView2 runtime. Most PCs
// already have it; fetch Microsoft's own installer only when one does not.
procedure InstallWebView;
var ExitCode: Integer;
begin
  if WebViewInstalled then exit;
  try
    DownloadTemporaryFile('https://go.microsoft.com/fwlink/p/?LinkId=2124703', 'MicrosoftEdgeWebview2Setup.exe', '', nil);
    Exec(ExpandConstant('{tmp}\MicrosoftEdgeWebview2Setup.exe'), '/silent /install', '', SW_HIDE, ewWaitUntilTerminated, ExitCode);
  except
    Log('WebView2 runtime download failed; the app explains how to get it.');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var Error: String;
begin
  if (CurStep = ssPostInstall) and RegistrationReady then begin
    InstallWebView;
    Error := RunHelper(ExpandConstant('{app}\setup\setup.ps1'), 'FirewallInstall', True);
    if Error <> '' then
      SuppressibleMsgBox('PocketVR is installed, but Wi-Fi may be blocked by the firewall. ' +
        'USB still works. To fix Wi-Fi, open PocketVR, then Help, then Repair firewall.', mbInformation, MB_OK, IDOK);
  end;
end;

function InitializeUninstall: Boolean;
var Error: String;
begin
  Error := RunHelper(ExpandConstant('{app}\setup\setup.ps1'), 'CheckUninstall', False);
  if Error = '' then
    Error := RunHelper(ExpandConstant('{app}\setup\setup.ps1'), 'FirewallRemove', True);
  if Error = '' then
    Error := RunHelper(ExpandConstant('{app}\setup\setup.ps1'), 'Uninstall', False);
  Result := Error = '';
  if not Result then SuppressibleMsgBox('PocketVR files were retained. ' + Error, mbError, MB_OK, IDOK);
end;
