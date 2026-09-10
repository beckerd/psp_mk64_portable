# Ad hoc multiplayer (branch `adhoc`)

Goal: two to four PSPs race or battle together over ad hoc WLAN, each showing
its own player full screen instead of a split-screen quarter.

## Design: input lockstep

MK64 is a deterministic simulation driven only by controller inputs (the RNG
is a 16-bit shift register seeded at race start, the one wall-clock value the
game reads advances a fixed two ticks per frame on the port, and audio never
feeds gameplay).  So every machine runs the whole game, all players included,
and only the pads travel:

- Slot 0 is the host; it assigns slots to the peers in join order and sends a
  START packet with the slot -> MAC table.  From then on both machines count
  frames from 0 and stay frame-locked.
- Frame F consumes, for every slot, the input that slot sampled at its frame
  F - INPUT_DELAY (2 frames = 67 ms).  A machine that lacks a slot's input for
  F stalls (holds the last picture) until it arrives.
- Every packet repeats the last REDUNDANCY (8) frames of local input, so a
  lost packet costs nothing; ad hoc PDP is unreliable by design.
- Every CHECK_EVERY (30) frames a checksum of the players' state rides along.
  A mismatch is a desync: logged as `net: DESYNC at frame N` and flagged in
  `gPortNetDesync`.  This turns a determinism bug into a bisectable frame
  number instead of a mystery.
- The session starts before the game does (`port_net_boot()` runs before
  `port_game_init()`), so the menus are part of the lockstep too: the host's
  pad is pad 1, the client's pad is pad 2, and the game's own 2P menus,
  character select, pause and results just work.

Files: `src/port/net/port_net.h` (API), `lockstep.c` (session),
`net_adhoc.c` (PSP transport: one PDP socket, broadcast), `net_file.c`
(mailbox files on the shared memory stick, for PPSSPP tests).  Hooks:
`osContGetReadData` / `osContInit` in `ultra_shim.c`, the boot and the frame
loop in `psp_main.c`.  Everything is behind `-DPORT_NET` plus one transport
define.

## Role selection (v1)

Hold L while the game starts to host, R to join; or put a byte in
`data/netrole.bin` (1 host, 2 join) -- that is how the scripted tests do it.
Nothing held: single player, the network code is idle.

## Milestones

1. **Simulation lockstep** (this commit): two machines run the 2P game in
   lockstep, both still rendering the game's split screen.  Proves the
   transport, the stall logic and determinism.  Test: two PPSSPP instances
   with the file transport, checksums match for a whole race.
2. **Memory probe on the PSP** (done 2026-09-10, David's PSP, hosting):

   | step | free | largest block |
   | --- | --- | --- |
   | before the net modules | 1857 KB | 1621 KB |
   | after NET_COMMON + NET_ADHOC | 1649 KB | 1413 KB |
   | after sceNetInit (128 KB pool) + adhoc + adhocctl init | 1505 KB | 1285 KB |
   | after adhocctl connect + PDP socket | 1505 KB | 1285 KB |

   The network stack costs about 350 KB and leaves 1.25 MB, so memory is not a
   blocker.  The transport connects a group on hardware; the PDP socket was
   created (no peer was present).
3. **Full-screen local view**: render only the local slot's camera, in the
   single-player layout (wide FOV, anchored HUD) while the simulation stays in
   2P mode.  The 2P code path ties screen mode to viewport layout in about a
   dozen files (render_player.c, code_80057C60.c, spawn_players.c,
   skybox_and_splitscreen.c, camera.c, race_logic.c).
4. **Two real PSPs**: ad hoc transport on hardware, latency and loss tuning.
5. **Lobby**: host/join screen, player count 2-4, drop-out handling (a peer
   that leaves becomes a parked kart or the race ends).
6. **3-4 players**: the game already simulates four; the lobby and the
   relay/latency budget are the work.

## Testing in PPSSPP

Build: `gmake -f Makefile.psp -j8 EXTRA_CFLAGS="-DPORT_INPUT_SCRIPT -DPORT_GFX_DEBUG -DPORT_COURSE_TEST -DPORT_NET -DPORT_NET_FILE"`.
Two game folders, `data/netrole.bin` = 1 in one and 2 in the other, no
`testcourse.bin`.  Start the host instance first.  Both run the input script,
which picks 2P GAME; the client's script becomes pad 2.  The mailbox files
live in `ms0:/mk64net/` (the emulator's memory stick directory).
