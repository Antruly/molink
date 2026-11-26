# moLink Android shell (M1)

A Kotlin + WebView **control-side** client. It loads the existing moLink Web UI
from a host's core (`http://<host>:<port>/`) and adds the parts a browser cannot
do by itself: touchpad-style gestures, the system file picker, and downloads
into `Download/`.

This is stage A of the mobile plan (roadmap §7): no native decoder, no
`MediaCodec`, and the phone is not a host. The Web UI's existing tile/stream
renderer is used as-is.

```
mobile/android/
  settings.gradle.kts          Aliyun Maven mirrors first
  build.gradle.kts             AGP 8.5.2 / Kotlin 1.9.24
  gradle/wrapper/              Gradle 8.7, Tencent mirror
  app/src/main/
    AndroidManifest.xml
    java/com/molink/remote/
      HostSetupActivity.kt     address + verification code (launcher)
      MainActivity.kt          WebView activity, file chooser, downloads
      SessionView.kt           WebView config + JS gesture bridge
      HostTarget.kt            host:port parsing / URL building
      Downloads.kt             Download/ writer (MediaStore + legacy)
    res/                       layouts, English + Chinese strings, theme, icon
```

## Requirements

- JDK 17 (Temurin 17 is what this was built with).
- Android SDK with:
  - `platforms;android-34`
  - `build-tools;34.0.0`
  - `platform-tools`
- Network access to the Tencent Gradle mirror and the Aliyun Maven mirrors.
  They are configured in `gradle/wrapper/gradle-wrapper.properties` and
  `settings.gradle.kts`; no extra setup is needed.

Installing the SDK without Android Studio:

```bash
export ANDROID_HOME="$HOME/.local/android-sdk"
mkdir -p "$ANDROID_HOME"
# commandline-tools zip from https://developer.android.com/studio#command-tools
unzip commandlinetools-linux-*.zip -d "$ANDROID_HOME/cmdline-tools"
mv "$ANDROID_HOME/cmdline-tools/cmdline-tools" "$ANDROID_HOME/cmdline-tools/latest"
yes | "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --licenses
"$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" \
  "platform-tools" "platforms;android-34" "build-tools;34.0.0"
```

Also export `JAVA_HOME` to a JDK 17 installation. AGP finds the SDK through
`ANDROID_HOME`; a `local.properties` with `sdk.dir=...` works too but is not
needed (and is git-ignored).

## Build

```bash
cd mobile/android
export JAVA_HOME=/path/to/jdk-17
export ANDROID_HOME=/path/to/android-sdk
./gradlew :app:assembleDebug
```

The first run downloads Gradle 8.7 from the Tencent mirror and the Android
Gradle plugin / AndroidX artifacts from Aliyun. The APK ends up at:

```
app/build/outputs/apk/debug/app-debug.apk
```

`compileSdk 34`, `minSdk 24`, `targetSdk 34`, application id
`com.molink.remote`.

## Install

With USB debugging enabled:

```bash
"$ANDROID_HOME/platform-tools/adb" install -r app/build/outputs/apk/debug/app-debug.apk
```

Or copy the APK to the phone and open it (allow installation from unknown
sources for the file manager). The launcher entry is **moLink Remote**.

## Using it

1. On the host, enable remote web access in **Security** (`allow_remote_access`,
   and `allow_web_client` if you want the UI served to non-loopback peers).
   Note the address (`IP:port`, usually `:28418`) and the current verification
   code.
2. In the app enter `192.168.1.10:28418` (the port defaults to 28418) and the
   code, then tap **Connect**.
3. The shell opens `http://<host>:<port>/`. If it has a code, it fills the
   host's login field (`#auth-code`) once and submits that form; a wrong code
   is left on screen so it can be corrected normally.
4. The top bar has **Keyboard** (focus the remote IME input and show the system
   keyboard), **Reload**, and **Host** (back to the address screen).

The Web UI is not modified. Everything it already supports (session control,
file drawer, clipboard panel, virtual keyboard button, quality panel, ...)
stays available and uses normal touch input.

### Optional `?client=` token

The host's Web UI only needs `client` when `allow_web_client` is off and the
page is loaded over loopback. A phone normally connects remotely and does not
need it. If you do have the host's per-launch client token, pass it when
starting the setup activity:

```bash
adb shell am start -n com.molink.remote/.HostSetupActivity \
  --es client_token <token> --es host 192.168.1.10 --ei port 28418 --es code 123456
```

When no token is supplied the shell opens the plain root path.

## Gestures

All gestures apply **only on the remote picture**. Touches on toolbars,
dialogs, the login form and the file drawer go to the WebView normally.

| Gesture | Remote action |
| --- | --- |
| One-finger tap | left click |
| One-finger drag | `mousemove` (no button held) |
| Long press (500 ms) | right click |
| Two-finger swipe | wheel (20 px of finger travel = one notch) |
| Two-finger pinch | Ctrl held + wheel (host zoom, no local WebView zoom) |

The pinch puts the remote cursor at the pinch centre first, so the host zooms
where the fingers are.

### How it is implemented

The shell injects one JavaScript bridge into the page (from both
`onPageStarted` and `onPageFinished`; it is idempotent). The bridge:

1. Installs **capture-phase** `pointerdown/move/up/cancel` listeners on
   `window`.
2. Recognises the remote picture by its stable Tailwind markers
   `touch-none select-none outline-none`; a touch that starts anywhere else is
   not consumed, so the normal UI keeps working.
3. Consumes the raw touch pointer events (`preventDefault`,
   `stopImmediatePropagation`) so React never sees the touch as a held left
   button.
4. Dispatches **synthetic** `PointerEvent`s (`pointerType: 'touch'`,
   `pointerId: 1`) straight at the picture element for hover moves, left/right
   clicks and button transitions. React's delegated handlers turn those into
   the existing `{k:"move"}` / `{k:"button"}` input messages.
5. Dispatches synthetic `WheelEvent`s (`deltaMode: 0`, 100 px per notch) on the
   picture; the Web UI's existing wheel listener sends `{k:"wheel"}`.
6. For pinch, dispatches synthetic `KeyboardEvent`s for `ControlLeft` around
   the wheel events, so the host receives Ctrl+wheel. The Ctrl key is released
   when the gesture ends or is cancelled.

Why not `WebView.dispatchTouchEvent`? Forwarding raw touches produces real
touch/pointer events, and the page interprets a touch as button-down drag and
two fingers as its own pinch/scroll paths. That is not the requested touchpad
mapping. Dispatching our own pointer events gives exact control and keeps
non-picture UI untouched. The only compatibility shim needed is a no-op wrapper
around `Element.setPointerCapture`/`releasePointerCapture`: a synthesised
pointer has no active pointer id, and Chromium would otherwise throw before the
page's button-down code runs.

Three-finger gestures and physical mice/keyboards are **not** implemented in
M1.

## Keyboard, clipboard, files

- **Keyboard**: `windowSoftInputMode="adjustResize"`. Tapping the remote
  picture focuses the page's hidden IME input (the Web UI's normal touch
  behaviour); the top-bar **Keyboard** button does the same explicitly and
  shows the system IME.
- **Clipboard**: nothing is disabled. WebView's clipboard works with
  `ClipboardManager` as usual (text selection / copy / paste in the page). The
  Web UI's clipboard panel is unchanged. Note that a plain `http://` origin is
  not a secure context, so the async Clipboard API may fall back to the Web
  UI's own panel/commands.
- **Upload**: `WebChromeClient.onShowFileChooser` opens `ACTION_OPEN_DOCUMENT`
  with the page's MIME filters and multiple selection when requested.
- **Download**:
  - `http(s)://` → `DownloadManager`, destination
    `Environment.DIRECTORY_DOWNLOADS`.
  - `data:` → decoded and written to `Download/` directly.
  - `blob:` → the page's blob is renderer-local, so the injected bridge reads
    it and streams 192 KiB base64 chunks over a `JavascriptInterface`; the
    native side writes them to `Download/`.
  - Before Android 10 the app asks for `WRITE_EXTERNAL_STORAGE` on the first
    download (declared with `maxSdkVersion=28`).
  - Folder upload (`webkitdirectory`) is not mapped to
    `ACTION_OPEN_DOCUMENT_TREE`; it falls back to a normal file picker.

## Status / limitations

Verified on the build machine (no emulator or physical device was available):

- `./gradlew :app:assembleDebug` succeeds and produces an installable APK.
- `aapt2 dump badging` reports the expected package, SDK levels and version.

Not verified on hardware:

- Touch gesture behaviour, IME show/hide, the file chooser, and downloads.
  These are implemented but have not been exercised on a phone.

Not implemented in M1:

- Native decoding / `MediaCodec` (stage B), `SurfaceView` rendering.
- Phone-as-host (screen capture / input injection, stage C).
- Foreground service / keep-alive.
- Camera/microphone `onPermissionRequest` handling (the Web UI's voice call
  will not get microphone permission yet).
- Pop-out windows (`window.open`): the mobile Web UI uses tabs.
- Self-signed HTTPS: the shell talks plain HTTP; a host with a self-signed
  certificate would show a WebView certificate error.
- The host must allow remote web access; with both switches off the WebView
  shows the core's `web_client_disabled` error instead of the UI.
- Physical mouse/keyboard (OTG) mapping and an orientation lock.
