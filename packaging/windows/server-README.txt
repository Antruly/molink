moLink Server (Windows)
=======================

This package contains the moLink device directory, admin console and API.

Quick start (console):
  1. Edit config.json (next to this file) if you need different ports or data paths.
  2. Run: molink-server.exe --config config.json
  3. Open http://127.0.0.1:28431 and create the first administrator.

Install as a Windows service:
  - Run install-service.cmd as Administrator (or use the installer's service task).
  - uninstall-service.cmd removes the service; data under C:\ProgramData\moLink\Server is kept.

Ports:
  28430  device API (clients register/heartbeat/lookup)
  28431  admin console (loopback by default)

The package includes the OpenSSL and Visual C++ runtime DLLs when they were found
on the build machine. On a clean Windows installation install the Microsoft
Visual C++ Redistributable if the server reports a missing VCRUNTIME/MSVCP DLL.

Configuration is JSON; see the server documentation for IP policy, rate limits,
TLS and PostgreSQL settings.
