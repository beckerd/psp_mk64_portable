# Test builds

Not releases: builds handed out for testing.

- `mk64-adhoc-test.zip` (2026-09-19): ad hoc multiplayer test build 14 from the
  `adhoc` branch (commit 496c2ba plus the save and build-check fixes). Unzip
  and copy the `MK64Portable` folder to `ms0:/PSP/GAME/` (keep your existing
  ROM there). Every PSP needs this same build: different builds now refuse
  each other. `ADHOC_TEST.txt` inside says what to try and which logs to send
  back.
- `mk64-fps-optimized-test.zip` (2026-09-20, build 2): experimental "fps-optimized" build
  from the `max_fps_experiments` branch (commit 9849e20; build 2 addresses a rare
  2-3 s freeze mid-race): 60 fps in 1P races,
  sound mixing on the Media Engine, a faster renderer. It installs NEXT TO the
  normal copy: unzip and copy the `MK64PortableFPS` folder to `ms0:/PSP/GAME/`,
  and put your ROM in it too. Keep `mk64k.prx` beside the EBOOT. `FPS_TEST.txt`
  inside says what to look at and which logs to send back. It only plays ad hoc
  against the same build, not against the ad hoc test build above.
- `MK64Portable-1.7-test.zip` (updated 2026-09-26, commit `3de0728`): 1.7 test build, everything in 1.6 plus
  two fixes to try -- EXTRA mode (the mirrored courses after gold in every cup,
  #20: the course vanished) and the penguins on Sherbet Land (#17: white square
  eyes, black beaks). This update also fixes the mirrored courses' collision
  mesh so the road faces up correctly. All 16 EXTRA courses passed short emulator
  runs and collision checks; full races and real PSP play still need testing.
  Same install as a release: unzip and drag the
  `MK64Portable` folder into `PSP/GAME/`, or just replace `EBOOT.PBP`; your save
  is kept and the first start rebuilds the cached game data. EBOOT.PBP MD5
  `cbf235ffa298d71aaa9aaa220b9c6d65`. Ad hoc play needs every PSP on this build,
  including anyone who downloaded the earlier 1.7 test ZIP.
