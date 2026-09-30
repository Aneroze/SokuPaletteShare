# PaletteShare design notes

Goal: players who both run PaletteShare see each other's custom palettes online, and so do
spectators and replay viewers. Anyone without the mod keeps seeing the normal palette for the
slot, so nothing changes for them.

Written from scratch. palette-picker (github enebe-nb/palette-picker, no license) was read for
inspiration only. None of its code is copied.

## How palettes work in th123 1.10a

- A palette is 256 colors: 16-bit A1R5G5B5 (512 bytes), or 32-bit (1024 bytes) with
  PaletteTrueColors. Files are `data/character/<name>/paletteNNN.pal`: one bpp byte followed by
  the colors.
- Palettes are applied once, when the character loads. The sprites are 8-bit indexed and get
  converted to textures through the single global palette `0x896B88`
  (`SokuLib::Palette::currentPalette`), so the colors are baked in at load time. Rollback and
  per-frame code never touch palettes.
- Netplay and replays only carry the slot number (`skinId` / `paletteId`), never the colors.

Load path (see `src/hooks.cpp`):

| Address | What |
|---|---|
| `0x46DE31` | character factory: `call 0x46C0B0` (thiscall on the new `v2::Player`, no stack args), **hooked** |
| `0x46C0F7` | `call 0x4679E0(name, paletteId, ...)`: loads the character's pattern |
| `0x467AEB` | sprintf `data/character/%s/palette%03d.bmp` (Okuu may use `...%03db.bmp`) |
| `0x467B7D` | `call 0x408BE0`: thiscall (ecx = `0x8A0048`), args `(name, Palette*, 16)`, `ret 0xC`, **hooked** |
| `0x41FFB7` | character-select preview uses the same loader (not hooked yet) |

The side comes from `v2::Player::teamId` (+0x350). The palette slot is `v2::Player::paletteId`
(+0x351). Both were verified live: Reimu left = side 0 slot 0, Sanae right = side 1 slot 1.

`0x408BE0` swaps `.bmp` for `.pal` and reads through the game's file layer, so palettes replaced
by shady-loader or by editing the data archives are what we see and hash. palette-picker patches
0x467AEB..0x467AFF and jumps over 0x467B7D for its custom palettes, so the two mods don't mix. We
log a warning when it's loaded.

## Core model

- **Palettes are content-addressed:** hash = first 16 bytes of SHA-256 over the serialized
  palette (bpp byte + colors).
- **One cache for everything:** `cache/<hash>.pal`. Every palette the game loads is stored, and
  every palette received will be too. Loads re-verify the hash.
- **We never need to know which palettes are "custom".** We hash what the game actually loaded
  for the local player's character and share that.

## Milestones

- [x] **M0 skeleton**: CMake + SokuLib (dev); hooks on character load and palette load;
  side/slot detection; cache; debug `ForcePaletteLeft/Right` to exercise the swap. Verified in
  a practice match: forcing Reimu's palette onto Sanae recolors only Sanae.
- [ ] **M1 local palettes**: custom palettes from a folder (`.pal` / `.act` / `.bmp`) and a picker
  at character select, so there's something custom to share. Import formats like palette-picker's.
- [ ] **M2 netplay**: a custom packet type over the game's socket (sendto/recvfrom hooks, as
  NetInfo does); handshake first so vanilla or giuroll peers never see unknown packets; offer
  the hash of the palette we loaded, fetch the bytes if unknown (~513 B, one UDP packet); apply
  them to the remote side at 0x467B7D; fall back to the normal slot if nothing arrived in time.
  Sanitize received palettes: keep index 0 transparent and force every other index opaque.
  Add a "show opponents' palettes" toggle.
- [ ] **M3 spectators**: the host sends both palettes to modded spectators; spectators that relay
  to other spectators (REDIRECT / CHAIN packets) pass them on from their cache.
- [ ] **M4 replays**: after the game saves a `.rep`, append a trailer (`SOKUPAL\0`, version,
  per-side palettes, CRC). On playback, read it and apply. Backup: a local index from
  replay → hashes.
- [ ] **M5 polish**: opponent's palette in the character-select preview (0x41FFB7);
  PaletteTrueColors interplay; Soku2 / CharacterEngine characters.

## Verified facts / tests

- **.rep trailer** (2026-09-30): the game reads the input count at offset 0x77 and stops at
  `0x7B + count*2`. Trailing data is ignored. A replay with a 1042-byte trailer played exactly
  like the original through ReplayDnD (same tick total, 0 HP mismatches over the full match) with
  giuroll, ReplayInputView+, ReplayHudExtras, SaveRep and ReplayLabelEx loaded. The in-game
  Replay menu lists and loads it. Full playback from the menu is not confirmed yet, because the
  harness gets stuck at frame 0 on that path for every replay, trailer or not.

- **Netplay, 2 local instances** (2026-09-30; host and guest, both with
  giuroll, driven by a small test harness):both games identify the host's
  character as side 0 and the guest's as side 1, and compute the same hashes for the vanilla
  palettes. Test palettes from `tools/make_test_palettes.py`: "blue Reimu" (red and blue swapped)
  `28903696204832a202b35a5efcfffb57`, "inverted Sanae" (color negative)
  `5aca8185dbf2e378412d158264d6781e`.
  - Run 1 (today's behavior, each player forces only their own side): the host sees blue Reimu +
    normal Sanae; the guest sees normal Reimu + inverted Sanae. This is the gap M2 closes.
  - Run 2 (both games force both sides, standing in for M2): both screens show blue Reimu +
    inverted Sanae, including palette-colored effects (Reimu's gohei streamers). The match
    played normally.

## Open questions

- Does the game reload characters on a rematch with the same characters, or reuse them? This
  decides whether the swap must happen earlier.
- Per-node spectator limit and when the host redirects.
- Do vanilla/giuroll peers ignore unknown packet types? The handshake-first design avoids
  depending on it.
