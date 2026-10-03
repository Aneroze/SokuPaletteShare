# Changelog

## 1.0.0 (not released yet: needs the two-PC test, see docs/RELEASING.md)

- Own palette picker (first version): palettes from `palettes/<character>/`, scrolled with Up/Down in
  the palette step. Each palette has an explicit fallback slot (palettes.ini, per-character default
  0) that the picker shows and steers the game to, so players without PaletteShare see exactly that
  slot. Confirm waits until the game is on the fallback slot. Last choice remembered per character.
  Turned off when palette-picker is loaded.
- Picker layout: a compact panel next to the game's palette preview with the list, "Custom" and "Fallback"
  previews (the fallback is what players without PaletteShare see), the fallback and the opponent
  status.
- The random-select key (A on the default keyboard) in the palette step edits the highlighted
  palette's fallback in place (Up/Down, A save, X cancel). Hints show the player's own key bindings
  (key names, or PadN on a gamepad) instead of the game's button letters.
- Palette menu (Profile, deck construction, a character, C): every palette's fallback, the character
  default, large previews, and a shortcut to the palette folder.
- The picker steps aside while the game's submenus are open over the select screen (but not for the
  Vs Network menu that stays open under it online).
- The picker works with a gamepad, and offline for both sides: player 2 in VS Player, and player 1
  choosing the CPU's or the Practice dummy's palette. Player 2's last choice is remembered
  separately (`[last2p]` in palettes.ini).
- An opponent palette identical to the one we'd show anyway is no longer treated as custom.
- Palette files: every `.pal` palette-picker reads (game format, or the old Palette Editor's 256
  colors, with or without the character name after them), JASC-PAL, RIFF PAL, `.act` (with color
  count), and indexed or 16/24/32-bit `.bmp`. Files are size-capped and bounds-checked.
- Files that can't be read are no longer silently ignored: the picker shows "N files skipped" and
  the palette menu lists them with the reason.
- The palette folders of all 20 characters are created at startup.
- Fallback previews show the game's original colors for the slot, read straight from its .dat
  archives, so a palette mod replacing that slot in your own game no longer misleads you. The panel
  says "original colors" when your game shows the slot differently.
- Mirror matches follow the game's rule: the other side's palette is skipped in the list, and a
  taken fallback slot moves to the next one for that match ("Default 05 taken, others see 06").
  Before, the picker probed for the taken slot (repeated menu sounds) and could still give both
  sides the same custom palette. A load-time check makes side 1 yield if they'd still match.
- Fixed: a held confirm key chose the palette instantly, and a held arrow scrolled every frame.
- Fixed: previews swayed left and right while animating (e.g. Reisen).
- Palette mods (shady-loader packages, data files): slots they replace are marked "(mod)" instead of
  "same palette for everyone" and work like custom palettes, with "Mod" / "Fallback NN" previews and
  a fallback of their own (`[*mod NN]` in palettes.ini, the same slot by default).
- `[Picker] ShowOriginalPalettes=1` also lists the game's original colors of replaced slots
  ("Original NN", always sent as that slot).
- Protocol version 2: palette offers are numbered. Fixed: the opponent sometimes loaded with their
  own palette for your fallback slot instead of your custom one (about 1 match in 20 online), when an
  offer made while your palette wheel was moving arrived after the final one.
- Fixed: while the palette wheel steps towards a palette's slot, the previews (yours and the
  opponent's) flashed the slots it passed on the way.
- Game-format `.pal` files with a few extra bytes at the end (515-byte shady-loader palettes) are
  read.
- The previous run's log is kept as `PaletteShare.prev.log`.
- Online: a direction pressed right after confirming no longer cancels the confirmed custom palette
  (the list and the game's wheel stay still until the game applies the confirm); a newer palette
  from the opponent replaces one already shown mid-match.
- HELLO carries the supported protocol range and the mod version, so later versions can still talk
  to 1.0.0; only the game's actual opponent is accepted.
- Limits on what a peer can send: character and slot checked, at most 32 new palettes per session;
  the cache keeps the palettes of the session in memory and trims itself past 2000 files.
- palettes.ini is UTF-16 (any file name survives) and palettes use `[palette:<name>]` sections
  (old `[<name>]` sections are still read).
- palette-picker is detected once every mod is loaded, whatever the load order.
- Ctrl+P also works at character select. The picker reloads after the deck screen's palette menu.
- Log lines keep non-ASCII names; the network thread stops at exit.
- The release zip contains the README, LICENSE (MIT) and the third-party notices.
- Replays: saving a replay keeps both sides' custom palettes; watching it with PaletteShare shows
  them. Stored as per-replay metadata (JSON, one key per mod) in a `<replay>.meta.json` sidecar by
  default, or with `[Replay] Storage=rep` appended to the `.rep` after the game's data (the game
  ignores it). Watching reads both. Your own side's palette is kept whatever made it (the picker,
  palette-picker, a palette mod, edited data files). Malformed replay data is ignored.
- Character select shows the opponent's palette as soon as it arrives. Before, their preview was
  only rebuilt when their palette wheel moved, usually just before their palette arrived (and never
  for a custom palette on the slot they were already on).

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
