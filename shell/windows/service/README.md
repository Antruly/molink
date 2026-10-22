# molink-service (experimental)

LocalSystem helper for Windows session control. It is **not installed by the
installer** and is not wired to the UI yet.

Build (MSVC or MinGW):

```powershell
cmake -S shell/windows/service -B shell/windows/service/build
cmake --build shell/windows/service/build --config Release
```

Install as an administrator:

```powershell
.\molink-service.exe --install
sc start moLinkSession
```

It listens on `\\.\pipe\molink-session` with an ACL for SYSTEM and
Administrators only. One line per request, JSON response:

- `sessions` — enumerate WTS sessions (`id`, `state`, `station`, `user`)
- `kick <session-id>` — `WTSLogoffSession` (refuses the console session)
- `switch <rdp-session-id>` — `tscon <id> /dest:console`

Secure desktop (experimental):

- `secure-start` launches `molink-helper.exe` as SYSTEM in the active console
  session. The helper opens the current input desktop (Winlogon/UAC), captures
  BGRA frames and injects input over `\\.\pipe\molink-secure`.
- `secure-stop` terminates it.
- The core is not wired to the secure pipe yet.

Not implemented yet:

- Core integration for the secure-desktop frame/input pipe.
- Automatic service installation during setup; core/UI session control needs
  an elevated app for kick/switch/secure commands.
- Real-machine validation on Windows 7/10/11.

Treat this binary as experimental until those paths have been tested on
Windows 7/10/11 with a locked screen and an active RDP session.
