param(
  [Parameter(Mandatory = $true)][string]$PkgDir,
  [Parameter(Mandatory = $true)][string]$OutDir,
  [Parameter(Mandatory = $true)][string]$Version
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$name = "moLink-Server-$Version-windows-x64"
$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $PkgDir '*') -DestinationPath $zip -CompressionLevel Optimal
Write-Output "built $zip"
