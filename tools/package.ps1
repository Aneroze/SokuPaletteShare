# Package build\PaletteShare.dll + mod.json + PaletteShare.ini + README.md, LICENSE and the third-party
# notices into dist\PaletteShare-<version>.zip
# (folder layout PaletteShare\..., like other SokuLauncher mods), plus an empty palettes\<character>
# folder per character so players see where palettes go before the first start. Run build.bat first.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $root 'mod.json') -Raw | ConvertFrom-Json).version
$dll = Join-Path $root 'build\PaletteShare.dll'
if (-not (Test-Path $dll)) { throw "build\PaletteShare.dll not found - run build.bat first" }

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$zip = Join-Path $dist "PaletteShare-$version.zip"
if (Test-Path $zip) { [IO.File]::Delete($zip) }

# Same list as setLibraryRoot in src\library.cpp.
$characters = 'reimu', 'marisa', 'sakuya', 'alice', 'patchouli', 'youmu', 'remilia', 'yuyuko', 'yukari', 'suika',
  'udonge', 'aya', 'komachi', 'iku', 'tenshi', 'sanae', 'chirno', 'meirin', 'utsuho', 'suwako'

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::Open($zip, 'Create')
try {
  $files = @($dll) + @('mod.json', 'PaletteShare.ini', 'README.md', 'LICENSE', 'THIRD-PARTY-NOTICES.txt' | ForEach-Object { Join-Path $root $_ })
  foreach ($f in $files) {
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $f, 'PaletteShare/' + (Split-Path -Leaf $f))
  }
  foreach ($c in $characters) { [void]$z.CreateEntry("PaletteShare/palettes/$c/") }
} finally { $z.Dispose() }

$md5 = (Get-FileHash $dll -Algorithm MD5).Hash
Write-Host "packaged $zip (PaletteShare.dll md5 $md5)"
