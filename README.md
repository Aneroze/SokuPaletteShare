# PaletteShare

A Touhou Hisoutensoku (th123 1.10a) SWRSToys mod for seeing other players' custom palettes
online. Work in progress; see [docs/DESIGN.md](docs/DESIGN.md) for how it works and the roadmap.

Current state (0.1.0): hooks the character palette load, identifies the side and slot, stores
every loaded palette in `cache\<hash>.pal`, and can force a cached palette on a side for testing
(`[Debug] ForcePaletteLeft/Right` in `PaletteShare.ini`). Nothing is shared yet.

## Build

```
git clone --recurse-submodules <repo>
build.bat
```

Needs Visual Studio (x86 C++ tools), CMake 3.20+ and Ninja. Output: `build\PaletteShare.dll`.
SokuLib is a submodule pinned to its `dev` branch.

## Install

Copy `PaletteShare.dll`, `mod.json` and `PaletteShare.ini` into `modules\PaletteShare\` and
enable it in the mod loader.
