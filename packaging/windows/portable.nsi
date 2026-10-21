; Single-file portable launcher: extracts to %TEMP% and runs moLink.
; No installation, no registry writes, no admin rights.
!include "MUI2.nsh"

!ifndef VERSION
  !define VERSION "0.3.0"
!endif
!ifndef PKG_DIR
  !error "PKG_DIR must point at the staged package directory"
!endif
!ifndef OUT_DIR
  !define OUT_DIR "."
!endif

Name "moLink Portable"
OutFile "${OUT_DIR}\moLink-${VERSION}-portable-x64.exe"
Unicode true
RequestExecutionLevel user
SilentInstall silent
AutoCloseWindow true
SetCompressor /SOLID lzma
InstallDir "$TEMP\moLink-${VERSION}"

Section
  SetOutPath "$INSTDIR"
  File /r "${PKG_DIR}\*.*"
  Exec "$INSTDIR\moLink.exe"
SectionEnd
