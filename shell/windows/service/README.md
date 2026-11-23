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

Core supervision:

- The helper also owns `molink-core.exe`. If a core is already running (the
  desktop shell starts one and then enables the service), the helper waits for
  it instead of launching a second one. When it exits, the helper logs
  `core exited code=0x...` to `service.log`, writes an `app.exit
  supervisor=helper ...` line to the daily diagnostic log, and restarts it
  after 1 s, 2 s, 5 s, 10 s and 10 s. The sixth consecutive abnormal exit is
  final: `core restart gave up` (the first five are restarted, matching the
  field test of six kills).
- An exit code of 0 is a normal exit and is never restarted.
- A named mutex (`Global\moLinkCoreSupervisor`) and a scan for an existing
  `molink-core.exe` keep one helper from racing another onto port 28418.
- On a console session change the helper stops the old core (Ctrl+Break, 3 s
  grace, then terminate) before the service relaunches it in the new session.
  The service temporarily refuses `SERVICE_CONTROL_STOP` during that hand-off,
  because the old core asks the SCM to stop the service on its own shutdown.
  A helper that cannot stop its core leaves `core.pid` behind (pid plus the
  process creation time); the service checks the image name and creation time
  and terminates the stale core before the new session's helper starts.
- Stopping the service itself (the app's Stop button, or `secure-stop`) only
  stops the helper: it receives a separate detach event and leaves the core
  running, which then falls back from the secure-desktop pipe to DXGI capture.
- `molink-service.exe --install` records the core path in
  `%ProgramData%\moLink\service\core.path` (one line, the absolute path); the
  helper also checks `MOLINK_CORE`, its own directory and
  `Program Files\moLink`. If the core lives somewhere else, write that file by
  hand.

Not implemented yet:

- Core integration for the secure-desktop frame/input pipe.
- Automatic service installation during setup; core/UI session control needs
  an elevated app for kick/switch/secure commands.
- Real-machine validation on Windows 7/10/11.

Treat this binary as experimental until those paths have been tested on
Windows 7/10/11 with a locked screen and an active RDP session.
