; MyPersonalDownloadManager — Inno Setup installer script.
; Build: ISCC.exe /DDistDir=<abs dist> /DOutDir=<abs out> packaging\mpdm.iss
; (DistDir = the windeployqt-deployed folder containing MPDM.exe and all DLLs.)

#ifndef DistDir
  #define DistDir "..\build-gui\dist"
#endif
#ifndef OutDir
  #define OutDir "..\build-gui"
#endif
#ifndef ExtDir
  #define ExtDir "..\..\extension"
#endif
#define AppName "MyPersonalDownloadManager"
#define AppVersion "1.0.0"
#define AppExe "MPDM.exe"
#define HostExe "mpdm_host.exe"
#define Publisher "MPDM"

[Setup]
AppId={{8F3A2C14-6B5D-4E7A-9C1F-2D4E6A8B0C13}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#Publisher}
AppPublisherURL=https://github.com/Thedeadji/My-Personal-Download-Manager
DefaultDirName={autopf}\MPDM
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
OutputDir={#OutDir}
OutputBaseFilename=MPDM-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Close a running MPDM (it can be hidden in the tray) so install/uninstall is
; not blocked by locked files; don't relaunch it afterwards.
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"

[Files]
Source: "{#DistDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion
; The MV3 browser extension (load it unpacked in Chrome/Edge from {app}\extension).
Source: "{#ExtDir}\*"; DestDir: "{app}\extension"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\Register browser host"; Filename: "{cmd}"; Parameters: "/k ""{app}\{#HostExe}"" --register --chrome --edge"; Comment: "Connect the MPDM browser extension (paste its ID when asked)"
Name: "{group}\Browser extension folder"; Filename: "{app}\extension"; Comment: "Load this folder unpacked at chrome://extensions"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
; If the user already knows the extension ID, register the native host now.
Filename: "{app}\{#HostExe}"; Parameters: "--register --chrome --edge --extension-id ""{code:GetExtId}"""; Flags: runhidden; Check: HasExtId; StatusMsg: "Registering browser host..."
; Open the extension folder so the user can load it unpacked in the browser.
Filename: "{win}\explorer.exe"; Parameters: """{app}\extension"""; Description: "Open the browser extension folder"; Flags: postinstall skipifsilent nowait
Filename: "{app}\{#AppExe}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Remove the native-host registration on uninstall.
Filename: "{app}\{#HostExe}"; Parameters: "--unregister --chrome --edge"; Flags: runhidden; RunOnceId: "unreghost"

[Code]
var
  ExtPage: TInputQueryWizardPage;

procedure InitializeWizard;
begin
  ExtPage := CreateInputQueryPage(wpSelectTasks,
    'Browser integration (optional)',
    'Connect the MPDM browser extension',
    'MPDM can take over downloads from your browser. The extension is installed to the ' +
    '"extension" folder inside the install folder.' + #13#10#13#10 +
    'If you have ALREADY loaded the extension (chrome://extensions -> Developer mode -> ' +
    'Load unpacked), paste its ID below to connect it now. Otherwise leave this blank and ' +
    'use the "Register browser host" Start-menu shortcut after you load the extension.');
  ExtPage.Add('Extension ID (optional):', False);
end;

function GetExtId(Param: string): string;
begin
  Result := Trim(ExtPage.Values[0]);
end;

function HasExtId(): Boolean;
begin
  Result := Trim(ExtPage.Values[0]) <> '';
end;
