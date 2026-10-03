# Releasing

## Build the release zip

```
build.bat
tools\test.bat
pwsh -File tools\package.ps1
```

After a Visual Studio update, check that no Windows 8+ import came back (Windows 7 support):
`dumpbin /imports build\PaletteShare.dll` must not list GetSystemTimePreciseAsFileTime,
CreateFile2, CopyFile2 or other newer functions (see src/win7.cpp).

Never put a palette file or a `palettes.ini` in the zip: SokuLauncher updates a mod by copying the
new zip's files over the old folder, so they would overwrite a player's own (it deletes nothing
else; `configFiles` in mod.json, i.e. `PaletteShare.ini`, is kept). Removing the mod deletes the
whole folder, which is why the library lives in `<game folder>\PaletteShare\palettes`.

`dist\PaletteShare-<version>.zip` contains `PaletteShare\PaletteShare.dll`, `mod.json`,
`PaletteShare.ini`, `README.md`, `LICENSE` (MIT) and `THIRD-PARTY-NOTICES.txt` (the MIT notices of
SokuLib and nlohmann/json, which are compiled into the DLL) and `WHERE-ARE-MY-PALETTES.txt`, the
same layout as other SokuLauncher mods. The palette folders are created in the game folder at the
first start, not shipped.

## Before 1.0.0 (needs a person, not a script)

0. **CHANGELOG**: replace "not released yet" with the release date.

1. **Two-PC test over the internet**: two players, ideally one through Autopunch/a relay, both
   with a custom palette. Check that both see both palettes, Ctrl+P works, and a match against
   someone without PaletteShare is unaffected. Set `[Debug] Log=1` on both sides to get
   `PaletteShare.log`.
2. **Public repo**: push to GitHub (e.g. Aneroze/SokuPaletteShare) with a release containing the
   zip.
3. **SokuLauncher listing**: a PR to `soku-launcher-modpacks/main-source` adding
   `modules/PaletteShare/mod.json` and `versions/<version>/version.json` (see the palette-picker
   entry there for the format), pointing at the GitHub release zip.
4. **Packet type**: tell the giuroll and SokuLobbies developers (and add to SokuLib's
   `PacketType` list) that PaletteShare uses UDP packet type `0xA7` with the magic `PSHR`, so no
   other mod picks the same type.
