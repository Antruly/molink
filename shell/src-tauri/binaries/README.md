# Bundled binaries

This directory is populated by CI before `tauri build`:

- `molink-core` / `molink-core.exe` — the core built from `backend/`.
- On Linux, the shared libraries it needs (`libuvcpp.so`, `libllhttp.so`, …),
  placed beside the core. The core is linked with `$ORIGIN` on its RPATH so it
  finds them there.

None of these files are committed; see `.gitignore` at the repository root.

For `tauri dev` the shell falls back to `backend/build/molink-core`, so this
directory can stay empty during development.
