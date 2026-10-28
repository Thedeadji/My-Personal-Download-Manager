; MyPersonalDownloadManager — Inno Setup installer script.
; Build: ISCC.exe /DDistDir=<abs dist> /DOutDir=<abs out> packaging\mpdm.iss
; (DistDir = the windeployqt-deployed folder containing MPDM.exe and all DLLs.)

#ifndef DistDir
  #define DistDir "..\build-gui\dist"
#endif
#ifndef OutDir
  #define OutDir "..\build-gui"
#endif
#define AppName "MyPersonalDownloadManager"
#define AppVersion "1.0.0"
#define AppExe "MPDM.exe"
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

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent
