# Package build\PaletteShare.dll + mod.json + PaletteShare.ini into dist\PaletteShare-<version>.zip
# (folder layout PaletteShare\..., like other SokuLauncher mods). Run build.bat first.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $root 'mod.json') -Raw | ConvertFrom-Json).version
$dll = Join-Path $root 'build\PaletteShare.dll'
if (-not (Test-Path $dll)) { throw "build\PaletteShare.dll not found - run build.bat first" }

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$zip = Join-Path $dist "PaletteShare-$version.zip"
if (Test-Path $zip) { [IO.File]::Delete($zip) }

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::Open($zip, 'Create')
try {
  foreach ($f in @($dll, (Join-Path $root 'mod.json'), (Join-Path $root 'PaletteShare.ini'))) {
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $f, 'PaletteShare/' + (Split-Path -Leaf $f))
  }
} finally { $z.Dispose() }

$md5 = (Get-FileHash $dll -Algorithm MD5).Hash
Write-Host "packaged $zip (PaletteShare.dll md5 $md5)"
