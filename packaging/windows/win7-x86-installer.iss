; moLink Windows 7 x86 (32-bit) preview installer.
;
; Build (on Windows, after staging the Win7 package directory):
;   ISCC.exe /DAppVersion=0.9.4 \
;            /DPkgDir=C:\path\to\molink-win7-x86-release \
;            /DOutputDir=C:\path\to\output \
;            /DLangFile=C:\path\to\ChineseSimplified.isl \
;            win7-x86-installer.iss
;
; The package is produced by the GCC 13 / libuvcpp Win7 kit build. It contains
; the GDI-capture core, the WebView2 shell, the optional secure-desktop service
; and helper, the web UI, and the runtime DLLs.

#ifndef AppVersion
  #define AppVersion "0.9.4"
#endif
#ifndef PkgDir
  #error PkgDir must point at the staged Win7 x86 package directory
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#ifndef LangFile
  #define LangFile "ChineseSimplified.isl"
#endif
#ifndef IconFile
  #define IconFile "molink.ico"
#endif

#define AppName "moLink"
#define AppExeName "moLink.exe"
#define AppId "{{B2D9F7D4-6C38-4E7A-9A4B-0B5E8C3F1A72}"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion} (Windows 7 x86)
AppPublisher=moLink Contributors
AppPublisherURL=https://github.com/Antruly/molink
AppSupportURL=https://github.com/Antruly/molink
AppUpdatesURL=https://github.com/Antruly/molink/releases
DefaultDirName={autopf}\moLink
DefaultGroupName=moLink
DisableProgramGroupPage=yes
PrivilegesRequired=admin
MinVersion=6.1sp1
ArchitecturesAllowed=x86compatible
OutputDir={#OutputDir}
OutputBaseFilename=moLink-{#AppVersion}-win7-x86-setup
SetupIconFile={#IconFile}
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
Name: "firewall"; Description: "Allow moLink through Windows Firewall (all profiles)"; GroupDescription: "Network access"; Flags: checkedonce
Name: "service"; Description: "Install the secure-desktop service (lock/login screen support)"; GroupDescription: "Background service"; Flags: unchecked
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#PkgDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PkgDir}\molink-service.exe"; DestDir: "{commonappdata}\moLink\service"; Flags: ignoreversion; Tasks: service
Source: "{#PkgDir}\molink-helper.exe"; DestDir: "{commonappdata}\moLink\service"; Flags: ignoreversion; Tasks: service

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\{#AppExeName}"; AppUserModelID: "com.molink.desktop"
Name: "{group}\{#AppName} (browser UI)"; Filename: "{app}\start-browser-ui.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\{#AppExeName}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\{#AppExeName}"; Tasks: desktopicon; AppUserModelID: "com.molink.desktop"

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""moLink Core 28418"""; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""moLink Core 28418"" dir=in action=allow protocol=TCP localport=28418 profile=any enable=yes"; Flags: runhidden; Tasks: firewall
Filename: "{sys}\sc.exe"; Parameters: "stop moLinkSession"; Flags: runhidden; Tasks: service
Filename: "{sys}\sc.exe"; Parameters: "delete moLinkSession"; Flags: runhidden; Tasks: service
Filename: "{sys}\sc.exe"; Parameters: "create moLinkSession binPath= ""{commonappdata}\moLink\service\molink-service.exe"" start= demand DisplayName= ""moLink session helper"""; Flags: runhidden waituntilterminated; Tasks: service
Filename: "{sys}\sc.exe"; Parameters: "description moLinkSession ""Secure desktop capture/input helper for moLink"""; Flags: runhidden; Tasks: service
Filename: "{sys}\sc.exe"; Parameters: "start moLinkSession"; Flags: runhidden; Tasks: service
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent; Check: WebView2RuntimePresent

[UninstallRun]
Filename: "{sys}\sc.exe"; Parameters: "stop moLinkSession"; Flags: runhidden; RunOnceId: "StopService"
Filename: "{sys}\sc.exe"; Parameters: "delete moLinkSession"; Flags: runhidden; RunOnceId: "DeleteService"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""moLink Core 28418"""; Flags: runhidden; RunOnceId: "DeleteFirewall"

[Code]
// Warn when the native WebView2 shell cannot find a runtime. The browser-UI
// shortcut still works, so this is informational rather than fatal.
function WebView2RuntimePresent(): Boolean;
var
  Names: TArrayOfString;
  I: Integer;
begin
  Result := False;
  if RegGetSubkeyNames(HKLM, 'SOFTWARE\Microsoft\EdgeUpdate\Clients', Names) then
    for I := 0 to GetArrayLength(Names) - 1 do begin
      if RegValueExists(HKLM, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\' + Names[I], 'pv') then begin
        Result := True;
        Exit;
      end;
    end;
end;

procedure InitializeWizard();
begin
  if not WebView2RuntimePresent() then
    MsgBox('WebView2 Runtime 109 (the last Windows 7 build) was not found.' + #13#10 +
           'The native shell will not start. Use the "moLink (browser UI)" shortcut' + #13#10 +
           'with Firefox ESR 115 or Chrome 109, or install the WebView2 109 runtime.',
           mbInformation, MB_OK);
end;
