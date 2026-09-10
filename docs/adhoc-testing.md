# Ad hoc test notes (for testers)

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
- Mid-race, one PSP presses HOME and quits: the others should carry on after
  about ten seconds with that kart parked.

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
