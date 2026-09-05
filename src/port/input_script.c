/**
 * Scripted controller input (PORT_INPUT_SCRIPT builds only).
 *
 * The emulator can't be driven from the outside in this environment, so
 * interactive play is verified by replaying a canned button sequence: Start
 * on the title screen, then A through the main menu / character select /
 * course select into a real race, then hold A and steer.  Progress (gamestate,
 * menu, race position) is logged and frames are dumped periodically so the
 * run can be checked after the fact.
 */
#ifdef PORT_INPUT_SCRIPT
#include <ultra64.h>
#include <common_structs.h>
#include <macros.h>
#include "port.h"
#include "main.h"
#include "menus.h"
#include "code_800029B0.h"
#include "cpu_vehicles_camera_path.h"
#include "course.h"
#include <stdio.h>
#ifdef PORT_COURSE_TEST
#include <malloc.h>
#include <pspsysmem.h>
extern int texman_usage_percent(void);
extern size_t gFreeMemorySize; extern uintptr_t gNextFreeMemoryAddress, gFreeMemoryResetAnchor;
#endif

typedef struct {
    u32 from;   // first frame (inclusive)
    u32 to;     // last frame (inclusive)
    u16 button;
    s8 stick_x;
    s8 stick_y;
} ScriptStep;

/* A "tap" is 6 frames held.  Menus react to button *presses* (edges), so
 * every tap must be separated by released frames. */
#define TAP(f, b) { (f), (f) + 5, (b), 0, 0 }

static const ScriptStep sSteps[] = {
    TAP(1560, START_BUTTON), // pause mid-race (first so it wins over the held-A steps); shot1590/1620 show the pause screen
    { 1566, 1640, 0, 0, 0 }, // ...and release everything: a new A press would pick "CONTINUE GAME"
    TAP(240, START_BUTTON),  // title screen -> main menu
    // Main menu: 1P -> Mario GP -> 50cc -> OK (defaults; each A advances)
    TAP(400, A_BUTTON),  // leaves the title; the game select appears ~12 frames later
    TAP(470, A_BUTTON),  // (shot450 shows its top level with the L OPTION / R DATA buttons)
    TAP(520, A_BUTTON),
    TAP(580, A_BUTTON),
    // Character select: pick, then OK
    TAP(660, A_BUTTON),
    TAP(720, A_BUTTON),
    // Course select: Mushroom Cup, Luigi Raceway
    TAP(840, A_BUTTON),
    TAP(900, A_BUTTON),
    TAP(960, A_BUTTON),   // skip course intro pan if it is still showing
    TAP(1020, A_BUTTON),
    // Race: hold A and keep a gentle right-hand line (Luigi Raceway runs
    // clockwise), with one hop/drift to exercise R.
    { 1100, 1300, A_BUTTON, 0, 0 },
    { 1301, 1360, A_BUTTON, -40, 0 },
    { 1361, 1520, A_BUTTON, 30, 0 },
    { 1521, 1600, A_BUTTON | R_TRIG, -100, 0 },  // hard drift turn (behind-camera regression check)
    { 1601, 1700, A_BUTTON, 30, 0 },
    { 1301, 1360, A_BUTTON, -40, 0 },
    { 1361, 1700, A_BUTTON, 30, 0 },
    { 1701, 1720, A_BUTTON | R_TRIG, 60, 0 }, // hop/drift
    { 1721, 1900, A_BUTTON, 25, 0 },
    { 1901, 1990, A_BUTTON, -80, 0 },   // sharp left turn away from the wall
    { 1991, 2080, A_BUTTON, 80, 0 },    // sharp right
    { 2081, 3200, A_BUTTON, 25, 0 },
};

#define SCRIPT_END 3300
#define SHOT_EVERY 120

#ifdef PORT_COURSE_TEST
s32 gPortForceCourse = -1;
#endif

void port_input_script(OSContPad* pad) {
    static u32 sFrame = 0;

#ifdef PORT_COURSE_TEST
    if (sFrame == 0) {
        FILE* fcp = fopen(port_save_path("testcourse.bin"), "rb");
        if (fcp) { int c = fgetc(fcp); fclose(fcp); gPortForceCourse = c; PORT_LOG("TEST: forcing course %d\n", c); }
    }
    if (gGamestate == RACING && (sFrame % 30) == 0) {
        PORT_LOG("TEST: RACING course %d frame %u\n", gCurrentCourseId, sFrame);
    }
#ifdef PORT_SPARK_DEBUG
    {
        static int sbDone = 0;
        if (gGamestate == RACING && gPlayerOne != NULL && !sbDone && gPlayerOne->speed > 0.1f) {
            gPlayerOne->effects |= 0x2000;      // MUSHROOM_EFFECT (exactly like func_8002A704)
            gPlayerOne->boostTimer = 0x0050;
            PORT_LOG("SBTEST: mimic start-boost (MUSHROOM_EFFECT+boostTimer) at frame %u\n", sFrame);
            sbDone = 1;
        }
    }
#endif

    {
        static int memLogged = 0;
        if (gGamestate == RACING && !memLogged && sFrame > 1150) {
            struct mallinfo mi = mallinfo();
            unsigned poolUsed = (unsigned)(gNextFreeMemoryAddress - gFreeMemoryResetAnchor);
            PORT_LOG("MEM course %d: pool %uKB/%uKB | heap used %uKB free %uKB arena %uKB | maxfree %uKB | tex %d%%\n",
                     gCurrentCourseId, poolUsed/1024u, (unsigned)gFreeMemorySize/1024u,
                     (unsigned)mi.uordblks/1024u, (unsigned)mi.fordblks/1024u, (unsigned)mi.arena/1024u,
                     (unsigned)sceKernelMaxFreeMemSize()/1024u, texman_usage_percent());
            memLogged = 1;
        }
    }
#endif
    static s32 sLastState = -2, sLastMenu = -2;
    u32 i;

    for (i = 0; i < ARRAY_COUNT(sSteps); i++) {
        if (sFrame >= sSteps[i].from && sFrame <= sSteps[i].to) {
            pad->button = sSteps[i].button;
            pad->stick_x = sSteps[i].stick_x;
            pad->stick_y = sSteps[i].stick_y;
            break;
        }
    }

    if (gGamestate != sLastState || gMenuSelection != sLastMenu) {
        PORT_LOG("script f%u: gamestate %d menu %d (course %d)\n", sFrame, gGamestate, gMenuSelection,
                 gCurrentCourseId);
        sLastState = gGamestate;
        sLastMenu = gMenuSelection;
    }
    if (((sFrame % SHOT_EVERY) == 0 && sFrame >= 240) || sFrame == 450 /* top-level game select: OPTION/DATA */ ||
        (sFrame >= 1380 && sFrame <= 1700 && (sFrame % 30) == 0)) {
        port_screenshot((int) sFrame);
        if (gGamestate == RACING && gPlayerOne != NULL) {
            PORT_LOG("script f%u: p1 pos %.1f %.1f %.1f speed %.2f lap %d\n", sFrame, gPlayerOne->pos[0],
                     gPlayerOne->pos[1], gPlayerOne->pos[2], gPlayerOne->speed, gLapCountByPlayerId[0]);
        }
    }
    if (sFrame == 1320) {
        // Dump the live player-1 kart palette for comparison with the ROM copy.
        extern u8 gPlayerPalettesList[];
        FILE* fp = fopen(port_save_path("pal_p1.bin"), "wb");
        if (fp) { fwrite(&gPlayerPalettesList[0], 1, 0x200, fp); fclose(fp); }
        PORT_LOG("script: palette dumped, gPlayerPalettesList at %p\n", &gPlayerPalettesList[0]);
    }
    if (sFrame == 480) {
        extern u16* gMenuTextureBuffer;
        FILE* fp = fopen(port_save_path("menubuf.bin"), "wb");
        if (fp) { fwrite(gMenuTextureBuffer, 1, 320 * 240 * 2, fp); fclose(fp); }
        PORT_LOG("script: menu buffer dumped from %p\n", gMenuTextureBuffer);
    }
    if (sFrame == 481) {
        // One fully traced frame of the game-select screen (issue #5: the
        // GAME SELECT banner and the OPTION/DATA buttons do not draw).
        extern int gfx_debug_frame, gfx_trace_frames;
        gfx_debug_frame = 1;
        gfx_trace_frames = 1;
    }
    if (sFrame == SCRIPT_END) {
        PORT_LOG("script done\n");
    }
    sFrame++;
}
#endif
