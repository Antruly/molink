# Build the host-only ("被控端专用") Windows package stage.
#
#   build-host-only.ps1 -Version 0.12.32 -Arch x64 -Stage C:\path\to\stage
#
# The CI job compiles the core, the Direct2D host shell, the service/helper and
# the frontend first; this script only collects the already-built artifacts
# into one directory and copies the runtime DLL set next to them.
# packaging\windows\host-only.iss consumes that directory.
#
# x86 is rejected on purpose: moLink-host.exe currently has only the MSVC x64
# target. The Win7 x86 path (GCC/MinGW) is not wired up yet, and failing here
# is better than producing a package with half of its binaries missing.
param(
  [string]$Version = "0.12.32",
  [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
  [string]$Stage = '',
  [string]$Repo = '',
  [string]$CoreBuild = '',
  [string]$ShellBuild = '',
  [string]$ServiceBuild = '',
  [string]$HelperBuild = '',
  [string]$FrontendDist = '',
  [string]$UvcppRoot = '',
  [string]$VcpkgRoot = ''
)

$ErrorActionPreference = 'Stop'

if ($Arch -ne 'x64') {
  throw ("x86 构建暂不支持：moLink-host.exe 目前只有 MSVC x64 目标，还没有 Win7 x86 " +
         "(GCC/MinGW) 构建，因此不会生成不完整的 x86 包。请使用 -Arch x64。 " +
         "(x86 build is not supported yet.)")
}

# These two lists are also parsed by tools/check-inno-script.py. They must match
# the HostPayload/HostWebDir defines in packaging\windows\host-only.iss.
$HostPayload = @('moLink-host.exe', 'molink-core.exe', 'molink-service.exe',
                 'molink-helper.exe', 'molink.ico')
$HostWebDir = 'web'

if (-not $Repo) {
  $Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
$Repo = (Resolve-Path $Repo).Path
if (-not $Stage) {
  $Stage = Join-Path $Repo 'build\host-only\stage'
}
$Stage = [System.IO.Path]::GetFullPath($Stage)

# Cleaning the output is normal, but never let a mistyped -Stage wipe the
# checkout or one of its parents.
$repoFull = [System.IO.Path]::GetFullPath($Repo).TrimEnd('\')
$stageFull = $Stage.TrimEnd('\')
if ($stageFull -eq $repoFull -or
    $repoFull.StartsWith($stageFull + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
  throw "refusing to clean -Stage '$Stage': it contains the repository"
}

# The core compiled in this tree is the one the host-only product ships. It
# must have been configured with -DMOLINK_HOST_ONLY_BUILD=ON; the check right
# below refuses to stage a full-product core. The default build directory is
# separate from the full product's backend\build so a normal release build
# cannot be reused by accident.
if (-not $CoreBuild) { $CoreBuild = Join-Path $Repo 'backend\build-host-only' }
if (-not $ShellBuild) { $ShellBuild = Join-Path $Repo 'shell\windows\build' }
if (-not $ServiceBuild) { $ServiceBuild = Join-Path $Repo 'shell\windows\service\build' }
if (-not $HelperBuild) { $HelperBuild = Join-Path $Repo 'shell\windows\helper\build' }
if (-not $FrontendDist) { $FrontendDist = Join-Path $Repo 'frontend\dist' }

# The host_only default is compiled into the core (backend/src/settings.h).
# A full-product core staged here would look identical until someone points a
# viewer at another machine, so verify the configure record before copying
# anything. CMakeCache.txt is written by both the Visual Studio and Ninja/Make
# generators, in the build root.
$coreCache = Join-Path $CoreBuild 'CMakeCache.txt'
if (-not (Test-Path $coreCache -PathType Leaf)) {
  throw ("the core build has no CMakeCache.txt: $CoreBuild. Configure it " +
         "with -DMOLINK_HOST_ONLY_BUILD=ON first.")
}
if (-not (Select-String -Path $coreCache -Pattern '^MOLINK_HOST_ONLY_BUILD:BOOL=ON$' -Quiet)) {
  throw ("the core in '$CoreBuild' was not configured with " +
         "-DMOLINK_HOST_ONLY_BUILD=ON; this stage would ship a core that can " +
         "still connect to other machines. Reconfigure that build directory " +
         "with -DMOLINK_HOST_ONLY_BUILD=ON and rebuild it.")
}

function Find-Built([string]$Directory, [string]$Name) {
  # Visual Studio generators put the exe in Release\; Ninja/Make put it in the
  # build root. Accept both so this works in CI and locally.
  foreach ($candidate in @((Join-Path $Directory "Release\$Name"),
                           (Join-Path $Directory $Name))) {
    if (Test-Path $candidate -PathType Leaf) {
      return (Resolve-Path $candidate).Path
    }
  }
  throw "built artifact not found: $Name under $Directory (run the build steps first)"
}

$coreExe = Find-Built $CoreBuild 'molink-core.exe'
$hostExe = Find-Built $ShellBuild 'moLink-host.exe'
$serviceExe = Find-Built $ServiceBuild 'molink-service.exe'
$helperExe = Find-Built $HelperBuild 'molink-helper.exe'
$icon = Join-Path $Repo 'shell\assets\icons\icon.ico'
if (-not (Test-Path $icon -PathType Leaf)) {
  throw "the application icon is missing: $icon"
}
$webIndex = Join-Path $FrontendDist 'index.html'
if (-not (Test-Path $webIndex -PathType Leaf)) {
  throw "the frontend build is missing: $webIndex (run pnpm build first)"
}

Write-Host "== stage moLink Host $Version ($Arch)"
Write-Host "   core:    $coreExe"
Write-Host "   shell:   $hostExe"
Write-Host "   service: $serviceExe"
Write-Host "   helper:  $helperExe"

if (Test-Path $Stage) {
  # A failed clean-up must stop the build: silently keeping a stale DLL from a
  # previous run can produce an installer whose contents nobody can explain.
  Remove-Item -Recurse -Force $Stage
}
New-Item -ItemType Directory -Force $Stage | Out-Null

Copy-Item $hostExe (Join-Path $Stage 'moLink-host.exe')
Copy-Item $coreExe (Join-Path $Stage 'molink-core.exe')
Copy-Item $serviceExe (Join-Path $Stage 'molink-service.exe')
Copy-Item $helperExe (Join-Path $Stage 'molink-helper.exe')
Copy-Item $icon (Join-Path $Stage 'molink.ico')

$webTarget = Join-Path $Stage $HostWebDir
New-Item -ItemType Directory -Force $webTarget | Out-Null
Copy-Item -Path (Join-Path $FrontendDist '*') -Destination $webTarget -Recurse -Force

# ---------------------------------------------------------------------------
# Runtime DLLs. The package must include everything the core, shell, service
# and helper import, otherwise it installs and then cannot start. The sources
# mirror packaging/windows/stage.sh: the build trees, the libuvcpp archive, the
# vcpkg runtime, OpenSSL and the newest MSVC redistributable.
# ---------------------------------------------------------------------------
$dllDirs = New-Object System.Collections.Generic.List[string]
function Add-DllDirectory([string]$Directory) {
  if (-not $Directory) { return }
  if (-not (Test-Path $Directory -PathType Container)) { return }
  $full = (Resolve-Path $Directory).Path
  if (-not ($dllDirs -contains $full)) { $dllDirs.Add($full) }
}

foreach ($directory in @($CoreBuild, $ShellBuild, $ServiceBuild, $HelperBuild)) {
  Add-DllDirectory $directory
}
Add-DllDirectory (Split-Path $coreExe -Parent)
Add-DllDirectory (Split-Path $hostExe -Parent)
Add-DllDirectory (Split-Path $serviceExe -Parent)
Add-DllDirectory (Split-Path $helperExe -Parent)

if (-not $UvcppRoot) {
  if ($env:UVCPP_ROOT) {
    $UvcppRoot = $env:UVCPP_ROOT
  } elseif ($env:RUNNER_TEMP) {
    $candidate = Join-Path $env:RUNNER_TEMP 'uvcpp'
    if (Test-Path $candidate -PathType Container) { $UvcppRoot = $candidate }
  }
}
if ($UvcppRoot) {
  Add-DllDirectory (Join-Path $UvcppRoot 'bin')
  Add-DllDirectory $UvcppRoot
} else {
  Write-Warning 'libuvcpp prefix not found (set -UvcppRoot or UVCPP_ROOT); libuvcpp.dll must already be in the build tree'
}

if (-not $VcpkgRoot) { $VcpkgRoot = $env:VCPKG_INSTALLATION_ROOT }
if ($VcpkgRoot) {
  # libdatachannel, libvpx, dav1d, SVT-AV1, libjpeg-turbo and zlib all land
  # here. Copying the whole runtime set avoids a hand-maintained list that has
  # gone stale one DLL at a time (juice.dll, srtp2.dll, jpeg62.dll, z.dll).
  Add-DllDirectory (Join-Path $VcpkgRoot 'installed\x64-windows\bin')
} else {
  Write-Warning 'VCPKG_INSTALLATION_ROOT is not set; vcpkg runtime DLLs were not staged'
}

$opensslDirs = @()
if ($env:OPENSSL_ROOT_DIR) {
  $opensslDirs += (Join-Path $env:OPENSSL_ROOT_DIR 'bin')
}
$opensslDirs += @('C:\OpenSSL-Win64\bin', 'C:\Program Files\OpenSSL-Win64\bin',
                  'C:\Program Files\OpenSSL\bin')
foreach ($directory in $opensslDirs) { Add-DllDirectory $directory }

foreach ($directory in $dllDirs) {
  # Do not ship debug CRT DLLs (msvcp140d.dll etc.). A plain '*d.dll' filter
  # would also drop the release decoder dav1d.dll, which the core imports.
  Get-ChildItem -Path $directory -Filter '*.dll' -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notmatch '(?i)^(msvcp|vcruntime|concrt|ucrtbase|libcpmt).*d\.dll$' } |
    ForEach-Object { Copy-Item $_.FullName $Stage -Force }
}

# The libuvcpp archive ships an old MSVCP140.dll. A newer runtime runs binaries
# built with older toolsets too, so overwrite the whole VC14 family with the
# newest redistributable installed on the runner.
if ($env:ProgramFiles) {
  $crtPattern = Join-Path $env:ProgramFiles `
    'Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC14*CRT'
  $crt = Get-ChildItem -Path $crtPattern -Directory -ErrorAction SilentlyContinue |
    Sort-Object { try { [version]$_.Parent.Parent.Name } catch { [version]'0.0' } } |
    Select-Object -Last 1
  if ($crt) {
    foreach ($pattern in @('msvcp140*.dll', 'vcruntime140*.dll', 'concrt140.dll')) {
      Get-ChildItem -Path (Join-Path $crt.FullName $pattern) -File `
        -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName $Stage -Force }
    }
    Write-Host "   MSVC runtime: $($crt.FullName)"
  } else {
    Write-Warning 'no MSVC redistributable directory was found; relying on the DLLs already in the build tree'
  }
}

# ---------------------------------------------------------------------------
# Final check: the stage must contain exactly the payload the installer
# promises, plus the web UI and the runtime.
# ---------------------------------------------------------------------------
$missing = @()
foreach ($name in $HostPayload) {
  if (-not (Test-Path (Join-Path $Stage $name) -PathType Leaf)) { $missing += $name }
}
if (-not (Test-Path (Join-Path $Stage "$HostWebDir\index.html") -PathType Leaf)) {
  $missing += "$HostWebDir\index.html"
}
if (-not (Test-Path (Join-Path $Stage 'msvcp140.dll') -PathType Leaf)) {
  $missing += 'msvcp140.dll'
}
if (-not (Test-Path (Join-Path $Stage 'vcruntime140.dll') -PathType Leaf)) {
  $missing += 'vcruntime140.dll'
}
if ($missing.Count -gt 0) {
  throw "the host-only stage is incomplete: $($missing -join ', ')"
}

$webFiles = @(Get-ChildItem (Join-Path $Stage $HostWebDir) -Recurse -File)
Write-Host "host-only stage ready: $Stage"
Get-ChildItem $Stage -File | Sort-Object Name |
  ForEach-Object { Write-Host ("  {0}" -f $_.Name) }
Write-Host ("  {0}\ ({1} files)" -f $HostWebDir, $webFiles.Count)
