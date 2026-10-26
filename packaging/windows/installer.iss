; moLink Windows installer (Inno Setup 6).
;
; Build with:
;   ISCC.exe /DAppVersion=0.9.25 \
;            /DPkgDir=C:\path\to\staged\pkg \
;            /DOutputDir=C:\path\to\build\package \
;            installer.iss
;
; The staged directory comes from stage.sh. The installer runs elevated so it
; can add a Windows Firewall rule for molink-core.exe. The core is loopback-only
; by default; when LAN access is enabled in Security it binds 0.0.0.0, and
; Windows otherwise blocks inbound connections. The taskbar icon and the desktop
; shortcut both use the icon embedded in moLink.exe. The portable build cannot
; add a rule; Windows prompts on first LAN listen instead.

#ifndef AppVersion
  #define AppVersion "0.9.25"
#endif
#ifndef PkgDir
  #error PkgDir must point at the staged package directory
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#ifndef LangFile
  #define LangFile "ChineseSimplified.isl"
#endif

#define AppName "moLink"
#define AppExeName "moLink.exe"
#define AppId "{{B2D9F7D4-6C38-4E7A-9A4B-0B5E8C3F1A72}"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=moLink Contributors
AppPublisherURL=https://github.com/Antruly/molink
AppSupportURL=https://github.com/Antruly/molink
AppUpdatesURL=https://github.com/Antruly/molink/releases
DefaultDirName={autopf}\moLink
DefaultGroupName=moLink
DisableProgramGroupPage=yes
PrivilegesRequired=admin
MinVersion=6.1sp1
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
OutputDir={#OutputDir}
OutputBaseFilename=moLink-{#AppVersion}-setup-x64
SetupIconFile={#PkgDir}\molink.ico
UninstallDisplayIcon={app}\{#AppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "{#LangFile}"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "firewall"; Description: "Allow moLink through Windows Firewall (private and domain networks)"; GroupDescription: "Network access"
Name: "service"; Description: "安装 moLink 后台服务（支持锁屏/登录页远程）"; GroupDescription: "后台服务"; Flags: checkedonce

[Files]
Source: "{#PkgDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\{#AppExeName}"; AppUserModelID: "com.molink.desktop"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\{#AppExeName}"; Tasks: desktopicon; AppUserModelID: "com.molink.desktop"

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""moLink"""; Flags: runhidden; Tasks: firewall
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""moLink"" dir=in action=allow program=""{app}\molink-core.exe"" profile=private,domain enable=yes"; Flags: runhidden; Tasks: firewall
Filename: "{app}\molink-service.exe"; Parameters: "--install"; Flags: runhidden waituntilterminated; Tasks: service
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\molink-service.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""moLink"""; Flags: runhidden
