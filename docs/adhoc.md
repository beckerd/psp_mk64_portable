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

## Starting a session: the lobby

Every player sets up the race in the game's own menus: 2P/3P/4P GAME, the
mode (Grand Prix, VS, Battle) and the class, then OK.  For two or more players
the OK press opens a modal over the game select: HOST A RACE / JOIN A RACE /
CANCEL (Up/Down, Cross, Square cancels at any point).

- HOST loads the network modules, joins the ad hoc group and advertises the
  race it set up (players, mode, class) twice a second, showing "WAITING FOR
  N MORE PLAYERS".  Joiners whose race matches are given slots; when the
  slots are full the host sends START and the game goes on.
- JOIN loads the modules and listens for an advert that matches the race the
  joiner set up ("SEARCHING..."), asks that host for a slot ("JOINING...")
  and, once the host's advert lists its id among the filled slots, shows
  "JOINED RACE" until START.  If the advert shows the race full without it,
  it drops that host and searches again.
- START carries the host's selections, RNG seed and menu timers; both
  machines then make the OK transition into the character select in lockstep
  frame 0 (host = pad 1, joiners = pads 2-4).  Everyone picks their own
  character; the course select answers the host's pad only.  Character select, course
  select, the race, pause and results all run in lockstep from there.

Scripted tests pick the modal choice from `data/netrole.bin`: 0x12/0x13/0x14
= HOST for a 2/3/4-player race, 2..4 = JOIN.  Nothing there: the modal waits
for the pad.  A machine without a session plays single player as before.

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
3. **Full-screen local view** (done 2026-09-10): `port_render_local_player()`
   in skybox_and_splitscreen.c walks the single-player render sequence with
   every player-one resource indexed by the local slot (camera, zoom, matrices,
   skybox buffer, kart renderer) through a shadow of the player's screen struct
   whose viewport is the whole screen, at the wide FOV and the 1P geometry
   mode.  The HUD is the game's split-screen drawers for that player at
   full-screen coordinates (`port_net_hud_layout()`); only the local rank, map
   and lap/time/item draw; the split-screen divider and the position-portrait
   pair are skipped in a session.  Verified: two PPSSPP instances, each showing
   its own kart full screen, zero desyncs, pause menu works.
   Left for polish: the 1P position-portrait column (the 2P HUD animates the
   portraits into its own layout every frame), the mini-map at 1P
   coordinates for slot 0, the kart shadows for slots 2-3 (func_80021B0C /
   func_80021C78 only cover two screens), the ceremony/results camera.
4. **Lobby, 3-4 players, drop-outs** (done 2026-09-10): the in-menu lobby
   above (first version: the boot console with L/R held, replaced the same
   day); START carries the selections, seed and timers.  The
   host relays every slot's inputs it knows (star on top of the broadcast),
   so client-to-client delivery is never required.  A slot the host has
   waited 10 s for is declared dropped from that frame on and reads as an
   idle pad on every machine from that same frame (the survivors stay in
   step); a client that hears nothing from the host for 15 s drops everyone
   and plays on alone.  Verified with three PPSSPP instances: a 3-player VS
   race, each machine full screen on its own kart, zero desyncs; killing one
   client mid-session dropped it at the same frame on both survivors, which
   ran on with identical state.
   Left for polish: the 3P/4P HUD (map position per slot, the timer), a
   proper lobby screen instead of the boot console, a "connection lost"
   message on the dropped machine.
5. **Two real PSPs**: ad hoc transport on hardware, latency and loss tuning.

## Testing in PPSSPP

Build: `gmake -f Makefile.psp -j8 EXTRA_CFLAGS="-DPORT_INPUT_SCRIPT -DPORT_GFX_DEBUG -DPORT_COURSE_TEST -DPORT_NET -DPORT_NET_FILE"`.
Two or three game folders, `data/netrole.bin` = 0x12 or 0x13 in the host's,
2 and 3 in the clients', no `testcourse.bin`.  Start the host instance first.  Both run the input script,
which picks 2P GAME; the client's script becomes pad 2.  The mailbox files
live in `ms0:/mk64net/` (the emulator's memory stick directory).

## Drop-outs

The host alone declares a drop: a slot whose input it has waited
DROP_AFTER_US for (5 s on the PSP, 10 s on the file transport) is dropped
from that frame and reads as a neutral pad on every machine.  The host then
pauses everyone and asks: "PLAYER N LEFT THE RACE" -- CONTINUE / EXIT.  The
pause and the exit travel as flags in the host's own inputs (NETIN_PAUSE,
NETIN_END), so every machine applies them at the same lockstep frame.  In
the race the pause is the game's own (what START does); in the menus the
overlay freezes them.  Joiners show "WAITING FOR HOST..." meanwhile; EXIT
ends the session everywhere and sends everyone to the main menu, joiners
first seeing "HOST EXITED THE GAME" -- MAIN MENU.

A joiner that hears nothing from the host for GIVEUP_AFTER_US (6 s / 15 s)
ends its session and shows the same "HOST EXITED THE GAME" prompt; one that
finds its own slot in the host's dropped mask (it was away too long but is
still here) shows "YOU WERE DROPPED FROM THE RACE".  Both keep the
full-screen view and the paused race under the prompt until MAIN MENU.
