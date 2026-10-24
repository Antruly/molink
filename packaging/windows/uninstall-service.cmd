@echo off
setlocal
"%~dp0molink-server.exe" --uninstall-service --service-name moLinkServer
if errorlevel 1 (
  echo Service removal failed.
  pause
  exit /b 1
)
echo moLink Server service removed.
pause
