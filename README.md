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
mod replaces in your game is marked "(mod)" in the list, and its panel shows **Mod** (your game's
colors, what PaletteShare users see) next to **Original NN** (what players without palette mods
see). Each such slot also gets a **Mod NN** entry that works like your own palettes: its fallback
starts as that same slot (so picking it changes nothing), and you can give it another one, e.g. to
show players without PaletteShare a different normal palette. The random-select key on a "(mod)"
slot jumps to its Mod NN entry and edits its fallback. The palette menu lists these entries too.

Each palette's **fallback** is the normal slot (0-7) your game actually sends. It's what players
without PaletteShare, spectators and replays see, and the picker always shows it ("(default)" when
it follows the character default). Changes are saved right away to the folder's `palettes.ini`.
The picker remembers your last choice per character.

## Controls and settings (`PaletteShare.ini`)

- **Ctrl+P** toggles your opponent's custom palette on and off at any time, including mid-match.
  Use it if an opponent's palette is hard to look at. Your choice stays until you toggle again or
  restart the game. The key is configurable under `[Hotkey]`.
- `[Display] ShowOpponentPalettes=0` starts with opponents' custom palettes hidden.

## Safety

- Received palettes only change colors. Each color keeps exactly the transparency of your own
  normal palette for that slot, so a palette can't make a character invisible or see-through, or
  draw solid boxes around sprites.
- Packets are size-checked, and every palette is verified against its SHA-256-based hash before
  use.
- Players without PaletteShare ignore its packets (the game drops packet types it doesn't know).

## Current limits (planned for 1.1)

- **Spectators** see the normal palettes.
- **Replays** show the normal palettes.
- The opponent's custom palette appears in the character-select preview only once it has arrived.

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
