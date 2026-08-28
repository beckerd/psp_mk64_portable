/**
 * PSP controller -> N64 controller pad.
 *
 *   analog stick     -> stick
 *   Cross            -> A (accelerate)
 *   Square           -> B (brake / reverse)
 *   Circle, L        -> Z (use item)
 *   R                -> R (hop / drift)
 *   Triangle         -> C-up (look behind)
 *   D-pad            -> D-pad
 *   Start            -> Start
 *   Select           -> L
 */
#include <ultra64.h>
#include <pspctrl.h>
#include <pspkernel.h>
#include "port.h"

#define STICK_DEADZONE 20

void controller_psp_init(void) {
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
}

static s8 map_axis(int v) {
    int c = v - 128; // -128..127
    if (c > -STICK_DEADZONE && c < STICK_DEADZONE) {
        return 0;
    }
    // Rescale the live range to the N64's -80..80.
    if (c > 0) {
        c = (c - STICK_DEADZONE) * 80 / (127 - STICK_DEADZONE);
    } else {
        c = (c + STICK_DEADZONE) * 80 / (128 - STICK_DEADZONE);
    }
    if (c > 80) {
        c = 80;
    } else if (c < -80) {
        c = -80;
    }
    return (s8) c;
}

void controller_psp_read(OSContPad* pad) {
    SceCtrlData d;
    u16 b = 0;

    sceCtrlPeekBufferPositive(&d, 1);
    /* Hold SELECT for 3 seconds to toggle the FPS counter (off by default). */
    {
        extern int gPortShowFps;
        static u32 selectSince; static int selectArmed = 1;
        u32 now = sceKernelGetSystemTimeLow();
        if (d.Buttons & PSP_CTRL_SELECT) {
            if (selectSince == 0) selectSince = now ? now : 1;
            else if (selectArmed && now - selectSince >= 3000000u) { gPortShowFps = !gPortShowFps; selectArmed = 0; }
        } else {
            selectSince = 0; selectArmed = 1;
        }
    }

    if (d.Buttons & PSP_CTRL_CROSS) {
        b |= A_BUTTON;
    }
    if (d.Buttons & PSP_CTRL_SQUARE) {
        b |= B_BUTTON;
    }
    if (d.Buttons & (PSP_CTRL_CIRCLE | PSP_CTRL_LTRIGGER)) {
        b |= Z_TRIG;
    }
    if (d.Buttons & PSP_CTRL_RTRIGGER) {
        b |= R_TRIG;
    }
    if (d.Buttons & PSP_CTRL_TRIANGLE) {
        b |= U_CBUTTONS;
    }
    if (d.Buttons & PSP_CTRL_SELECT) {
        b |= L_TRIG;
    }
    if (d.Buttons & PSP_CTRL_START) {
        b |= START_BUTTON;
    }
    if (d.Buttons & PSP_CTRL_UP) {
        b |= U_JPAD;
    }
    if (d.Buttons & PSP_CTRL_DOWN) {
        b |= D_JPAD;
    }
    if (d.Buttons & PSP_CTRL_LEFT) {
        b |= L_JPAD;
    }
    if (d.Buttons & PSP_CTRL_RIGHT) {
        b |= R_JPAD;
    }

    pad->button = b;
    pad->stick_x = map_axis(d.Lx);
    pad->stick_y = (s8) -map_axis(d.Ly); // PSP Y grows downward
    /* The D-pad steers too: Mario Kart 64 ignores the N64 D-pad, and the PSP
     * nub is imprecise.  Full deflection (diagonals scaled to keep the
     * magnitude) whenever the nub itself is centred. */
    if (pad->stick_x == 0 && pad->stick_y == 0) {
        int dx = ((d.Buttons & PSP_CTRL_RIGHT) ? 1 : 0) - ((d.Buttons & PSP_CTRL_LEFT) ? 1 : 0);
        int dy = ((d.Buttons & PSP_CTRL_UP) ? 1 : 0) - ((d.Buttons & PSP_CTRL_DOWN) ? 1 : 0);
        int mag = (dx && dy) ? 57 : 80; /* 80/sqrt(2) on diagonals */
        pad->stick_x = (s8) (dx * mag);
        pad->stick_y = (s8) (dy * mag);
    }
    pad->errno = 0;
}
