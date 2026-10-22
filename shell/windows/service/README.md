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

Not implemented yet:

- Capture/input on the Winlogon secure desktop (needs a helper injected into
  the active session; cannot be validated without a Windows test machine).
- Core/UI integration for choosing console vs RDP and showing the session list.
- Automatic installation during setup.

Treat this binary as experimental until those paths have been tested on
Windows 7/10/11 with a locked screen and an active RDP session.
