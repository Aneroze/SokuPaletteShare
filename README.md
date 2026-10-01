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
