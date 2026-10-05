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

- **Palettes are identified by hash:** the first 16 bytes of SHA-256 over the serialized palette
  (bpp byte + colors). Received palettes are verified against it and kept in memory, by value, per
  side (nothing on disk: a palette is about 0.5 KB, so sending it again in a later session costs
  nothing worth a cache).
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
  highest version in both ranges,
  re-checked on every HELLO; HELLO's own layout never changes. Only the game's opponent is
  accepted: the client checks the host's address, the host an address that asked the game to play
  (INIT_REQUEST with PLAY_REQU) in the last 60 s; spectators ask to spectate and aren't counted.
  A spectator HELLO (role spectator) is answered by whoever the spectator's game watches through,
  if that address asked the game to spectate (INIT_REQUEST with SPECTATE_REQU, last 30 min).
- **PALETTE** `{side, seq, charId, slot, hash, totalSize, offset, len, bytes}` (protocol v3): what
  that side is shown with. A player sends its own side's to the opponent (as the player browses,
  and again at load). Any node a spectator watches through sends it both sides': a player node its
  own palette and the opponent's raw offer, a spectator what it received, so relays work the same
  way (sent at each change and once a spectator joined). Each is resent every 500 ms until
  acknowledged (60 times to the opponent, 20 to a spectator; one `Send` record and one resend
  loop for both). `seq` numbers what the sender sends, so a late or reordered datagram never
  replaces a newer palette. The receiver accepts its opponent's side from the opponent and either
  side from the node it watches through, reassembles (max 4 in flight), checks side, charId < 256,
  slot < 8, the sizes and the hash (max 256 new palettes per character select, per match when
  spectating; a palette equal to the slot's original doesn't count), and keeps the newest per side
  and character (character-select previews) and per side, character and slot (what loads).
  Up to 16 spectators per node; when full, one that never joined makes room.
- **Spectator handshake**: HELLO (role spectator) → HELLO reply with a random u32 cookie (at most
  one reply per address per 500 ms; the same cookie until it joins) → **JOIN** `{cookie}` → palettes.
  A forged sender address never sees the cookie, so it gets nothing but small HELLO replies (no
  more than the game itself answers). A HELLO from a joined spectator starts over (new cookie).
  A player node stops updating spectators from the opponent's offers at the next character select
  (the loaded characters are history), so a lagging spectator never gets browsing palettes.
- **ACK** `{hash, seq}`: confirms that one offer. Offers in a row often share a palette (the
  wheel stepping to the fallback slot re-offers it per slot), so an ACK by hash alone could confirm
  the final offer before it was sent. The local offer is also sent once more at character load.

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
- [x] **M3 spectators** (2026-10-05): see SIDE above. Spectators are told apart by their scenes
  (LOADINGWATCH 12 / BATTLEWATCH 15; mainMode VSWATCH, subMode REPLAY, so before this they were
  taken for replay playback and got nothing). The spectator's upstream is the game's own peer
  address (`netUdp.opponent`). Verified with three local games (Copy host, CC2 client, CC3
  spectator): the host handed the spectator to the client (127.0.0.1:10802), which served both
  palettes; the spectator showed Green Reimu and inverted Sanae at load, both when watching from
  the start and when joining mid-battle. With a 4th game (CC4): the second spectator was handed to
  the first one (CC3 relayed both palettes). Through the public IP (router hairpin, peers seen at
  LAN addresses): same results. Full matches (driven to a 2-0 with the harness): both spectators
  saved `[Spectate]` replays with both palettes in the sidecar, and they play back with them.
  Spectators save a replay at the end of the match (BattleWatch 0x4288CB → 0x454240) only when the
  config byte 0x8998B3 is 0 or 2; the generic save (0x43EBE0) is for subMode 1 only.
  Found on the way: a spectator whose relay sent the palettes after its loading screen picks up
  both sides late, and the late-palette rebuild handled one side at a time (the second cancelled
  the first, which stayed on its fallback slot); now one rebuild per side.
- [x] **M4 replays**: per-replay metadata (src/replaymeta.cpp, meant to become its own mod): a JSON
  object with one key per mod, saved after the game's writer (0x42B2D0) returns, either to a
  `<name>.meta.json` sidecar (default; merged with the keys already there, written via a temp file)
  or (until 2026-10-05, git tag `rep-storage-last`) appended to the .rep after the game's data
  (`[Replay] Storage=rep`; compact JSON + footer: u32 size, CRC-32, `SOKUMETA`), dropped as rarely
  worth its code. Read back after readReplay (0x42EAC0). Both functions
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
  applied it. 2026-10-05: presses are held like a key (counting up) until the wheel moves, at most
  20 frames (the game's InputHandler `0x41FBF0` steps on 1 and repeats only past 30), then released
  for 3 frames online: a one-frame press got lost with packet loss (relay 60±60 ms, 15% loss: 6
  timeouts of 1.5 s in a 5 s hold; held: none in 23 presses). A confirm is held the same way and
  re-sent (twice at most) if the game didn't take it within 90 frames. The step's move sound
  (`push 0x27; call` at `0x4206BE`, eax = what moved, ebp = side) is muted for sides we drive; we
  play it once per list move instead (a list move whose fallback is the wheel's slot made none). Verified offline (Blue Reimu fallback 3 → wheel 3; Green Reimu default 0 → wheel 0)
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
- Per-node spectator limit and when exactly the host redirects (seen: the first spectator went to
  the client).

## Robustness

- **Patching** (src/patch.cpp): every patch checks what it replaces. A call site must hold a near
  call (another mod's call there is chained through), a replaced instruction run must match
  byte for byte, a function entry must start with the SEH prologue or another mod's jump (chained),
  and a vtable or import slot must point at code. Otherwise only that feature stays off, logged.
- **Exceptions**: every entry point the game calls (hooks, render hooks, the network thread) runs
  through `guarded` (src/guard.hpp); the game has no handler for C++ exceptions.
- **Threads**: the loader thread (character loads), the game thread (select, battle, menus), the
  game's receive thread (our recvfrom hook) and our network thread. The receive thread only parses
  and queues: sends go through an outbox, handled outside the network lock.
- **Windows 7**: hashing is built in (src/sha256.cpp); win7.asm/win7.cpp replace the three Windows
  8 imports the C++ library brings (GetSystemTimePreciseAsFileTime, CreateFile2, CopyFile2) with
  stand-ins that fall back to older functions. Check with `dumpbin /imports` after toolset updates.
- **Tests** (tests/tests.cpp, `tools\test.bat`): the game-free parts (sha256, base64, palette,
  palettefile, protocol, metaformat) plus a seeded fuzz pass over every parser.
- **Okuu's cloak** (verified 2026-10-03): at 0x467B29 the game loads `paletteNNNb` instead of
  `paletteNNN` for Okuu (character 18) when configex123.ini [etc] limit=1. The two differ only at
  color 31 (the cloak's inside: magenta, drawn over by the game, vs dark navy). Every palette we
  show on Okuu takes color 31 from the palette the viewer's game loaded (`fitToViewer` in
  hooks.cpp). Test: limit=1, Okuu's normal palette 0 forced -> shown palette identical to the
  game's own palette000b.
