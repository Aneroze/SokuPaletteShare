# Package build\PaletteShare.dll + mod.json + PaletteShare.ini + README.md, LICENSE, the third-party
# notices and WHERE-ARE-MY-PALETTES.txt into dist\PaletteShare-<version>.zip (folder layout
# PaletteShare\..., like other SokuLauncher mods). Run build.bat first.
#
# Never add palette files, palettes.ini or palettes\ folders: SokuLauncher updates a mod by copying
# the zip over the mod folder (they would overwrite the player's own), and the library lives in the
# game folder anyway (WHERE-ARE-MY-PALETTES.txt says where).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $root 'mod.json') -Raw | ConvertFrom-Json).version
$cmakeVersion = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(PaletteShare VERSION ([0-9.]+)').Matches[0].Groups[1].Value
if ($version -ne $cmakeVersion) { throw "version mismatch: mod.json says $version, CMakeLists.txt says $cmakeVersion" }

$dll = Join-Path $root 'build\PaletteShare.dll'
if (-not (Test-Path $dll)) { throw "build\PaletteShare.dll not found - run build.bat first" }
# The DLL must be built from the current sources (a stale build would ship old code).
$newestSource = Get-ChildItem (Join-Path $root 'src'), (Join-Path $root 'CMakeLists.txt') -Recurse -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($newestSource.LastWriteTime -gt (Get-Item $dll).LastWriteTime) { throw "build\PaletteShare.dll is older than $($newestSource.Name) - run build.bat" }

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$zip = Join-Path $dist "PaletteShare-$version.zip"
if (Test-Path $zip) { [IO.File]::Delete($zip) }

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::Open($zip, 'Create')
try {
  $files = @($dll) + @('mod.json', 'PaletteShare.ini', 'README.md', 'LICENSE', 'THIRD-PARTY-NOTICES.txt', 'WHERE-ARE-MY-PALETTES.txt' |
    ForEach-Object { Join-Path $root $_ })
  foreach ($f in $files) {
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $f, 'PaletteShare/' + (Split-Path -Leaf $f))
  }
} finally { $z.Dispose() }

$md5 = (Get-FileHash $dll -Algorithm MD5).Hash
Write-Host "packaged $zip (version $version, PaletteShare.dll md5 $md5)"
