; moLink Windows installer (Inno Setup 6).
;
; Build with:
;   ISCC.exe /DAppVersion=0.10.2 \
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
  #define AppVersion "0.10.2"
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
; The native Direct2D shell is packaged from the same staged tree with
; /DAppExeName=moLink-native.exe and its own output name.
#ifndef AppExeName
  #define AppExeName "moLink.exe"
#endif
; A nested {#...} inside a #define is expanded only once, so
; "moLink-{#AppVersion}-..." would keep the literal braces in the filename
; when OutputName itself is used with {#OutputName}. Concatenate at define
; time instead; the workflow also passes an explicit OutputName.
#ifndef OutputName
  #define OutputName "moLink-" + AppVersion + "-setup-x64"
#endif
#define AppId "{{B2D9F7D4-6C38-4E7A-9A4B-0B5E8C3F1A72}"

; Native / Win7 builds pass:
;   /DAppExeName=moLink-native.exe /DArchAllowed=x86 /DNoInstall64Mode=1
;   /DOutputName=moLink-<ver>-native-setup-x86 /DNoService=1
#ifndef Install64Mode
  #define Install64Mode "x64"
#endif

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
#ifndef ArchAllowed
  #define ArchAllowed "x64"
#endif
ArchitecturesAllowed={#ArchAllowed}
#ifndef NoInstall64Mode
ArchitecturesInstallIn64BitMode={#Install64Mode}
#endif
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
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
Name: "firewall"; Description: "Allow moLink through Windows Firewall (all network profiles)"; GroupDescription: "Network access"
#ifndef NoService
Name: "service"; Description: "安装 moLink 后台服务（支持锁屏/登录页远程）"; GroupDescription: "后台服务"; Flags: checkedonce
#endif

[Files]
Source: "{#PkgDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\{#AppExeName}"; AppUserModelID: "com.molink.desktop"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\{#AppExeName}"; Tasks: desktopicon; AppUserModelID: "com.molink.desktop"

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""moLink"""; Flags: runhidden; Tasks: firewall
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""moLink"" dir=in action=allow program=""{app}\molink-core.exe"" profile=any enable=yes"; Flags: runhidden; Tasks: firewall
#ifndef NoService
Filename: "{app}\molink-service.exe"; Parameters: "--install"; Flags: runhidden waituntilterminated; Tasks: service
#endif
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /T /IM moLink.exe"; Flags: runhidden; RunOnceId: "KillShell"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /T /IM moLink-native.exe"; Flags: runhidden; RunOnceId: "KillNativeShell"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /T /IM molink-core.exe"; Flags: runhidden; RunOnceId: "KillCore"
#ifndef NoService
Filename: "{app}\molink-service.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated
#endif
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""moLink"""; Flags: runhidden

[Code]
// The shell hides to the tray on WM_CLOSE, so Windows' Restart Manager cannot
// close it and Setup eventually asks the user to close a program that never
// goes away. Kill the tray app and the core outright before replacing files.
// The native build's shell is moLink-native.exe: without killing it too, the
// upgrade of a running native install fails with "DeleteFile 失败；错误代码 5"
// once Setup reaches that exe (Restart Manager cannot help across sessions).
procedure StopRunningApp();
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop moLinkSession', '', SW_HIDE,
       ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM moLink.exe', '',
       SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM moLink-native.exe', '',
       SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM molink-core.exe', '',
       SW_HIDE, ewWaitUntilTerminated, Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopRunningApp();
  Result := '';
end;
