# Ad hoc test notes (for testers)

## Build 5: one race, both logs (this may already be fixed)

I fixed a bug that could make two consoles drift apart mid-race.  This build
also records extra detail so that, if it still happens, one race pins down
the exact cause.

1. Both PSPs: confirm the EBOOT is this build (MD5 e02b53e953764163f8e4e8be02eff3d0), and reboot both so
   the logs start clean.
2. WLAN on.  Set up the SAME 2P VS race, one hosts, one joins.
3. Race one race to the finish, or until the two screens clearly disagree
   (a kart driving into walls, both players "winning").  Try to keep other
   2.4 GHz gear (a PS5 and its controller, Wi-Fi) away, since that causes the
   lag.
4. Send back log.txt AND log_prev.txt from BOTH PSPs, and say whether the two
   screens stayed in agreement or drifted, and roughly when.

That is all.  The logs now include "PDUMP" lines; they are for me to compare
the two consoles and are meant to be large.

---

This build adds PSP-to-PSP ad hoc play to the 2P/3P/4P modes.  Two or more
PSPs, each with this build in `ms0:/PSP/GAME/MK64Portable/` (EBOOT.PBP plus
the ROM, the same as the normal install).

## Playing

1. Slide the WLAN switch on on every PSP.
2. On every PSP pick the same thing in the game select: 2P (or 3P/4P) GAME,
   the same mode (MARIO GP, VS or BATTLE) and the same class (50cc etc.).
3. Press OK.  A panel appears: HOST RACE / JOIN RACE / CANCEL.
   - One PSP picks HOST RACE.  It shows "WAITING FOR N PLAYERS" and counts
     down as the others join.
   - The others pick JOIN RACE.  They show "SEARCHING...", then "JOINING...",
     then "JOINED RACE" while the host waits for the rest.
   - CANCEL (or Circle) backs out at any point.
4. When the last player joins, every PSP moves to the character select
   together.  Everyone picks their own character.  The host picks the cup and
   course; the other pads do nothing on that screen.
5. Race.  Each PSP shows its own player full screen, with the usual HUD.

Things worth trying, in this order:
- 2P MARIO GP 50cc, one full cup.
- 2P VS, then 2P BATTLE.
- 3P or 4P if there are enough PSPs.
- Mid-race, a joiner presses HOME and quits: after about five seconds the
  host's game pauses with "PLAYER N LEFT THE RACE" and CONTINUE / EXIT;
  the other joiners see "WAITING FOR HOST...".  CONTINUE resumes with that
  kart parked; EXIT sends everyone to the main menu.
- Mid-race, the HOST presses HOME and quits: after about six seconds each
  joiner pauses with "HOST EXITED THE GAME" and a MAIN MENU button.

## If the two games stop agreeing (the other kart drives into walls, items differ)

That is a desync, not a dropped link: the log on each PSP prints "DESYNC at
frame N -- differs in: ..." lines from the first mismatch on.  Nothing to
do in the moment; just send both logs.  The "net: state check" lines near
the start of the session and every 20 s are there to be compared side by
side.

## If the players get disconnected during a race

Please note roughly how long into the race it happened and what each screen
showed, and send the logs (below).  Then one more try: put an empty file
named `cpu222` (no extension) into `ms0:/PSP/GAME/MK64Portable/data/` on
BOTH PSPs and race again.  That runs the game at 222 MHz instead of 333;
the original PSP-1000's WLAN is known to misbehave at 333 MHz.  Say whether
it made a difference, and which PSP models (1000 / 2000 / 3000 / Go) were
used.  Also check Settings > Power Save Settings > WLAN Power Save on each
PSP and say whether it is on.

## What to send back

From EVERY PSP that took part, the two files

    ms0:/PSP/GAME/MK64Portable/data/log.txt
    ms0:/PSP/GAME/MK64Portable/data/log_prev.txt

(log.txt is the last run, log_prev.txt the one before it), and a few words
per attempt: which PSP hosted, what was chosen, what happened, and roughly
when (the log lines carry seconds since boot).  If a PSP froze or reset, say
which one and what was on its screen.  If the race ran but the players saw
different things (one thought they won, the other did not), say so: the logs
record a checksum every 30 frames and will show where they parted.

The log is written continuously; at most half a second is lost on a freeze.
