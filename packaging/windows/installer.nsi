; moLink Windows installer. Build with:
;   makensis /DVERSION=0.1.0 /DPKG_DIR=<abs path to staged pkg> installer.nsi
!include "MUI2.nsh"

!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!ifndef PKG_DIR
  !error "PKG_DIR must point at the staged package directory"
!endif

Name "moLink"
!ifndef OUT_DIR
  !define OUT_DIR "."
!endif
OutFile "${OUT_DIR}\moLink-setup-${VERSION}-x64.exe"
Unicode true
RequestExecutionLevel admin
InstallDir "$PROGRAMFILES64\moLink"
InstallDirRegKey HKLM "Software\moLink" "InstallDir"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

Section "moLink" SecMain
  SetOutPath "$INSTDIR"
  File /r "${PKG_DIR}\*.*"

  CreateShortcut "$DESKTOP\moLink.lnk" "$INSTDIR\moLink.exe"
  CreateDirectory "$SMPROGRAMS\moLink"
  CreateShortcut "$SMPROGRAMS\moLink\moLink.lnk" "$INSTDIR\moLink.exe"
  CreateShortcut "$SMPROGRAMS\moLink\Uninstall.lnk" "$INSTDIR\uninstall.exe"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\moLink" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\moLink" \
    "DisplayName" "moLink"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\moLink" \
    "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\moLink" \
    "Publisher" "moLink Contributors"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\moLink" \
    "UninstallString" "$INSTDIR\uninstall.exe"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\moLink" \
    "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\moLink" \
    "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\moLink.lnk"
  Delete "$SMPROGRAMS\moLink\moLink.lnk"
  Delete "$SMPROGRAMS\moLink\Uninstall.lnk"
  RMDir "$SMPROGRAMS\moLink"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\moLink"
  DeleteRegKey HKLM "Software\moLink"
SectionEnd
