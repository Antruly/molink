; moLink Host (host-only / 被控端专用) installer (Inno Setup 6).
;
; Build with:
;   ISCC.exe /DAppVersion=0.12.28 \
;            /DPkgDir=C:\path\to\staged\host-only \
;            /DOutputDir=C:\path\to\build\package \
;            host-only.iss
;
; This is an independent product, not a variant of installer.iss:
;   * its own AppId (so it can be installed next to the full product),
;   * its own directory ({autopf}\moLinkHost),
;   * its own service name (moLinkHost) and {commonappdata}\moLinkHost tree,
;   * its own Run value and single-instance mutex (created by the shell).
; The full product's installer and artifacts are intentionally untouched.
;
; HostPayload/HostWebDir are also parsed by tools/check-inno-script.py, which
; keeps them in sync with packaging/windows/build-host-only.ps1 without Inno
; Setup (it cannot run on the Linux CI runner).
#ifndef AppVersion
  #define AppVersion "0.12.28"
#endif
#ifndef PkgDir
  #error PkgDir must point at the staged host-only package directory
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#ifndef LangFile
  #define LangFile "ChineseSimplified.isl"
#endif
#ifndef OutputName
  #define OutputName "moLinkHost-" + AppVersion + "-x64"
#endif

#define AppName "moLink Host"
; New product GUID. It must never be the full product's
; {{B2D9F7D4-6C38-4E7A-9A4B-0B5E8C3F1A72}}: sharing it would make Windows treat
; both products as one installed application.
#define AppId "{{9C2A6B51-4E7D-4F13-8A0C-5D6B2E9F7C41}"
#define HostPayload "moLink-host.exe,molink-core.exe,molink-service.exe,molink-helper.exe,molink.ico"
#define HostWebDir "web"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=moLink Contributors
AppPublisherURL=https://github.com/Antruly/molink
AppSupportURL=https://github.com/Antruly/molink
AppUpdatesURL=https://github.com/Antruly/molink/releases
DefaultDirName={autopf}\moLinkHost
DefaultGroupName=moLink Host
DisableProgramGroupPage=yes
; A system service needs an elevated install.
PrivilegesRequired=admin
; The installer and its Pascal code stay on the Windows 7 SP1 API surface.
; NOTE: the current x64 payload is an MSVC build and its core still imports
; Win8+ APIs (CreateFile2). The real Windows 7 package must come from the
; pending x86 build; see docs/host-only.md.
MinVersion=6.1sp1
; moLink-host.exe currently has only the MSVC x64 target. build-host-only.ps1
; fails explicitly for -Arch x86 instead of producing a half package.
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
SetupIconFile={#PkgDir}\molink.ico
UninstallDisplayIcon={app}\moLink-host.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "{#LangFile}"

[Tasks]
; Plain flags (checked by default): an upgrade must reinstall/start the
; service, otherwise PrepareToInstall would leave it stopped. "checkedonce"
; would uncheck it on the second install.
Name: "service"; Description: "安装为系统服务（关窗口后仍可被远程）"; GroupDescription: "后台服务"
Name: "autostart"; Description: "开机自动启动界面（仅在未安装服务时需要）"; GroupDescription: "启动方式"; Flags: unchecked
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "firewall"; Description: "在 Windows 防火墙中放行 moLink Host（所有网络配置）"; GroupDescription: "网络访问"

[Files]
; Everything from the stage, including web\ and the runtime DLLs. The stage is
; produced by build-host-only.ps1; its payload list is checked against
; HostPayload above.
Source: "{#PkgDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\moLink-host.exe"; WorkingDir: "{app}"; IconFilename: "{app}\moLink-host.exe"; AppUserModelID: "com.molink.host"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\moLink-host.exe"; WorkingDir: "{app}"; IconFilename: "{app}\moLink-host.exe"; Tasks: desktopicon; AppUserModelID: "com.molink.host"

[Registry]
; The shell also has an "开机自启" checkbox that writes this value; both paths
; use the same value name so they never create duplicates.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "moLinkHost"; ValueData: """{app}\moLink-host.exe"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
; Firewall rule name is product-specific: the full installer deletes rules
; named "moLink"/"moLink Core" and must not remove this one.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""moLink Host"""; Flags: runhidden; Tasks: firewall
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""moLink Host"" dir=in action=allow program=""{app}\molink-core.exe"" profile=any enable=yes"; Flags: runhidden; Tasks: firewall
; --install creates the service, copies molink-service.exe + molink-helper.exe
; into {commonappdata}\moLinkHost\service, writes core.path and starts it.
Filename: "{app}\molink-service.exe"; Parameters: "--install --name moLinkHost"; Flags: runhidden waituntilterminated; Tasks: service
; Nobody may be logged on when the machine boots, so the service must be
; automatic; delayed-auto avoids competing with boot drivers. (The service
; helper itself is what keeps molink-core.exe alive.)
Filename: "{sys}\sc.exe"; Parameters: "config moLinkHost start= delayed-auto"; Flags: runhidden waituntilterminated; Tasks: service
Filename: "{sys}\sc.exe"; Parameters: "start moLinkHost"; Flags: runhidden; Tasks: service
Filename: "{app}\moLink-host.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Order matters: the UI goes first (so no new core is spawned), then the
; service is stopped and removed, then any core the service supervised is
; terminated via its isolated core.pid. The explicit wait gives a core spawned
; by the UI time to notice its --parent-pid is gone before files are removed.
Filename: "{sys}\taskkill.exe"; Parameters: "/F /T /IM moLink-host.exe"; Flags: runhidden; RunOnceId: "KillHostShell"
Filename: "{sys}\sc.exe"; Parameters: "stop moLinkHost"; Flags: runhidden; RunOnceId: "StopService"
Filename: "{app}\molink-service.exe"; Parameters: "--uninstall --name moLinkHost"; Flags: runhidden waituntilterminated; RunOnceId: "UninstallService"
Filename: "{cmd}"; Parameters: "/c for /f ""tokens=1"" %p in ('type ""{commonappdata}\moLinkHost\service\core.pid""') do @taskkill /F /FI ""PID eq %p"" /FI ""IMAGENAME eq molink-core.exe"""; Flags: runhidden; RunOnceId: "KillServiceCore"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""moLink Host"""; Flags: runhidden; RunOnceId: "DeleteFirewall"
Filename: "{cmd}"; Parameters: "/c ping -n 3 127.0.0.1 >nul"; Flags: runhidden; RunOnceId: "WaitCoreExit"

[Code]
// Stop this product's own processes before an upgrade replaces files.
// Taskkill is scoped by image name (moLink-host.exe is unique to us); the core
// is killed via the service's core.pid, never by the shared image name
// molink-core.exe, because the full product may be running at the same time.
procedure KillSupervisedCore();
var
  Code: Integer;
begin
  Exec(ExpandConstant('{cmd}'),
       '/c for /f "tokens=1" %p in (''type "' +
       ExpandConstant('{commonappdata}\moLinkHost\service\core.pid') +
       '"'') do @taskkill /F /FI "PID eq %p" /FI "IMAGENAME eq molink-core.exe"',
       '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

procedure StopRunningApp();
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM moLink-host.exe', '',
       SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop moLinkHost', '',
       SW_HIDE, ewWaitUntilTerminated, Code);
  // Give the service's detach event a moment to be observed before the core is
  // killed, so the helper cannot restart it during the upgrade.
  Exec(ExpandConstant('{cmd}'), '/c ping -n 3 127.0.0.1 >nul', '',
       SW_HIDE, ewWaitUntilTerminated, Code);
  KillSupervisedCore();
  Exec(ExpandConstant('{cmd}'), '/c ping -n 3 127.0.0.1 >nul', '',
       SW_HIDE, ewWaitUntilTerminated, Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopRunningApp();
  Result := '';
end;

// The data directory is deliberately preserved: it holds the service logs,
// core.pid/core.path and (for the service) the installed helper binaries.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    SuppressibleMsgBox('即将卸载 moLink Host。' + #13#10 + #13#10 +
           '用户数据与日志将保留在：' + #13#10 +
           ExpandConstant('{commonappdata}\moLinkHost') + #13#10 +
           '如需彻底清除，请在卸载完成后手动删除该目录。',
           mbInformation, MB_OK, IDOK);
end;
