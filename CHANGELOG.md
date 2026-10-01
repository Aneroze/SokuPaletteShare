# Changelog

## Unreleased

- Own palette picker (first version): palettes from `palettes/<character>/`, scrolled with Up/Down in
  the palette step. Each palette has an explicit fallback slot (palettes.ini, per-character default
  0) that the picker shows and steers the game to, so players without PaletteShare see exactly that
  slot. Confirm waits until the game is on the fallback slot. Last choice remembered per character.
  Turned off when palette-picker is loaded.
- An opponent palette identical to the one we'd show anyway is no longer treated as custom.

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
