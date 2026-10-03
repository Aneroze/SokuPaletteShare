# PaletteShare

A Touhou Hisoutensoku (th123 1.10a) mod that lets players see each other's **custom palettes**
online. Normally the game only sends your palette *slot number*, so your opponent sees their own
palette for that slot. With PaletteShare on both sides, each game sends the actual colors your
character is shown with, and the other game uses them.

## What you need

- Both players run PaletteShare. If your opponent doesn't, nothing changes: you both see what
  you'd see without it.
- A custom palette, set up the way you already do it. PaletteShare shares whatever palette your
  game actually shows for your character:
  - **shady-loader packages** that replace `paletteNNN.pal`,
  - **edited data files** (`th123*.dat`),
  - **palette-picker** custom palettes,
  - 32-bit palettes with **PaletteTrueColors**. Opponents without PaletteTrueColors get a 16-bit
    version.

## Your own palettes (picker)

Put palette files in `modules\PaletteShare\palettes\<character>\`. The folder name is the game's
internal one (e.g. `reimu`, `udonge` for Reisen, `utsuho` for Okuu, `chirno` for Cirno, `meirin`
for Meiling). The folders of all 20 characters are created when the game starts. The file name
(without extension) is the palette's name in the picker. New files show up the next time you open
that character's palette step; no restart needed.

Supported files:
- `.pal`: the game's own format (513 or 1025 bytes, e.g. a shady-loader `paletteNNN.pal`), the old
  Palette Editor format (256 colors in 1024 bytes, with or without the character's name appended),
  JASC-PAL (GIMP, Aseprite, Paint Shop Pro) and RIFF PAL (Microsoft).
- `.act`: Adobe color table (Photoshop).
- `.bmp`: an indexed image (its color table is the palette), or a 16/24/32-bit image whose first 256
  pixels (row by row from the top) are the colors. Uncompressed only.

Every `.pal` that palette-picker reads works here too. One difference: palette-picker reads 24-bit
BMP images with red and blue swapped, so such a file made to look right there shows those two colors
swapped here (PaletteShare reads it correctly).

A file PaletteShare can't read isn't silently ignored: the picker shows "N files skipped", and the
palette menu lists each one with the reason (e.g. "compressed BMP (save it uncompressed)").

**At character select**, in the palette step (keyboard or gamepad; offline both sides get the
picker: player 2 in VS Player, or player 1 choosing for the CPU or the Practice dummy):
- Up/Down scroll through the 8 normal palettes and your own.
- A panel shows the list and two previews: **Custom** (your palette, what PaletteShare users see)
  and **Fallback NN** (the slot players without PaletteShare see). The fallback preview shows the
  game's original colors, read from the game's own archives: if a palette mod (e.g. a shady-loader
  package) replaces that slot in your game, players without palette mods still see the original,
  and the panel says "original colors".
- The **random-select key** (**A** on the default keyboard layout, Pad3 on a gamepad) changes the
  highlighted palette's fallback: Up/Down pick a slot (the Fallback preview follows), A saves, X
  cancels. This replaces the game's random palette in that step; on the character grid the key
  still picks a random character.

Keys below are the default keyboard ones (Z confirm, X cancel, C submenu, A random). The on-screen
hints show your own bindings.

**The palette menu**: Profile → deck construction → choose a character → **C**. It lists that
character's palettes with their fallbacks and large previews ("Custom" / "Fallback"):
- Left/Right change the highlighted palette's fallback.
- C returns it to the character default.
- The "Character default" row sets the default for every palette without its own (slot 0 unless
  changed).
- "Open palette folder" opens the folder in Explorer.

**Mirror matches** (both sides the same character) work like the game's own rule: a palette the
other side already uses is skipped in the list, just as the game skips the other side's slot. A
different custom palette whose fallback slot is taken uses the next slot for that match only (the
panel says "Default 05 taken, others see 06"), and nothing is saved. The two sides never end up
looking the same.

**Palette mods** (e.g. a shady-loader package replacing `paletteNNN.pal`): a slot that a palette
mod replaces in your game is marked "(mod)" in the list (e.g. "Default 03 (mod)") and works like
your own palettes: its panel shows **Mod** (your game's colors, what PaletteShare users see) next to
**Fallback NN** (the original colors players without palette mods see). Its fallback is that same
slot unless you change it with the random-select key, e.g. to show players without PaletteShare a
different normal palette. The palette menu lists these slots as "Mod NN". With
`[Picker] ShowOriginalPalettes=1` the list also offers the game's **original colors** of each
replaced slot ("Original 03"), always sent as that slot.

Each palette's **fallback** is the normal slot (0-7) your game actually sends. It's what players
without PaletteShare, spectators and replays see, and the picker always shows it ("(default)" when
it follows the character default). Changes are saved right away to the folder's `palettes.ini`.
The picker remembers your last choice per character.

## Controls and settings (`PaletteShare.ini`)

- **Ctrl+P** toggles your opponent's custom palette on and off at any time, including mid-match.
  Use it if an opponent's palette is hard to look at. Your choice stays until you toggle again or
  restart the game. The key is configurable under `[Hotkey]`.
- `[Display] ShowOpponentPalettes=0` starts with opponents' custom palettes hidden.
- `[Replay] Storage=rep` keeps replay palettes inside the `.rep` instead of a `.meta.json` file next
  to it (see Replays).
- `[Picker] ShowOriginalPalettes=1` lists the original colors of slots a palette mod replaces
  ("Original 03"), so you can still pick them.
- `[Debug] Log=1` writes `PaletteShare.log` next to the DLL; the previous run's log is kept as
  `PaletteShare.prev.log`.

## Safety

- Received palettes only change colors. Each color keeps exactly the transparency of your own
  normal palette for that slot, so a palette can't make a character invisible or see-through, or
  draw solid boxes around sprites.
- Packets are size-checked, and every palette is verified against its SHA-256-based hash before
  use.
- Players without PaletteShare ignore its packets (the game drops packet types it doesn't know).

## Replays

When a replay is saved (by the game, or by SaveRep), PaletteShare keeps the custom palettes both
sides were shown with, about 1 KB each (2 KB for a 32-bit one), nothing if neither side had one.
Watching the replay with PaletteShare (from the replay menu or ReplayDnD) shows them again, with
the same transparency check as online and the Ctrl+P toggle. Players without PaletteShare see the
normal palettes.

Where they're kept is set by `[Replay] Storage`:
- `sidecar` (default): in `<replay name>.meta.json` next to the `.rep`, which stays exactly as the
  game wrote it. To share a replay with its palettes, share both files.
- `rep`: appended to the `.rep` itself, after the game's data (the game ignores it), so the one file
  carries them.

Watching always looks in both places. The `.meta.json` file is plain JSON with one key per mod
(PaletteShare uses `"PaletteShare"`), so other mods can keep their own data in the same file.

## Current limits (planned for 1.1)

- **Spectators** see the normal palettes.
- The opponent's custom palette appears in the character-select preview once it has arrived
  (usually a fraction of a second after they scroll to it).

## Install

Copy `PaletteShare.dll`, `mod.json` and `PaletteShare.ini` into `modules\PaletteShare\` in your
game folder and enable the mod in the mod loader (SokuLauncher or SWRSToys). Received palettes
are kept in `modules\PaletteShare\cache\` (about 0.5 KB each).

## Build

```
git clone --recurse-submodules <repo>
build.bat
```

Needs Visual Studio (x86 C++ tools), CMake 3.20+ and Ninja. Output: `build\PaletteShare.dll`.
SokuLib is a submodule on its `dev` branch. See [docs/DESIGN.md](docs/DESIGN.md) for how it works.
