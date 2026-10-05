# Changelog

## 1.0.0 (not released yet: needs the two-PC test, see docs/RELEASING.md)

First public release. (Earlier versions were test builds; their history is in git.)

**Netplay**
- When both players run PaletteShare, each sees the other's custom palette: whatever the game
  actually shows for their character (your own palettes, shady-loader packages, edited data files,
  palette-picker, 32-bit palettes with PaletteTrueColors; opponents without PaletteTrueColors get a
  16-bit version). Players without the mod are unaffected and see your fallback slot.
- The opponent's palette shows in their character-select preview as soon as it arrives, and a
  palette that arrives after their character loaded is applied mid-match, spread over a few frames.
- Ctrl+P hides or shows the opponent's custom palette at any time, with an on-screen notice. The key
  is configurable; removing `Key` from `[Hotkey]` turns it off.
- Spectators with PaletteShare see both players' custom palettes, from whichever game they watch
  through (often the client, or another spectator relaying the match).

**Your own palettes**
- A picker in the palette step: Up/Down scroll through the normal palettes and your own `.pal`,
  `.act` and `.bmp` files from `modules\PaletteShare\palettes\<character>\`. Each palette has a
  fallback slot, always shown, which is what players without PaletteShare see; the random-select
  key edits it in place. Works with a gamepad and offline for both sides (player 2, the CPU, the
  Practice dummy); the last choice is remembered per character.
- The palette menu (Profile → Deck Construction → a character → C): fallbacks, the character default,
  large previews, files that couldn't be read (with the reason), and a shortcut to the folder.
- Slots a palette mod replaces in your game are listed as "Mod NN" entries with their own fallback;
  `[Picker] ShowOriginalPalettes=1` also lists the original colors. Fallback previews always show the
  game's original colors.
- Mirror matches follow the game's rule: a palette the other side uses is skipped, and a taken
  fallback slot moves to the next one for that match.
- The palettes are backed up to `<game folder>\PaletteShare\backup`, so removing the mod and
  installing it again brings them back; deleted ones are kept in `backup\_removed`.

**Replays**
- Saved replays (by players and spectators) keep both sides' custom palettes in a
  `<replay>.meta.json` next to the `.rep` (one key per mod, so other mods can share the file), and
  show them again when watched with PaletteShare.

**Safety and compatibility**
- Received palettes only change colors: each keeps the transparency of your own palette for that
  slot. Packets are size-checked and every palette is verified against its hash; a peer can send at
  most 256 new palettes per character select, and only your actual opponent (or, spectating, the
  game you watch through) is listened to. Spectators get palettes only after proving their address.
- Patches check what they replace; if another mod changed a spot unexpectedly, only that feature
  turns off (logged). Soku2 and other added characters are left alone. With palette-picker loaded,
  its picker is used and palettes are still shared.
- Runs on Windows 7 and later, and under Wine/Proton. Logging is on by default (2 MB per run, the
  previous run kept as `PaletteShare.prev.log`).
