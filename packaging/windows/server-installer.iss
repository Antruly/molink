; moLink Server Windows installer (Inno Setup 6).
;
; Build with:
;   ISCC.exe /DAppVersion=0.2.0 /DPkgDir=C:\path\to\staged\pkg ^
;            /DOutputDir=C:\path\to\build\package installer.iss

#ifndef AppVersion
  #define AppVersion "0.2.0"
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

[Setup]
AppId={{8C6C0D2E-4B7B-4F1E-9C3A-2F7A9E1D5B60}
AppName=moLink Server
AppVersion={#AppVersion}
AppPublisher=moLink
AppPublisherURL=https://github.com/Antruly/molink
DefaultDirName={autopf}\moLink Server
DefaultGroupName=moLink Server
OutputDir={#OutputDir}
OutputBaseFilename=moLink-Server-{#AppVersion}-setup-x64
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
UninstallDisplayName=moLink Server
UninstallDisplayIcon={app}\molink-server.exe

[Languages]
Name: "chinesesimplified"; MessagesFile: "{#LangFile}"

[Tasks]
Name: "service"; Description: "安装为 Windows 服务（开机自动启动）"; GroupDescription: "后台服务"; Flags: checkedonce
Name: "firewall"; Description: "在防火墙中放行设备 API 端口 28430"; GroupDescription: "网络"; Flags: checkedonce
Name: "admin_lan"; Description: "允许局域网访问管理控制台（无 TLS，请仅在可信网络使用）"; GroupDescription: "管理控制台"

[Files]
Source: "{#PkgDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "config.json,config-lan.json,install-service.cmd,uninstall-service.cmd"
Source: "{#PkgDir}\config.json"; DestDir: "{commonappdata}\moLink\Server"; DestName: "config.json"; Tasks: not admin_lan; Flags: onlyifdoesntexist uninsneveruninstall
Source: "{#PkgDir}\config-lan.json"; DestDir: "{commonappdata}\moLink\Server"; DestName: "config.json"; Tasks: admin_lan; Flags: onlyifdoesntexist uninsneveruninstall

[Dirs]
Name: "{commonappdata}\moLink\Server"; Permissions: users-modify

[Icons]
Name: "{group}\moLink Server 管理控制台"; Filename: "http://127.0.0.1:28431"; IconFilename: "{app}\molink.ico"
Name: "{group}\卸载 moLink Server"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\molink-server.exe"; Parameters: "--install-service --config ""{commonappdata}\moLink\Server\config.json"""; Flags: runhidden waituntilterminated; Tasks: service; StatusMsg: "正在安装 moLink Server 服务…"
Filename: "netsh.exe"; Parameters: "advfirewall firewall add rule name=""moLink Server Device API"" dir=in action=allow protocol=TCP localport=28430 profile=private,domain"; Flags: runhidden; Tasks: firewall; StatusMsg: "正在添加防火墙规则…"

[UninstallRun]
Filename: "{app}\molink-server.exe"; Parameters: "--uninstall-service --service-name moLinkServer"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveService"
Filename: "netsh.exe"; Parameters: "advfirewall firewall delete rule name=""moLink Server Device API"""; Flags: runhidden; RunOnceId: "RemoveFirewall"

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  { Stop a running service so its executable can be replaced. The data and
    configuration under ProgramData are never removed by this installer. }
  Exec('sc.exe', 'stop moLinkServer', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(1500);
  Result := '';
end;
