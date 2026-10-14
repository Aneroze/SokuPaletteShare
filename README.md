# PaletteShare

A Touhou Hisoutensoku (th123 1.10a) mod: online, you see your opponent's **custom palette** and
they see yours, as long as both of you have this mod.

## Quick start

1. Copy the `PaletteShare` folder from the zip into `modules\` and enable it in SokuLauncher or
   SWRSToys. Windows 7 and later, and Wine/Proton.
2. Put palette files (`.pal`, `.act`, `.bmp`) in `modules\PaletteShare\palettes\<character>\`.
3. At character select, in the palette step, **Up/Down** scroll through the normal palettes and
   yours. Players without the mod see the palette's **fallback** slot instead.

## Features
- Other players with the mod see your custom palettes, of course.
- In-game palette picker, so you don't need to rely on other palette loading methods.
	- Compatibility with shady-loader packages -- if you already have palettes loaded using it, PaletteShare will pick them up.
	- Ability to choose a fallback from among the default palettes so you control what people without the mod see.
- Use **Ctrl+P** to toggle the opponent's custom palette, in case they're clowning too hard.
- Opponents' custom palettes are saved to a folder, ready to copy into your own palettes.
- Spectator support.
- Persists in replays using a sidecar .json file.

## Your palettes

- **Folders** use the game's internal character names: `udonge` is Reisen, `utsuho` Okuu, `chirno`
  Cirno, `meirin` Meiling. The file name is the palette's name. New files show up the next time you
  reach that character's palette step.
- **Fallback**: each palette has the normal slot (0-7) the game actually sends. Players and
  spectators without PaletteShare see that slot. The default is slot 0.
- **Backup**: removing the mod in SokuLauncher deletes its folder, so PaletteShare keeps a copy in
  `PaletteShare\backup\` in the game folder. Reinstall the mod and your palettes come back.

### At character select

Keys are the default keyboard ones; the on-screen hints show your own.

- **Up/Down** scroll the list. The panel shows **Custom** (what PaletteShare users see) and
  **Fallback NN** (what others see).
- **S** (card switch) changes the highlighted palette's fallback: Up/Down pick a slot, S or Z saves,
  X cancels. `[Picker] FallbackButton` picks another button. A still picks a random deck and
  confirms, with your palette.
- Offline, both sides get the picker (player 2 in VS Player, or player 1 for the CPU or the
  Practice dummy). The last choice is remembered per character.

### The palette menu

Profile → Deck Construction → a character → **C**. It lists that character's palettes with large
previews. Left/Right change the fallback, C resets it to the character default. The "Character
default" row sets that default. "Game palettes" set to Hidden leaves the game's 8 palettes out of
that character's list at character select, so it only scrolls through your own (and slots a palette
mod replaces); while you have none, the game's are listed anyway. "Open palette folder" opens the
folder.

### Supported files

- `.pal`: the game's format (513 or 1025 bytes, e.g. a shady-loader `paletteNNN.pal`), the old
  Palette Editor format (1024 bytes), JASC-PAL (GIMP, Aseprite) and RIFF PAL.
- `.act`: Adobe color table (Photoshop).
- `.bmp`, uncompressed: an indexed image's color table, or the first 256 pixels of a 16/24/32-bit
  image. palette-picker swaps red and blue in 24-bit BMPs; PaletteShare doesn't.

Palette names show in your game's language: Japanese without a translation mod, and with
th123intl the language it's set to (Chinese names in Chinese). Characters that language can't
show appear as `?`.

Files that can't be read are counted in the picker ("N files skipped") and listed with the reason in
the palette menu.

## Replays

Saved replays keep both sides' custom palettes, about 1 KB each, and show them again when watched
with PaletteShare. They're stored in `<replay name>.meta.json` next to the `.rep` (share both files);
the `.rep` itself stays exactly as the game wrote it. The `.meta.json` has one key per mod, so other
mods can use it too. `[Replay] SaveMetadata=0` turns it off.

## Opponents' palettes

Every custom palette an opponent plays with is saved to `PaletteShare\saved\` in the game folder
(next to the backup, so removing the mod keeps them), as `<character>-<profile>-NN.pal`: the game's
character name, the opponent's profile name, and a number counting that player's palettes for that
character, e.g. `udonge-Quosu-01.pal`. A palette already saved for that character and player isn't
saved again. What's saved is what you saw, so copy a file into `palettes\<character>\` to use it.

With `[Save] AutoSave=0` nothing is saved by itself; set the save hotkey (`SaveModifier`/`SaveKey`
under `[Hotkey]`, shipped off; uncomment both for Ctrl+S) to save the opponent's palette when you
want, at character select or in the match.

## The site (soku.cleista.cc)

When you play online, PaletteShare tells [soku.cleista.cc](https://soku.cleista.cc) which palette
you show, so the site's live rooms and replays show it too: the game's random seed, your character
and palette slot, and your palette if it isn't the game's own (the same data it writes into replay
sidecars). Only your own side: your opponent's palette is theirs to send, so a player without
PaletteShare, or with `[Site] Upload=0`, never appears. Nothing is sent offline, while spectating, in
replays or when you show the game's own palette. Like any website, the site sees your IP address,
and since it also lists the match, it can tell which player you are. `[Site] Upload=0` turns it off.

## Settings (`PaletteShare.ini`)

| Setting | |
|---|---|
| `[Hotkey] Modifier`, `Key` | the toggle hotkey (default Ctrl+P). Remove `Key` for no hotkey, `Modifier` for the key alone |
| `[Display] ShowOpponentPalettes=0` | start with opponents' palettes hidden |
| `[Picker] ShowOriginalPalettes=1` | list original colors of slots a palette mod replaces |
| `[Picker] FallbackButton` | the button that changes a palette's fallback at character select: `Switch` (card switch, default), `Spell`, `D` or `None` |
| `[Save] AutoSave=0` | don't save opponents' palettes by themselves |
| `[Replay] SaveMetadata=0` | don't save custom palettes with replays (no `.meta.json`; they play back with the normal palettes) |
| `[Hotkey] SaveModifier`, `SaveKey` | save the opponent's palette now (default none; the shipped ini has Ctrl+S commented out) |
| `[Site] Upload=0` | don't tell soku.cleista.cc which palette you show online (see above) |
| `[Debug] Log=1` | `PaletteShare.log` (max 2 MB; previous run in `PaletteShare.prev.log`). Includes opponents' IP addresses. Send both logs with bug reports. |

Testing only, under `[Debug]`:
- `ForcePaletteLeft=<file>` / `ForcePaletteRight=<file>` show a palette file (absolute, or relative
  to the mod folder) on that side, as if the opponent had sent it, also offline.
- `HotkeyIgnoresFocus=1` lets the hotkeys work without the game focused.

Testing only, under `[Site]`: `Url=` sends the reports elsewhere (e.g. a local test server).

`IniVersion` records the version that last wrote the ini. When you update the mod, an older ini is
rebuilt from the new one with your settings kept, so new settings show up with their comments.

## Build

```
git clone --recurse-submodules https://github.com/Aneroze/SokuPaletteShare
build.bat
```

Needs Visual Studio (x86 C++ tools), CMake 3.20+ and Ninja. Output: `build\PaletteShare.dll`.
`tools\test.bat` runs the tests. SokuLib is a submodule on its `dev` branch. See
[docs/DESIGN.md](docs/DESIGN.md) for how it works.
