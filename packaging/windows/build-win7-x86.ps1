# Build the Windows 7 x86 (32-bit) client from the libuvcpp Win7 MinGW kit.
#
#   build-win7-x86.ps1 -Kit C:\path\libuvcpp-1.8.5-mingw32 `
#                      -Gcc C:\path\i686-13.2.0-posix-dwarf-msvcrt `
#                      -WebView2 C:\path\wv2 -Version 0.9.13
#
# -Kit is the portable build kit (uvcpp-kit-1.8.5-mingw32.zip). It supplies
# cmake, the zlib static library (molink's tile compressor calls compress2
# directly) and, in 1.8.5, a prebuilt/ prefix with the full profile
# (web + webapp). Its GCC 8.1 is too old for molink's std::filesystem use, so
# -Gcc must point at a newer i686 toolchain (mingw-builds 13.2 msvcrt is what
# the release uses).
#
# -UvcppPrefix optionally overrides that prefix with the standalone 1.8.5+
# release archive (uvcpp-win7-x86-mingw32). OpenSSL/libuv/llhttp are statically
# linked into libuvcpp.dll; only zlib is taken from the kit. Without it, the
# kit's prebuilt/ is used; only a kit older than 1.8.5 (no prebuilt full
# profile) falls back to building libuvcpp from source.
param(
  [Parameter(Mandatory = $true)][string]$Kit,
  [Parameter(Mandatory = $true)][string]$Gcc,
  [Parameter(Mandatory = $true)][string]$WebView2,
  [string]$UvcppPrefix = "",
  [string]$Repo = "",
  [string]$Version = "0.9.13",
  [string]$Out = ""
)

$ErrorActionPreference = 'Stop'
if (-not $Repo) {
  $Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
if (-not $Out) {
  $Out = Join-Path $Repo 'build\package'
}
$Kit = (Resolve-Path $Kit).Path
$Gcc = (Resolve-Path $Gcc).Path
$WebView2 = (Resolve-Path $WebView2).Path
if ($UvcppPrefix) {
  $UvcppPrefix = (Resolve-Path $UvcppPrefix).Path
} elseif (Test-Path (Join-Path $Kit 'prebuilt\include\uvcpp\uvcpp_config.h')) {
  # 1.8.5+ kits carry the full-profile install; use it instead of rebuilding.
  $UvcppPrefix = (Join-Path $Kit 'prebuilt')
  Write-Host "using the kit's prebuilt prefix: $UvcppPrefix"
}
New-Item -ItemType Directory -Force $Out | Out-Null

$cmake = Join-Path $Kit 'deps\cmake\bin\cmake.exe'
$make = Join-Path $Gcc 'bin\mingw32-make.exe'
if (-not (Test-Path $make)) { $make = Join-Path $Kit 'deps\mingw32\bin\mingw32-make.exe' }
$cc = Join-Path $Gcc 'bin\gcc.exe'
$cxx = Join-Path $Gcc 'bin\g++.exe'
$rc = Join-Path $Gcc 'bin\windres.exe'
$toolchain = Join-Path $Repo 'cmake\toolchains\mingw32-win7.cmake'
$env:MOLINK_WIN7_KIT = $Kit
$env:MOLINK_WIN7_GCC = $Gcc
$env:PATH = "$(Join-Path $Gcc 'bin');$(Join-Path $Kit 'deps\mingw32\bin');$env:PATH"

function To-CMakePath([string]$Path) { return $Path.Replace('\', '/') }
$kitF = To-CMakePath $Kit
$outF = To-CMakePath $Out
$gccF = To-CMakePath $Gcc
$ucPrefixF = "$kitF/out/gcc13"

function Invoke-CMake([string]$Source, [string]$Build, [string[]]$Extra) {
  Remove-Item -Recurse -Force $Build -ErrorAction SilentlyContinue
  $cmakeArgs = @(
    '-S', $Source, '-B', $Build, '-G', 'MinGW Makefiles',
    "-DCMAKE_MAKE_PROGRAM=$(To-CMakePath $make)",
    "-DCMAKE_C_COMPILER=$(To-CMakePath $cc)",
    "-DCMAKE_CXX_COMPILER=$(To-CMakePath $cxx)",
    "-DCMAKE_RC_COMPILER=$(To-CMakePath $rc)",
    "-DCMAKE_TOOLCHAIN_FILE=$(To-CMakePath $toolchain)",
    '-DCMAKE_BUILD_TYPE=Release'
  ) + $Extra
  & $cmake @cmakeArgs
  if ($LASTEXITCODE -ne 0) { throw "configure failed: $Source" }
  & $cmake --build $Build --parallel 8
  if ($LASTEXITCODE -ne 0) { throw "build failed: $Source" }
}

Write-Host '== libuvcpp full (web + WebApp) for Win7 x86'
if (-not $UvcppPrefix -and -not (Test-Path "$Kit\out\gcc13\bin\libuvcpp.dll")) {
  if (-not (Test-Path "$Kit\deps\nlohmann\CMakeLists.txt")) {
    throw 'The kit needs deps\nlohmann for the full profile (see tools or pack it manually).'
  }
  & $cmake -S $kitF -B "$kitF/build-gcc13-full" -G 'MinGW Makefiles' `
    -DCMAKE_C_COMPILER="$gccF/bin/gcc.exe" `
    -DCMAKE_CXX_COMPILER="$gccF/bin/g++.exe" `
    -DCMAKE_RC_COMPILER="$gccF/bin/windres.exe" `
    -DCMAKE_MAKE_PROGRAM="$(To-CMakePath $make)" `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_CXX_FLAGS=-DENABLE_VIRTUAL_TERMINAL_PROCESSING=0x0004 `
    -DCMAKE_C_FLAGS=-DENABLE_VIRTUAL_TERMINAL_PROCESSING=0x0004 `
    -DUVCPP_FEATURES=full -DUVCPP_WINVER=0x0601 -DUVCPP_BUILD_TESTS=OFF `
    -DUVCPP_STATIC_RUNTIME=ON -DBUILD_SHARED_LIBS=OFF `
    -DUVCPP_BUILD_SHARED=ON -DUVCPP_BUILD_STATIC=OFF `
    -DUVCPP_USE_SYSTEM_LIBUV=ON -DUVCPP_BUILD_LIBUV_FROM_SOURCE=OFF `
    -DLIBUV_INCLUDE_DIR="$kitF/deps/libuv/include" `
    -DLIBUV_LIB="$kitF/deps/libuv/lib/libuv_a.a" `
    -DOPENSSL_ROOT_DIR="$kitF/deps/openssl" -DOPENSSL_USE_STATIC_LIBS=ON `
    -DZLIB_ROOT="$kitF/deps/zlib" `
    -DZLIB_LIBRARY="$kitF/deps/zlib/lib/libzlibstatic.a" `
    -DZLIB_INCLUDE_DIR="$kitF/deps/zlib/include" `
    -DFETCHCONTENT_SOURCE_DIR_LLHTTP="$kitF/deps/llhttp" `
    -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$kitF/deps/nlohmann" `
    -DCMAKE_INSTALL_PREFIX="$ucPrefixF"
  if ($LASTEXITCODE -ne 0) { throw 'libuvcpp configure failed' }
  & $cmake --build "$Kit\build-gcc13-full" --parallel 8
  if ($LASTEXITCODE -ne 0) { throw 'libuvcpp build failed' }
  & $cmake --install "$Kit\build-gcc13-full"
  if ($LASTEXITCODE -ne 0) { throw 'libuvcpp install failed' }
}
if (-not $UvcppPrefix) {
  # uvcpp's CMake target links bare `uv`; expose the kit's import lib/headers.
  $uc = Join-Path $Kit 'out\gcc13'
  Copy-Item (Join-Path $Kit 'deps\libuv\lib\libuv.dll.a') (Join-Path $uc 'lib') -Force
  Copy-Item (Join-Path $Kit 'deps\libuv\bin\libuv.dll') (Join-Path $uc 'bin') -Force
  Copy-Item -Recurse -Force (Join-Path $Kit 'deps\libuv\include\*') (Join-Path $uc 'include')
}

Write-Host '== molink-core'
if ($UvcppPrefix) {
  $uvcppPrefixF = To-CMakePath $UvcppPrefix
  $coreFlags = @(
    "-DMOLINK_UVCPP_PREFIX=$uvcppPrefixF",
    "-DCMAKE_PREFIX_PATH=$uvcppPrefixF;$kitF/deps/zlib",
    '-DMOLINK_CXX_STANDARD=17',
    "-DZLIB_INCLUDE_DIR=$kitF/deps/zlib/include",
    "-DZLIB_LIBRARY=$kitF/deps/zlib/lib/libzlibstatic.a"
  )
} else {
  $coreFlags = @(
    "-Duvcpp_DIR=$($ucPrefixF)/lib/cmake/uvcpp",
    "-DCMAKE_PREFIX_PATH=$($ucPrefixF);$kitF/deps/libuv;$kitF/deps/zlib;$kitF/deps/openssl",
    '-DMOLINK_CXX_STANDARD=17',
    "-DZLIB_INCLUDE_DIR=$kitF/deps/zlib/include",
    "-DZLIB_LIBRARY=$kitF/deps/zlib/lib/libzlibstatic.a"
  )
}
Invoke-CMake (Join-Path $Repo 'backend') (Join-Path $Repo 'build-win7x86-gcc13') $coreFlags

Write-Host '== moLink shell'
Invoke-CMake (Join-Path $Repo 'shell\windows') (Join-Path $Repo 'build-win7x86-shell') @(
  "-DWEBVIEW2_SDK=$(To-CMakePath $WebView2)",
  '-DMOLINK_CXX_STANDARD=17'
)

Write-Host '== service and helper'
Invoke-CMake (Join-Path $Repo 'shell\windows\service') (Join-Path $Repo 'build-win7x86-service') @()
Invoke-CMake (Join-Path $Repo 'shell\windows\helper') (Join-Path $Repo 'build-win7x86-helper') @()

$frontend = Join-Path $Repo 'frontend\dist'
if (-not (Test-Path (Join-Path $frontend 'index.html'))) {
  Write-Host '== frontend'
  Push-Location (Join-Path $Repo 'frontend')
  try {
    & pnpm install --frozen-lockfile
    if ($LASTEXITCODE -ne 0) { throw 'pnpm install failed' }
    & pnpm build
    if ($LASTEXITCODE -ne 0) { throw 'frontend build failed' }
  } finally {
    Pop-Location
  }
}

Write-Host '== stage package'
$pkg = Join-Path $Repo 'build\molink-win7-x86'
Remove-Item -Recurse -Force $pkg -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $pkg | Out-Null
New-Item -ItemType Directory -Force (Join-Path $pkg 'web') | Out-Null
Copy-Item (Join-Path $Repo 'build-win7x86-gcc13\molink-core.exe') $pkg
Copy-Item (Join-Path $Repo 'build-win7x86-shell\moLink.exe') $pkg
Copy-Item (Join-Path $Repo 'build-win7x86-shell\WebView2Loader.dll') $pkg
Copy-Item (Join-Path $Repo 'build-win7x86-service\molink-service.exe') $pkg
Copy-Item (Join-Path $Repo 'build-win7x86-helper\molink-helper.exe') $pkg
# Prebuilt kit: libuvcpp.dll lives in its bin/. Source build: in out\gcc13\bin.
$uvcppDll = if ($UvcppPrefix) {
  Join-Path $UvcppPrefix 'bin\libuvcpp.dll'
} else {
  Join-Path $Kit 'out\gcc13\bin\libuvcpp.dll'
}
Copy-Item $uvcppDll $pkg
if (-not $UvcppPrefix) {
  # The prebuilt/release libuvcpp.dll links libuv statically; a source build
  # against the kit's shared uv target still needs it next to the DLL.
  Copy-Item (Join-Path $Kit 'deps\libuv\bin\libuv.dll') $pkg
}
Copy-Item (Join-Path $Gcc 'bin\libstdc++-6.dll') $pkg
Copy-Item (Join-Path $Gcc 'bin\libgcc_s_dw2-1.dll') $pkg
Copy-Item (Join-Path $Gcc 'bin\libwinpthread-1.dll') $pkg
Copy-Item -Recurse (Join-Path $frontend '*') (Join-Path $pkg 'web')

$settings = "server_enabled=1`r`nserver_url=`r`nserver_ca=`r`nallow_remote_access=1`r`nallow_web_client=1`r`ndiscovery_enabled=0`r`n"
Set-Content -Path (Join-Path $pkg 'settings.example') -Value $settings -Encoding ASCII
Set-Content -Path (Join-Path $pkg 'start-browser.cmd') -Encoding ASCII -Value "@echo off`r`nset MOLINK_CONFIG_DIR=%APPDATA%\moLink`r`ncd /d %~dp0`r`nmolink-core.exe --host 0.0.0.0 --port 28418 --print-ready`r`n"
Set-Content -Path (Join-Path $pkg 'start-browser-ui.cmd') -Encoding ASCII -Value "@echo off`r`nset MOLINK_CONFIG_DIR=%APPDATA%\moLink`r`ncd /d %~dp0`r`nstart `"`" molink-core.exe --host 0.0.0.0 --port 28418 --print-ready`r`ntimeout /t 2 >nul`r`nstart `"`" http://127.0.0.1:28418/`r`n"
Set-Content -Path (Join-Path $pkg 'install-firewall.cmd') -Encoding ASCII -Value "@echo off`r`nnetsh advfirewall firewall add rule name=`"moLink Core 28418`" dir=in action=allow protocol=TCP localport=28418`r`npause`r`n"
Set-Content -Path (Join-Path $pkg 'install-service.cmd') -Encoding ASCII -Value "@echo off`r`nif not exist `"%ProgramData%\moLink\service`" mkdir `"%ProgramData%\moLink\service`"`r`ncopy /Y `"%~dp0molink-service.exe`" `"%ProgramData%\moLink\service\`"`r`ncopy /Y `"%~dp0molink-helper.exe`" `"%ProgramData%\moLink\service\`"`r`nsc create moLinkSession binPath= `"%ProgramData%\moLink\service\molink-service.exe`" start= demand DisplayName= `"moLink session helper`"`r`nsc start moLinkSession`r`npause`r`n"
Set-Content -Path (Join-Path $pkg 'uninstall-service.cmd') -Encoding ASCII -Value "@echo off`r`nsc stop moLinkSession`r`nsc delete moLinkSession`r`ndel /Q `"%ProgramData%\moLink\service\molink-service.exe`"`r`ndel /Q `"%ProgramData%\moLink\service\molink-helper.exe`"`r`npause`r`n"
$readme = @"
moLink $Version - Windows 7 x86 (32-bit)

  moLink.exe            Win32 + WebView2 shell (WebView2 Runtime 109)
  molink-core.exe       GDI capture + SendInput core
  molink-service.exe    Optional secure-desktop service
  molink-helper.exe     Secure-desktop helper
  web\                  Client web UI
  start-browser.cmd     Start the core only
  start-browser-ui.cmd  Start the core and open the web UI
  install-firewall.cmd  Allow inbound 28418 (administrator)
  install-service.cmd   Install the secure-desktop service (administrator)

Without WebView2 109 use start-browser-ui.cmd with Firefox ESR 115 or
Chrome 109 (IE11 cannot run the React UI). Config: %APPDATA%\moLink.
"@
Set-Content -Path (Join-Path $pkg 'README.txt') -Value $readme -Encoding UTF8

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = Join-Path $Out "moLink-$Version-win7-x86.zip"
Remove-Item -Force $zip -ErrorAction SilentlyContinue
[System.IO.Compression.ZipFile]::CreateFromDirectory($pkg, $zip)
Write-Host "zip: $zip"

$iscc = @(
  'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
  'C:\Program Files\Inno Setup 6\ISCC.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($iscc) {
  $iss = Join-Path $Repo 'packaging\windows\win7-x86-installer.iss'
  $lang = Join-Path $Repo 'packaging\windows\ChineseSimplified.isl'
  $icon = Join-Path $Repo 'shell\assets\icons\icon.ico'
  & $iscc "/DAppVersion=$Version" "/DPkgDir=$pkg" "/DOutputDir=$Out" `
    "/DLangFile=$lang" "/DIconFile=$icon" $iss
  if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compile failed' }
  Write-Host "setup: $(Join-Path $Out "moLink-$Version-win7-x86-setup.exe")"
} else {
  Write-Host 'Inno Setup not found; skipping the installer'
}
