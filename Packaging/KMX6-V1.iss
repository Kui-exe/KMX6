#define MyAppVersion "1.0.0"
[Setup]
AppId={{B2F3100D-903F-49DA-809A-6DB4D6099141}
AppName=KMX6 V1
AppVersion={#MyAppVersion}
AppPublisher=KMX6 Project
DefaultDirName={localappdata}\Programs\KMX6
DefaultGroupName=KMX6
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible and not arm64
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.18362
OutputDir=..\dist
OutputBaseFilename=KMX6-V1-Setup
SetupIconFile=KMX6.ico
UninstallDisplayIcon={app}\bin\KMX6.exe
LicenseFile=..\build\release-staging\Licenses\AGPL-3.0.txt
InfoBeforeFile=Installer-Notes.txt
Compression=lzma2/normal
SolidCompression=yes
WizardStyle=modern
WizardSizePercent=110,130
CloseApplications=yes
RestartApplications=no
UninstallDisplayName=KMX6 V1
VersionInfoVersion=1.0.0.3
VersionInfoDescription=KMX6 V1 Online Installer
[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked
[Files]
Source: "..\build\release-staging\*"; DestDir: "{app}"; Excludes: "Source\*,drivers\*"; Flags: ignoreversion recursesubdirs createallsubdirs
[InstallDelete]
Type: files; Name: "{app}\Documentation\Publisher.txt"
Type: files; Name: "{app}\Source\KMX6-V1-Source.zip"
Type: files; Name: "{app}\Source\BUILDING.txt"
Type: dirifempty; Name: "{app}\Source"
Type: files; Name: "{app}\drivers\VC_redist.x64.exe"
Type: files; Name: "{app}\drivers\USBip-0.9.8.1-x64.exe"
Type: dirifempty; Name: "{app}\drivers"
Type: files; Name: "{app}\Documentation\Benchmark\benchmark.csv"
Type: files; Name: "{app}\Documentation\Benchmark\benchmark.json"
Type: files; Name: "{app}\Documentation\Benchmark\health-baseline-end.txt"
Type: files; Name: "{app}\Documentation\Benchmark\health-before.txt"
Type: files; Name: "{app}\Documentation\Benchmark\health-recovery-end.txt"
Type: files; Name: "{app}\Documentation\Benchmark\health-stress-end.txt"
Type: files; Name: "{app}\Documentation\Benchmark\live-resource-samples.csv"
Type: files; Name: "{app}\Documentation\Benchmark\plugin-feature-check.txt"
Type: files; Name: "{app}\Documentation\Benchmark\REPORT.md"
Type: files; Name: "{app}\Documentation\Benchmark\run-status.json"
Type: files; Name: "{app}\Documentation\Benchmark\self-test.txt"
Type: files; Name: "{app}\Documentation\Benchmark\source-manifest-sha256.json"
Type: files; Name: "{app}\Documentation\Benchmark\ui-scale-check.txt"
Type: files; Name: "{app}\Documentation\Benchmark\virtual-output-check.txt"
Type: files; Name: "{app}\Documentation\Benchmark\heavy\benchmark-heavy.csv"
Type: files; Name: "{app}\Documentation\Benchmark\heavy\benchmark-heavy.json"
Type: files; Name: "{app}\Documentation\Benchmark\heavy\health-baseline-end.txt"
Type: files; Name: "{app}\Documentation\Benchmark\heavy\health-before.txt"
Type: files; Name: "{app}\Documentation\Benchmark\heavy\health-recovery-end.txt"
Type: files; Name: "{app}\Documentation\Benchmark\heavy\health-stress-end.txt"
Type: files; Name: "{app}\Documentation\Benchmark\heavy\live-resource-samples.csv"
Type: files; Name: "{app}\Documentation\Benchmark\heavy\run-status.json"
Type: dirifempty; Name: "{app}\Documentation\Benchmark\heavy"
Type: dirifempty; Name: "{app}\Documentation\Benchmark"
[Icons]
Name: "{group}\KMX6"; Filename: "{app}\bin\KMX6.exe"; WorkingDir: "{app}\bin"
Name: "{group}\KMX6 Quick Start"; Filename: "{app}\Documentation\Quick-Start.html"
Name: "{group}\KMX6 Project Reference"; Filename: "{app}\Documentation\KMX6-Project-Reference.pdf"
Name: "{group}\Repair KMX6 Audio"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\Install-Dependencies.ps1"""; WorkingDir: "{app}"; IconFilename: "{app}\bin\KMX6.exe"
Name: "{group}\Uninstall KMX6"; Filename: "{uninstallexe}"
Name: "{autodesktop}\KMX6"; Filename: "{app}\bin\KMX6.exe"; WorkingDir: "{app}\bin"; Tasks: desktopicon
[Run]
Filename: "{app}\Documentation\Quick-Start.html"; Description: "Open the quick start guide"; Flags: shellexec postinstall skipifsilent unchecked
Filename: "{app}\bin\KMX6.exe"; Description: "Launch KMX6"; WorkingDir: "{app}\bin"; Flags: nowait postinstall skipifsilent; Check: CanLaunch
[Code]
var
  DependencyRestart: Boolean;
  DependenciesOK: Boolean;
  DependencySkipped: Boolean;
  DownloadsPage: TWizardPage;
procedure InitializeWizard;
var
  Notice: TNewStaticText;
begin
  DownloadsPage := CreateCustomPage(wpSelectDir, 'Required driver and runtime downloads', 'KMX6 needs these components to provide virtual audio.');
  Notice := TNewStaticText.Create(DownloadsPage);
  Notice.Parent := DownloadsPage.Surface;
  Notice.Left := 0;
  Notice.Top := 0;
  Notice.Width := DownloadsPage.SurfaceWidth;
  Notice.Height := DownloadsPage.SurfaceHeight;
  Notice.AutoSize := False;
  Notice.WordWrap := True;
  Notice.Caption := 'Setup will automatically download and install missing dependencies:' + #13#10#13#10 +
    '- USB/IP Client for Windows 0.9.8.1 (required virtual-audio driver), from the official GitHub release.' + #13#10#13#10 +
    '- Microsoft Visual C++ x64 Runtime 14.51 or newer, from Microsoft.' + #13#10#13#10 +
    'An Internet connection is required when these components are missing. Downloads total approximately 50 MB if both are needed. Existing compatible components and verified cached downloads are reused.' + #13#10#13#10 +
    'Windows may request administrator approval. Download signatures are checked before installation. A restart may be required; setup will not restart Windows automatically.' + #13#10#13#10 +
    'The application source archive is provided separately with the release and is not installed.';
end;
function CanLaunch: Boolean;
begin
  Result := DependenciesOK and not DependencyRestart;
end;
function NeedRestart: Boolean;
begin
  Result := DependencyRestart;
end;
procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
  Args: String;
begin
  if CurStep = ssPostInstall then begin
    DependenciesOK := False;
    if ExpandConstant('{param:SKIPDEPENDENCIES|0}') = '1' then begin
      DependencySkipped := True;
      Log('Dependency setup skipped explicitly for isolated installer QA.');
      Exit;
    end;
    WizardForm.StatusLabel.Caption := 'Downloading and installing missing runtime and audio-driver dependencies...';
    Args := '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + ExpandConstant('{app}\Install-Dependencies.ps1') + '"';
    if Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Args, ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code) then begin
      DependenciesOK := (Code = 0) or (Code = 3010);
      DependencyRestart := Code = 3010;
      if not DependenciesOK then begin
        WizardForm.FinishedLabel.Caption := 'KMX6 was copied, but audio dependency setup did not finish. Open Repair KMX6 Audio from the Start menu to retry. See bin\dependency-setup.txt for details.';
        if not WizardSilent then MsgBox('Audio setup did not finish (code ' + IntToStr(Code) + '). Use Repair KMX6 Audio from the Start menu to retry. Your files and documentation are installed.', mbError, MB_OK);
      end;
    end else begin
      WizardForm.FinishedLabel.Caption := 'KMX6 was copied, but dependency setup could not start. Open Repair KMX6 Audio from the Start menu.';
    end;
  end;
end;

function GetCustomSetupExitCode: Integer;
begin
  if DependencyRestart then Result := 3010
  else if (not DependenciesOK) and (not DependencySkipped) then Result := 1
  else Result := 0;
end;