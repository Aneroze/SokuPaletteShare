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
- Characters are created on the game's loader thread, one at a time (`0x46E6D0` → `0x46E5C0` →
  `GameDataManager::createPlayer` `0x46DA40`), once per match. A rematch creates them again.

Hooks (see `src/hooks.cpp`):

| Address | What |
|---|---|
| `0x46DE31` | factory `call 0x46C0B0` (thiscall on the new `v2::Player`): remember which player is loading (thread-local) |
| `0x467B82` | right after the palette load (`0x467B7D` `call 0x408BE0`). palette-picker jumps straight here with its own palette, so both paths meet here. Capture our own palette / apply the opponent's |
| `0x467C43` | `TextureManager::loadTexture` per image: record (handle, path) per side, for the mid-match toggle |
| `0x41FFBD` | character-select preview, `call getCharName` right after the preview palette load (palette-picker's preview path also meets here; edi = player, `[esp+140h]` = slot): offer our palette as we browse, show the opponent's |
| battle scene vtables | `onRender` of Battle / BattleServer / BattleClient / BattleWatch: hotkey, late palettes |
| IAT `0x857280` | `recvfrom`: take our packets out before the game (and giuroll at `0x41DAEA`) sees them |
| IAT `0x857290` | `sendto`: we send through it, so relay/rewrite mods (Autopunch, InGameHostlist) see our packets |

The side comes from `v2::Player::teamId` (+0x350), the slot from `paletteId` (+0x351). The host is
always side 0 (`mainMode` VSCLIENT = 4), the client side 1 (VSSERVER = 5).

## Core model

- **Palettes are content-addressed:** hash = first 16 bytes of SHA-256 over the serialized
  palette (bpp byte + colors). `cache/<hash>.pal` holds every palette we load or receive. Loads
  re-verify the hash.
- **We never need to know which palettes are "custom".** We share whatever the game actually
  shows for the local player's character.
- **Per side, at load:** `vanilla` = what this game would show, and `custom` = the opponent's
  palette (or a debug-forced one) after `lockTransparency`. The hotkey switches between the two by
  re-creating that side's textures through the game's loader and swapping the D3D textures behind
  the existing handles. 591–826 textures take ~31 ms. Game state isn't touched, so it's
  rollback-safe.

## Protocol (v2, `src/protocol.hpp`)

`[0xA7]["PSHR"][version][kind][payload]` on the game's UDP socket. The game handles types 1..14
only and drops the rest (`0x41DB04`), so peers without the mod ignore ours. Other mods' ranges:
SokuRoll 0x10–0x17, DesyncDetector 0x20–0x21, giuroll 0x6B–0x80, Soku2 0xD3. The game's receive
buffer is 1024 bytes, so palettes go in chunks of ≤900 bytes (16-bit: 1 packet, 32-bit: 2).

- **HELLO** `{role, isReply, minVersion, maxVersion, modVersion}`: the client sends it to the host
  every 500 ms (up to 40 times) when a netplay session starts; the host replies. Both talk the
  highest version in both ranges (peers that don't send a range speak just their header version),
  re-checked on every HELLO; HELLO's own layout never changes. Only the game's opponent is
  accepted: the client checks the host's address, the host an address that asked the game to play
  (INIT_REQUEST with PLAY_REQU) in the last 60 s; spectators ask to spectate and aren't counted
  (their HELLOs, role spectator, are ignored until spectator support).
- **PALETTE** `{seq, charId, slot, hash, totalSize, offset, len, bytes}`: our current palette,
  resent every 500 ms until acknowledged (max 60 times). `seq` numbers our offers, so a late or
  reordered datagram never replaces a newer offer. The receiver reassembles (max 4 in flight),
  checks charId < 256, slot < 8, the sizes and the hash, caches it (max 32 new palettes per
  session), and keeps the newest offer per character (character-select previews) and per
  character and slot (what the opponent loads with).
- **ACK** `{hash}`.

Offers are made from the character-select preview as the player browses, and again at load (a
no-op if unchanged). A palette received after the opponent's character loaded, or a newer one
replacing it, is applied mid-match by the next battle render.

## Anti-abuse

- `lockTransparency`: received colors keep, index by index, exactly the transparency of the
  palette this game would show for that slot. No invisible or translucent characters, no opaque
  background boxes.
- Hotkey (Ctrl+P by default) to hide or show the opponent's custom palette at any time, plus
  `ShowOpponentPalettes` in the ini.
- Strict packet parsing: only the two real sizes, chunks within bounds, hash verified before use.

## Milestones

- [x] **M0 skeleton** (0.1.0).
- [x] **M2 netplay** (0.9.0): exchange, palette-picker compatibility, safety, hotkey, late
  palettes. See the tests below.
- [ ] **1.0.0**: a two-PC test over the internet, then a public repo and a SokuLauncher listing
  (see `docs/RELEASING.md`).
- [x] **M1 own picker** (done for 1.0.0, 2026-10-02; Soku2/modded characters deferred indefinitely): custom palettes from a folder (`.pal` / `.act` / `.bmp`) and a picker at
  character select, replacing palette-picker. Requirement (user, 2026-10-01): **the fallback must
  be obvious.** Players without PaletteShare only get the slot number, so:
  - every custom palette is paired with an explicit fallback slot 0–7, the slot the game actually
    sends. **The player controls it directly, and it is always clearly visible** (user decisions,
    2026-10-01): no color-based auto-suggestion. A palette whose fallback was never set uses a
    predictable default, slot 0 unless the player changes the per-character default in the
    settings. It can be confirmed right away, but the picker always shows the fallback and marks
    the default ("Fallback: Default 00 (default)" vs "Fallback: Default 03"). The player can set
    it in the picker (cycling slots with the fallback preview updating). It's saved with the
    palette and can be changed at any time;
  - the picker shows the custom and fallback previews side by side ("Custom" / "Fallback 0N");
  - in netplay, live status once the handshake settles: opponent has PaletteShare → custom,
    doesn't → "they'll see Default 0N", unknown → checking. The same slot is what spectators and
    replay viewers without the mod see;
  - mirror matches: verify the game's same-slot bump rule (an offline Okuu mirror gave P2 slot 1
    with no palette chosen; palette-picker bumps 0→1, otherwise →0) and warn live when it changes
    our fallback;
  - fallback previews come from the original `.dat` palettes (not shady-loader overrides), read
    straight from the game's archives (src/original.cpp). Locally modified slots are flagged.
    Honest limit: an opponent without the mod who replaced their own slot files sees their
    version.
- [ ] **M3 spectators**: the host sends both palettes to modded spectators; spectators that relay
  to other spectators (REDIRECT / CHAIN packets) pass them on from their cache.
- [x] **M4 replays**: per-replay metadata (src/replaymeta.cpp, meant to become its own mod): a JSON
  object with one key per mod, saved after the game's writer (0x42B2D0) returns, either to a
  `<name>.meta.json` sidecar (default; merged with the keys already there, written via a temp file)
  or appended to the .rep after the game's data (`[Replay] Storage=rep`; compact JSON + footer: u32
  size, CRC-32, `SOKUMETA`). Read back after readReplay (0x42EAC0) from both places. Both functions
  are hooked at their entry (SEH prologue moved to a trampoline), which also covers SaveRep (calls
  the game's save, 0x43EBE0) and ReplayDnD (calls readReplay directly; the sidecar is found through
  the real path of the opened replay, since ReplayDnD passes an ANSI stand-in). src/replay.cpp stores
  `"PaletteShare": {"version": 1, "palettes": {"p1"/"p2": {character, slot, base64 .pal}}}`, applied
  at character load when the character and slot match. (The design came out of a discussion with
  Aristocrat: an extensible key schema that mods without knowledge of each other can share.)
- [ ] **M5 polish**: Soku2 / CharacterEngine characters; an on-screen notice for the hotkey.

## Verified facts / tests

All netplay tests used two local game instances (host and guest, both with giuroll), driven by a
small test harness, with InGameHostlist disabled so nothing was advertised publicly. Test palettes come from `tools/make_test_palettes.py`: "blue
Reimu" `28903696…fb57`, "inverted Sanae" `5aca8185…781e`, plus a 32-bit "green Reimu"
`339d4b57…fa1d`.

- **.rep trailer** (2026-09-30): the game reads the input count at offset 0x77 and stops at
  `0x7B + count*2`. Trailing data is ignored (ReplayDnD playback identical over a full match; the
  menu lists and loads it).
- **Replay palettes** (2026-10-03): netplay host (debug-forced blue Reimu on side 0 and the
  32-bit green Reimu on side 1) vs a guest without PaletteShare; closing the window made SaveRep
  save the replay. Sidecar mode: .rep byte-identical to the game's (ends at the inputs), sidecar
  with both palettes (513 B / 1025 B); with an extra foreign key added to the sidecar, playback
  through ReplayDnD (command line, force removed) showed both recorded palettes. Rep mode: +2198
  bytes after the game's data, CRC valid, no sidecar; played back with the setting on sidecar:
  read from the .rep, both palettes shown (screenshot).
- **Exchange** (2026-10-01): host = blue Reimu from a shady-loader package, guest = inverted Sanae
  from palette-picker 0.2b. Both screens show both custom palettes. With empty caches the bytes
  were transferred, reassembled and hash-checked.
- **32-bit**: the host's PaletteTrueColors palette (bpp 32, 1025 bytes) went over in 2 chunks; the
  guest without PaletteTrueColors shows it reduced to 16-bit.
- **Hotkey**: offline (debug force) inverted → normal → inverted mid-match. In netplay, the
  guest's toggle only changes the guest's screen; the match continued in sync.
- **Peers without the mod**: modded host + vanilla guest, and vanilla host + modded guest (40
  HELLOs ignored by the host's game and giuroll); both matches ran normally.
- **Lossy link**: 100 ms one way + 15% loss through `udp-delay.ts`: palettes were resent and
  arrived before the characters loaded.
- **Okuu mirror** (offline): both sides load and are identified (slots 0 and 1); the cape effect
  is intact.

- **Picker** (2026-10-01): the palette step is selection stage 1 (`Select+0x22C0/1`). Palette
  cursors are `leftPalInput` (+0x164) and `rightPalInput` (+0x18C), wheels of 8 moved by Up/Down.
  Left/Right = deck, C = profile submenu, D = random, so there's no free key. We drive the wheel
  through the local menu KeymapManager (`0x8986A8`) at the SetInputs call site `0x40A45D`: the
  decoded input at +0x38 plus the netcode bits at +0x62. One press at a time, waiting until the game
  applied it. Verified offline (Blue Reimu fallback 3 → wheel 3; Green Reimu default 0 → wheel 0)
  and in netplay: both games hold slot 3 for the host's Reimu (`0x899D15`). A guest with
  PaletteShare shows Blue Reimu; a guest without it shows vanilla palette 3 (purple). The preview
  sprite shows the highlighted custom palette (rebuilt via `0x41FE60`).
  2026-10-01 (later): the panel (300x96 at (132,44) / (132,324)) with "You"/"Others" previews.
  Previews are the select-screen `stand.pat` built under a palette (`ui::PalettePreview`). The
  select effect renderer ignores `renderInfos.scale`, so we draw the pose's `SpriteEx` ourselves:
  loadTransform, fit to a height, place by its measured bounds. The game's text renderer treats `<`
  as markup, so strings avoid angle brackets. D edits the fallback in place (user choice: both this
  and a menu). The palette menu hooks the deck construction menu's onProcess (vtable slot
  `0x859878`, character at `+0xA0`, -1 on the grid). Menus read keys through `*(0x89A394)`, i.e.
  `0x89A248`. The picker stands down while `menuManager` is non-empty (the C submenu over the
  select screen). All verified in Practice: fallback 5 → game slot 5 → match with the custom
  palette.
  2026-10-02: mirror rule (the game's: skip the other side's slot and palette, bump a taken
  fallback for that match), fallback previews from the original `.dat` archives, both sides offline
  and gamepads, netplay re-verified. Soku2/modded characters: deferred indefinitely (user).

## Open questions

- Two real PCs over the internet (NAT, Autopunch relay) — not testable from one machine.
- Both players changing palettes at the last moment before confirming: offers replace each other
  and the newest is resent, but this hasn't been exercised by a test.
- The toggle temporarily writes the global palette on the render thread; the loader thread also
  uses it. Toggles only happen in battle scenes, when the loader is idle.
- Per-node spectator limit and when the host redirects (for M3).
