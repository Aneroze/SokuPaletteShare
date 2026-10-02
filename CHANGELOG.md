# Changelog

## Unreleased

- Own palette picker (first version): palettes from `palettes/<character>/`, scrolled with Up/Down in
  the palette step. Each palette has an explicit fallback slot (palettes.ini, per-character default
  0) that the picker shows and steers the game to, so players without PaletteShare see exactly that
  slot. Confirm waits until the game is on the fallback slot. Last choice remembered per character.
  Turned off when palette-picker is loaded.
- Picker layout: a compact panel next to the game's palette preview with the list, "You" and "Others"
  previews (what players without PaletteShare see), the fallback and the opponent status.
- The random-select key (A on the default keyboard) in the palette step edits the highlighted
  palette's fallback in place (Up/Down, A save, X cancel). Hints show the player's own key bindings
  (key names, or PadN on a gamepad) instead of the game's button letters.
- Palette menu (Profile, deck construction, a character, C): every palette's fallback, the character
  default, large previews, and a shortcut to the palette folder.
- The picker steps aside while the game's submenus are open over the select screen.
- An opponent palette identical to the one we'd show anyway is no longer treated as custom.
- Palette files: every `.pal` palette-picker reads (game format, or the old Palette Editor's 256
  colors, with or without the character name after them), JASC-PAL, RIFF PAL, `.act` (with color
  count), and indexed or 16/24/32-bit `.bmp`. Files are size-capped and bounds-checked.
- Files that can't be read are no longer silently ignored: the picker shows "N files skipped" and
  the palette menu lists them with the reason.
- The palette folders of all 20 characters are created at startup.
- Fixed: a held confirm key chose the palette instantly, and a held arrow scrolled every frame.
- Fixed: previews swayed left and right while animating (e.g. Reisen).

## 0.9.0 (2026-10-01)

First feature-complete version for netplay (release candidate; still needs a test between two real
PCs over the internet before 1.0.0).

- The opponent sees your custom palette and you see theirs, when both run PaletteShare. Palettes
  are captured from what the game actually shows (shady-loader packages, edited data files,
  palette-picker, PaletteTrueColors 32-bit palettes), sent during character select, cached by
  hash, and applied when the characters load.
- A palette that arrives after the opponent's character loaded is applied mid-match.
- Ctrl+P toggles the opponent's custom palette on/off at any time (configurable).
- Received palettes keep the transparency of your own palette for that slot (no invisible or
  see-through characters, no solid boxes).
- Opponents without the mod are unaffected; the handshake gives up after ~20 s.
- Not yet: spectators and replays show the normal palettes.

## 0.1.0 (2026-09-30)

Skeleton: palette load hooks, side detection, palette cache, debug palette forcing.
