@echo off
setlocal
set "DIR=%~dp0"
"%DIR%molink-server.exe" --install-service --config "%DIR%config.json"
if errorlevel 1 (
  echo Service installation failed.
  pause
  exit /b 1
)
echo moLink Server service installed.
echo Admin console: http://127.0.0.1:28431
pause
