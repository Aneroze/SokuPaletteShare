# Releasing

## Build the release zip

```
build.bat
pwsh -File tools\package.ps1
```

Never put a palette file or a `palettes.ini` in the zip: SokuLauncher updates a mod by copying the
new zip's files over the old folder, so they would overwrite a player's own (it deletes nothing
else; `configFiles` in mod.json, i.e. `PaletteShare.ini`, is kept). Removing the mod deletes the
whole folder, which is why the library lives in `<game folder>\PaletteShare\palettes`.

`dist\PaletteShare-<version>.zip` contains `PaletteShare\PaletteShare.dll`, `mod.json`,
`PaletteShare.ini`, `README.md`, `LICENSE` (MIT) and `THIRD-PARTY-NOTICES.txt` (the MIT notices of
SokuLib and nlohmann/json, which are compiled into the DLL), the same layout as other SokuLauncher
mods, plus an empty `PaletteShare\palettes\<character>\` folder for each of the 20 characters.

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
