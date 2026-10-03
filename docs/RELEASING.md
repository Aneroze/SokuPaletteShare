# Releasing

## Build the release zip

```
build.bat
powershell -File tools\package.ps1
```

`dist\PaletteShare-<version>.zip` contains `PaletteShare\PaletteShare.dll`, `mod.json` and
`PaletteShare.ini`, the same layout as other SokuLauncher mods, plus an empty
`PaletteShare\palettes\<character>\` folder for each of the 20 characters.

## Before 1.0.0 (needs a person, not a script)

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
