# PaletteShare

A Touhou Hisoutensoku (th123 1.10a) mod: online, you see your opponent's **custom palette** and
they see yours. Both players need the mod; without it on both sides nothing changes.

## Quick start

1. Copy the `PaletteShare` folder from the zip into `modules\` and enable it in SokuLauncher or
   SWRSToys. Windows 7 and later, and Wine/Proton.
2. Put palette files (`.pal`, `.act`, `.bmp`) in `modules\PaletteShare\palettes\<character>\`.
3. At character select, in the palette step, **Up/Down** scroll through the normal palettes and
   yours. Players without the mod see the palette's **fallback** slot instead.

**Ctrl+P** hides or shows your opponent's custom palette, at character select and mid-match.

Palettes you already use also get shared: shady-loader packages, edited `th123*.dat` files,
palette-picker, and 32-bit palettes with PaletteTrueColors (opponents without it get 16-bit).

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

### Palette mods (shady-loader packages)

Slots a palette mod replaces in your game show as "Default 03 (mod)" in the list and "Mod 03" in the
menu. They work like your own palettes, with that same slot as the fallback unless you change it.
The fallback preview always shows the game's original colors, since that's what players without
palette mods see. `[Picker] ShowOriginalPalettes=1` also lists the original colors as "Original 03".

### Mirror matches

Same rule as the game: a palette the other side already uses is skipped. If your palette's fallback
slot is taken, the next slot is used for that match only, and the panel says so.

### Supported files

- `.pal`: the game's format (513 or 1025 bytes, e.g. a shady-loader `paletteNNN.pal`), the old
  Palette Editor format (1024 bytes), JASC-PAL (GIMP, Aseprite) and RIFF PAL.
- `.act`: Adobe color table (Photoshop).
- `.bmp`, uncompressed: an indexed image's color table, or the first 256 pixels of a 16/24/32-bit
  image. palette-picker swaps red and blue in 24-bit BMPs; PaletteShare doesn't.

Files that can't be read are counted in the picker ("N files skipped") and listed with the reason in
the palette menu.

### Backup details

The backup is updated at startup and whenever a character's palettes change. It includes
`palettes.ini`, which holds the fallbacks. After a reinstall, the first start copies it back, and
newer files win. Palettes you delete are moved to `backup\_removed\`, never deleted. To remove them
for good, delete the `backup` folder too. Updating the mod never touches your palettes. If the game
folder can't be written to, there's no backup, and the log says so. Libraries of earlier test builds
(`PaletteShare\palettes` in the game folder) are brought over once and become the backup.

## Replays

Saved replays keep both sides' custom palettes, about 1 KB each, and show them again when watched
with PaletteShare. By default they're stored in `<replay name>.meta.json` next to the `.rep`; share
both files. `[Replay] Storage=rep` appends them to the `.rep` itself instead; the game ignores the
extra data. Watching reads both. The `.meta.json` has one key per mod, so other mods can use it too.

## Spectators

Spectators with PaletteShare see both players' custom palettes, with the same Ctrl+P toggle and
transparency check. A spectator's game watches through one of the players (often the client, not
the host) or through another spectator, and gets the palettes from there, so that one needs
PaletteShare too.

## Settings (`PaletteShare.ini`)

| Setting | |
|---|---|
| `[Hotkey] Modifier`, `Key` | the toggle hotkey (default Ctrl+P). Remove `Key` for no hotkey, `Modifier` for the key alone |
| `[Display] ShowOpponentPalettes=0` | start with opponents' palettes hidden |
| `[Picker] ShowOriginalPalettes=1` | list original colors of slots a palette mod replaces |
| `[Replay] Storage=sidecar\|rep` | where replay palettes go |
| `[Debug] Log=1` | `PaletteShare.log` (max 2 MB; previous run in `PaletteShare.prev.log`). Includes opponents' IP addresses. Send both logs with bug reports. |

Testing only, under `[Debug]`:
- `ForcePaletteLeft=<hash>` / `ForcePaletteRight=<hash>` show a cached palette (`cache\<hash>.pal`)
  on that side, also offline.
- `HotkeyIgnoresFocus=1` lets the hotkey work without the game focused.

## Safety

- Received palettes only change colors. Transparency always comes from your own palette, so nobody
  can make a character invisible or draw boxes around sprites.
- Packets are size-checked. Each palette is verified against its hash. At most 256 new palettes per
  character select, only from your actual opponent.
- Players without the mod ignore its packets.
- If another mod already patched a spot PaletteShare needs, that feature turns off and the log says
  which, so the other mod keeps working.

## Limits

- With palette-picker loaded, its picker is used instead of PaletteShare's. Palettes are still
  shared.
- Soku2 and other added characters are left alone.
- The opponent's palette shows in their preview once it arrives, usually within a fraction of a
  second.
- Received palettes are cached in `modules\PaletteShare\cache\` (about 0.5 KB each, at most 2000).

## Build

```
git clone --recurse-submodules https://github.com/Aneroze/SokuPaletteShare
build.bat
```

Needs Visual Studio (x86 C++ tools), CMake 3.20+ and Ninja. Output: `build\PaletteShare.dll`.
`tools\test.bat` runs the tests. SokuLib is a submodule on its `dev` branch. See
[docs/DESIGN.md](docs/DESIGN.md) for how it works.
