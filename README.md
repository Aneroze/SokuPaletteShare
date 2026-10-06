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
- **A** (random select; Pad3 on a gamepad) changes the highlighted palette's fallback: Up/Down pick a
  slot, Z or A saves, X cancels. On the character grid, A still picks a random character.
- Offline, both sides get the picker (player 2 in VS Player, or player 1 for the CPU or the
  Practice dummy). The last choice is remembered per character.

### The palette menu

Profile → Deck Construction → a character → **C**. It lists that character's palettes with large
previews. Left/Right change the fallback, C resets it to the character default. The "Character
default" row sets that default. "Open palette folder" opens the folder.

### Supported files

- `.pal`: the game's format (513 or 1025 bytes, e.g. a shady-loader `paletteNNN.pal`), the old
  Palette Editor format (1024 bytes), JASC-PAL (GIMP, Aseprite) and RIFF PAL.
- `.act`: Adobe color table (Photoshop).
- `.bmp`, uncompressed: an indexed image's color table, or the first 256 pixels of a 16/24/32-bit
  image. palette-picker swaps red and blue in 24-bit BMPs; PaletteShare doesn't.

Files that can't be read are counted in the picker ("N files skipped") and listed with the reason in
the palette menu.

## Replays

Saved replays keep both sides' custom palettes, about 1 KB each, and show them again when watched
with PaletteShare. They're stored in `<replay name>.meta.json` next to the `.rep` (share both files);
the `.rep` itself stays exactly as the game wrote it. The `.meta.json` has one key per mod, so other
mods can use it too.

## Settings (`PaletteShare.ini`)

| Setting | |
|---|---|
| `[Hotkey] Modifier`, `Key` | the toggle hotkey (default Ctrl+P). Remove `Key` for no hotkey, `Modifier` for the key alone |
| `[Display] ShowOpponentPalettes=0` | start with opponents' palettes hidden |
| `[Picker] ShowOriginalPalettes=1` | list original colors of slots a palette mod replaces |
| `[Debug] Log=1` | `PaletteShare.log` (max 2 MB; previous run in `PaletteShare.prev.log`). Includes opponents' IP addresses. Send both logs with bug reports. |

Testing only, under `[Debug]`:
- `ForcePaletteLeft=<file>` / `ForcePaletteRight=<file>` show a palette file (absolute, or relative
  to the mod folder) on that side, as if the opponent had sent it, also offline.
- `HotkeyIgnoresFocus=1` lets the hotkey work without the game focused.

## Build

```
git clone --recurse-submodules https://github.com/Aneroze/SokuPaletteShare
build.bat
```

Needs Visual Studio (x86 C++ tools), CMake 3.20+ and Ninja. Output: `build\PaletteShare.dll`.
`tools\test.bat` runs the tests. SokuLib is a submodule on its `dev` branch. See
[docs/DESIGN.md](docs/DESIGN.md) for how it works.
