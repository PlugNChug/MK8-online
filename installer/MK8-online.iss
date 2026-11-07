; MK8-online setup (Inno Setup 6). Built by scripts/release.py; not meant to be compiled by hand.
; Per-user install, no administrator rights. A wizard page chooses the data folder (unpacked versions,
; import, tools, builds, saves; default %LOCALAPPDATA%\MK8-online), written to {app}\mk8-online.json for the
; launcher. Updates keep the previous choice; uninstalling leaves the data folder in place.
; Silent installs take it from /DATADIR="D:\MK8-online".

#ifndef AppVersion
  #error AppVersion must be defined (scripts/release.py passes /DAppVersion=...)
#endif
#ifndef NumericVersion
  #define NumericVersion "0.0.0.0"
#endif
#ifndef SourceExe
  #define SourceExe "..\target\portable\Launcher.exe"
#endif
#ifndef OutputDir
  #define OutputDir "..\target\dist"
#endif

[Setup]
AppId={{6F2C9E1A-4B8D-4E7A-9C51-3A7D2E8F0B64}
AppName=MK8-online
AppVersion={#AppVersion}
AppVerName=MK8-online {#AppVersion}
AppPublisher=MK8-online contributors
VersionInfoVersion={#NumericVersion}
DefaultDirName={localappdata}\Programs\MK8-online
DefaultGroupName=MK8-online
DisableProgramGroupPage=yes
DisableDirPage=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=MK8-online-{#AppVersion}-setup
SetupIconFile=..\apps\launcher\icons\icon.ico
UninstallDisplayIcon={app}\Launcher.exe
LicenseFile=..\LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; DestName: "Launcher.exe"; Flags: ignoreversion

[Icons]
Name: "{group}\MK8-online"; Filename: "{app}\Launcher.exe"
Name: "{group}\{cm:UninstallProgram,MK8-online}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\MK8-online"; Filename: "{app}\Launcher.exe"; Tasks: desktopicon

[Run]
; Also after a silent update started by the launcher: it closes itself and this starts the new version.
Filename: "{app}\Launcher.exe"; Description: "{cm:LaunchProgram,MK8-online}"; Flags: nowait postinstall

[UninstallRun]
; Only the launcher of this installation (matched by path); other programs may also be called Launcher.exe.
Filename: "powershell.exe"; Parameters: "-NoProfile -NonInteractive -Command ""Get-Process Launcher -ErrorAction SilentlyContinue | Where-Object {{ $_.Path -eq '{app}\Launcher.exe' } | Stop-Process -Force"""; Flags: runhidden waituntilterminated; RunOnceId: "StopLauncher"

[UninstallDelete]
Type: files; Name: "{app}\mk8-online.json"

[Code]
const
  NeededGiB = 30;
var
  DataPage: TInputDirWizardPage;

function DefaultDataDir(): String;
begin
  Result := ExpandConstant('{param:DATADIR|}');
  if Result = '' then
    Result := GetPreviousData('DataDir', ExpandConstant('{localappdata}\MK8-online'));
end;

procedure InitializeWizard();
begin
  DataPage := CreateInputDirPage(wpSelectDir,
    'Pasta de dados do jogo',
    'Onde o MK8-online guarda a importação, as ferramentas, as builds e os saves?',
    'A primeira preparação usa cerca de ' + IntToStr(NeededGiB) + ' GiB. Escolha um disco com espaço livre. ' +
    'Essa pasta é mantida em atualizações e ao desinstalar.',
    False, 'MK8-online');
  DataPage.Add('');
  DataPage.Values[0] := DefaultDataDir();
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'DataDir', DataPage.Values[0]);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  FreeBytes, TotalBytes: Int64;
  Drive: String;
begin
  Result := True;
  if CurPageID = DataPage.ID then begin
    Drive := ExtractFileDrive(DataPage.Values[0]);
    if (Drive <> '') and GetSpaceOnDisk64(AddBackslash(Drive), FreeBytes, TotalBytes) and
       (FreeBytes < Int64(NeededGiB) * 1024 * 1024 * 1024) then
      Result := MsgBox('O disco ' + Drive + ' tem menos de ' + IntToStr(NeededGiB) +
        ' GiB livres. A preparação pode falhar por falta de espaço. Continuar mesmo assim?',
        mbConfirmation, MB_YESNO) = IDYES;
  end;
end;

function JsonPath(Value: String): String;
begin
  Result := Value;
  StringChangeEx(Result, '\', '\\', True);
  StringChangeEx(Result, '"', '\"', True);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    SaveStringToFile(ExpandConstant('{app}\mk8-online.json'),
      '{"home": "' + JsonPath(DataPage.Values[0]) + '"}' + #13#10, False);
end;
