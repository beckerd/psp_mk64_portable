# Test builds

Not releases: builds handed out for testing.

- `mk64-adhoc-test.zip` (2026-09-19): ad hoc multiplayer test build 14 from the
  `adhoc` branch (commit 496c2ba plus the save and build-check fixes). Unzip
  and copy the `MK64Portable` folder to `ms0:/PSP/GAME/` (keep your existing
  ROM there). Every PSP needs this same build: different builds now refuse
  each other. `ADHOC_TEST.txt` inside says what to try and which logs to send
  back.
- `mk64-fps-optimized-test.zip` (2026-09-20): experimental "fps-optimized" build
  from the `max_fps_experiments` branch (commit aa5d809): 60 fps in 1P races,
  sound mixing on the Media Engine, a faster renderer. It installs NEXT TO the
  normal copy: unzip and copy the `MK64PortableFPS` folder to `ms0:/PSP/GAME/`,
  and put your ROM in it too. Keep `mk64k.prx` beside the EBOOT. `FPS_TEST.txt`
  inside says what to look at and which logs to send back. It only plays ad hoc
  against the same build, not against the ad hoc test build above.
