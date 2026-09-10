/**
 * Lockstep session: every machine simulates all players; only the controller
 * inputs travel.  Frame F of the simulation uses, for every slot, the input
 * that slot sampled at its frame F - INPUT_DELAY; a machine that lacks an
 * input for F stalls until it arrives.  Each packet repeats the last
 * REDUNDANCY frames so a lost packet costs nothing.  Every CHECK_EVERY frames
 * a checksum of the players' state rides along; a mismatch is logged as a
 * desync.  Slot 0 is the host; the host assigns slots in join order.
 */
#ifdef PORT_NET
#include <ultra64.h>
#include <macros.h>
#include <stdio.h>
#include <string.h>
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <common_structs.h>
#include "port_net.h"
#include "../port.h"
#include "main.h"
#include "menus.h"
#include "buffers.h"

#define RING 128
#define INPUT_DELAY 2
#define REDUNDANCY 8
#define CHECK_EVERY 30
#define NET_MAGIC 0xA7
enum { PKT_HELLO = 1, PKT_START = 2, PKT_INPUT = 3 };

typedef struct { u16 button; s8 sx, sy; } NetInput;
/* Naturally aligned (both machines run the same binary); no packing: a packed
 * u32 member would be an unaligned store on MIPS. */
typedef struct {
    u8 magic, type, slot, count;
    u32 frame;       /* INPUT: frame of in[0].  START: 0 */
    u32 check_frame; /* latest state checksum the sender has */
    u32 checksum;
    u8 ids[NET_MAX_PLAYERS][NET_ID_LEN]; /* START: slot -> transport id */
    u8 players, delay, pad[2];
    NetInput in[REDUNDANCY];
} NetPkt;

static int sRole = NET_ROLE_NONE, sRunning, sPlayers = 1, sSlot;
static u32 sFrame;
static u8 sIds[NET_MAX_PLAYERS][NET_ID_LEN];
static int sKnown[NET_MAX_PLAYERS]; /* host: slot has a peer */
static NetInput sRing[NET_MAX_PLAYERS][RING];
static u32 sHave[NET_MAX_PLAYERS][RING]; /* frame stored in that ring slot, ~0 = none */
static struct { u32 frame, sum; } sMyCheck[16];
static u32 sLastReported[NET_MAX_PLAYERS];
static u32 sStalls, sLastStallLog;
int gPortNetDesync;

extern s32 gGlobalTimer;
extern void controller_psp_read(OSContPad* pad);

static u32 fnv(u32 h, const void* p, u32 n) {
    const u8* b = (const u8*) p;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

/* State that must agree on every machine after the same inputs. */
static u32 state_checksum(void) {
    u32 h = 2166136261u;
    int i;
    for (i = 0; i < 8; i++) {
        h = fnv(h, gPlayers[i].pos, sizeof(gPlayers[i].pos));
        h = fnv(h, gPlayers[i].velocity, sizeof(gPlayers[i].velocity));
        h = fnv(h, &gPlayers[i].speed, sizeof(gPlayers[i].speed));
        h = fnv(h, gPlayers[i].rotation, sizeof(gPlayers[i].rotation));
    }
    h = fnv(h, &gRandomSeed16, sizeof(gRandomSeed16));
    h = fnv(h, &gCourseTimer, sizeof(gCourseTimer));
    h = fnv(h, &gGamestate, sizeof(gGamestate));
    h = fnv(h, &gMenuSelection, sizeof(gMenuSelection));
    h = fnv(h, &gGlobalTimer, sizeof(gGlobalTimer));
    return h;
}

static int slot_of(const u8 id[NET_ID_LEN]) {
    int s;
    for (s = 0; s < sPlayers; s++) {
        if (memcmp(sIds[s], id, NET_ID_LEN) == 0) return s;
    }
    return -1;
}

static void store_input(int slot, u32 frame, const NetInput* in) {
    sRing[slot][frame & (RING - 1)] = *in;
    sHave[slot][frame & (RING - 1)] = frame;
}

static int have_input(int slot, u32 frame) {
    return sHave[slot][frame & (RING - 1)] == frame;
}

static void send_pkt(NetPkt* p) {
    p->magic = NET_MAGIC;
    p->slot = (u8) sSlot;
    net_transport_send(p, sizeof(*p));
}

static void send_start(void) {
    NetPkt p;
    memset(&p, 0, sizeof(p));
    p.type = PKT_START;
    p.players = (u8) sPlayers;
    p.delay = INPUT_DELAY;
    memcpy(p.ids, sIds, sizeof(sIds));
    send_pkt(&p);
}

static void send_hello(void) {
    NetPkt p;
    memset(&p, 0, sizeof(p));
    p.type = PKT_HELLO;
    send_pkt(&p);
}

static void latest_check(u32* frame, u32* sum) {
    int i, best = -1;
    for (i = 0; i < 16; i++) {
        if (sMyCheck[i].frame != ~0u && (best < 0 || sMyCheck[i].frame > sMyCheck[best].frame)) best = i;
    }
    if (best < 0) { *frame = ~0u; *sum = 0; return; }
    *frame = sMyCheck[best].frame;
    *sum = sMyCheck[best].sum;
}

static void send_inputs(void) {
    NetPkt p;
    u32 last = sFrame + INPUT_DELAY, first = last >= REDUNDANCY - 1 ? last - (REDUNDANCY - 1) : 0;
    u32 f;
    memset(&p, 0, sizeof(p));
    p.type = PKT_INPUT;
    p.frame = first;
    for (f = first; f <= last; f++) {
        p.in[p.count++] = sRing[sSlot][f & (RING - 1)];
    }
    latest_check(&p.check_frame, &p.checksum);
    send_pkt(&p);
}

static void handle_pkt(const NetPkt* p, const u8 from[NET_ID_LEN]) {
    int slot;
    if (p->magic != NET_MAGIC) return;
    if (p->type == PKT_HELLO) {
        if (sRole != NET_ROLE_HOST) return;
        slot = slot_of(from);
        if (slot < 0) {
            for (slot = 1; slot < sPlayers; slot++) {
                if (!sKnown[slot]) { memcpy(sIds[slot], from, NET_ID_LEN); sKnown[slot] = 1; break; }
            }
            if (slot >= sPlayers) return; /* full */
            PORT_LOG("net: peer %02X%02X%02X%02X%02X%02X -> slot %d\n", from[0], from[1], from[2], from[3], from[4], from[5], slot);
        }
        /* Everyone here?  Send START (again: a late joiner or a lost packet). */
        for (slot = 1; slot < sPlayers; slot++) if (!sKnown[slot]) return;
        send_start();
        return;
    }
    if (p->type == PKT_START) {
        if (sRole != NET_ROLE_CLIENT || sRunning) return;
        sPlayers = p->players;
        memcpy(sIds, p->ids, sizeof(sIds));
        slot = slot_of(net_transport_local_id());
        if (slot < 0) { PORT_LOG("net: START without our id\n"); return; }
        sSlot = slot;
        sRunning = 1;
        PORT_LOG("net: START: %d players, we are slot %d\n", sPlayers, sSlot);
        return;
    }
    if (p->type == PKT_INPUT) {
        u32 i;
        static u32 sSeen;
        slot = p->slot;
        if (sSeen++ < 3) PORT_LOG("net: INPUT from slot %d frames %u..%u (we are at %u)\n", slot, (unsigned) p->frame, (unsigned) (p->frame + p->count - 1), (unsigned) sFrame);
        if (slot < 0 || slot >= sPlayers || slot == sSlot) return;
        if (memcmp(sIds[slot], from, NET_ID_LEN) != 0) return; /* not who they claim */
        for (i = 0; i < p->count && i < REDUNDANCY; i++) {
            u32 f = p->frame + i;
            if (f + RING / 2 < sFrame) continue;      /* ancient */
            if (f >= sFrame + RING / 2) continue;     /* too far ahead for the ring */
            store_input(slot, f, &p->in[i]);
        }
        if (p->check_frame != ~0u) {
            const u32 idx = (p->check_frame / CHECK_EVERY) % 16;
            if (sMyCheck[idx].frame == p->check_frame && sMyCheck[idx].sum != p->checksum && sLastReported[slot] != p->check_frame) {
                sLastReported[slot] = p->check_frame;
                gPortNetDesync = 1;
                PORT_LOG("net: DESYNC at frame %u: ours %08X, slot %d %08X\n", (unsigned) p->check_frame, (unsigned) sMyCheck[idx].sum, slot, (unsigned) p->checksum);
            }
        }
    }
}

static void poll(void) {
    NetPkt p;
    u8 from[NET_ID_LEN];
    int n, guard = 64;
    while (guard-- > 0 && (n = net_transport_recv(&p, sizeof(p), from)) > 0) {
        if (n == (int) sizeof(p)) handle_pkt(&p, from);
    }
}

static int pick_role(void) {
    FILE* f = fopen(port_save_path("netrole.bin"), "rb");
    SceCtrlData d;
    if (f != NULL) {
        int c = fgetc(f);
        fclose(f);
        if (c == 1) return NET_ROLE_HOST;
        if (c >= 2 && c <= NET_MAX_PLAYERS) return c;
    }
    sceCtrlPeekBufferPositive(&d, 1);
    if (d.Buttons & PSP_CTRL_LTRIGGER) return NET_ROLE_HOST;
    if (d.Buttons & PSP_CTRL_RTRIGGER) return NET_ROLE_CLIENT;
    return NET_ROLE_NONE;
}

int port_net_boot(void) {
    int role = pick_role(), i, s, iter = 0;
    NetInput neutral = { 0, 0, 0 };
    if (role == NET_ROLE_NONE) return 1;
    PORT_LOG("net: role %d\n", role);
    sRole = role == NET_ROLE_HOST ? NET_ROLE_HOST : NET_ROLE_CLIENT;
    sPlayers = 2; /* v1: two machines; the host's START carries the count */
    memset(sHave, 0xFF, sizeof(sHave));
    for (i = 0; i < 16; i++) sMyCheck[i].frame = ~0u;
    for (s = 0; s < NET_MAX_PLAYERS; s++) {
        for (i = 0; i < INPUT_DELAY; i++) store_input(s, (u32) i, &neutral); /* nobody's input exists for the first frames */
    }
    pspDebugScreenSetXY(0, 2);
    pspDebugScreenPrintf("MK64 Portable ad hoc: %s\n", sRole == NET_ROLE_HOST ? "hosting" : "joining");
    if (!net_transport_init(role, "MK64")) {
        pspDebugScreenPrintf("%s\n", net_transport_status());
        PORT_LOG("net: transport init failed: %s\n", net_transport_status());
        sceKernelDelayThread(3 * 1000 * 1000);
        sRole = NET_ROLE_NONE;
        return 1;
    }
    pspDebugScreenPrintf("%s\n", net_transport_status());
    memcpy(sIds[0], sRole == NET_ROLE_HOST ? net_transport_local_id() : sIds[0], NET_ID_LEN);
    if (sRole == NET_ROLE_HOST) { sKnown[0] = 1; sSlot = 0; }
    while (!sRunning) {
        poll();
        if (sRole == NET_ROLE_HOST) {
            int n = 0;
            for (s = 1; s < sPlayers; s++) n += sKnown[s];
            if (n == sPlayers - 1) { sRunning = 1; send_start(); }
            if ((iter % 30) == 0) { pspDebugScreenSetXY(0, 5); pspDebugScreenPrintf("waiting for %d player(s)...   ", sPlayers - 1 - n); }
        } else {
            if ((iter % 15) == 0) send_hello();
            if ((iter % 30) == 0) { pspDebugScreenSetXY(0, 5); pspDebugScreenPrintf("waiting for the host...   "); }
        }
        iter++;
        sceKernelDelayThread(33 * 1000);
    }
    PORT_LOG("net: session started: %d players, slot %d, delay %d\n", sPlayers, sSlot, INPUT_DELAY);
    pspDebugScreenSetXY(0, 6);
    pspDebugScreenPrintf("connected: %d players, you are player %d\n", sPlayers, sSlot + 1);
    return sPlayers;
}

int port_net_active(void) { return sRole != NET_ROLE_NONE && sRunning; }
int port_net_players(void) { return sPlayers; }
int port_net_local_slot(void) { return sSlot; }

int port_net_frame_begin(void) {
    OSContPad pad;
    NetInput in;
    int s;
    static u32 sSampled = ~0u;
    static int sJustSampled;
    poll();
    if (sRole == NET_ROLE_HOST && sFrame < 90) send_start(); /* cover a lost START */
    /* Our input for frame F + delay: sampled once per frame (a stall retry
     * must not re-read the pad -- the debug input script counts frames by it). */
    if (sSampled != sFrame) {
        sSampled = sFrame;
        sJustSampled = 1;
        port_local_pad(&pad);
        in.button = pad.button; in.sx = pad.stick_x; in.sy = pad.stick_y;
        store_input(sSlot, sFrame + INPUT_DELAY, &in);
        if ((sFrame % 60) == 0) PORT_LOG("net: frame %u (%u stalls)\n", (unsigned) sFrame, (unsigned) sStalls);
    }
    if ((sFrame % CHECK_EVERY) == 0) {
        int idx = (sFrame / CHECK_EVERY) % 16;
        sMyCheck[idx].frame = sFrame;
        sMyCheck[idx].sum = state_checksum();
    }
    /* Send once per new frame; while stalled, resend every 50 ms (loss cover)
     * rather than on every retry -- a flood only slows the peer down. */
    {
        static u32 sLastSend;
        u32 now = sceKernelGetSystemTimeLow();
        if (sSampled == sFrame && sJustSampled) { send_inputs(); sLastSend = now; sJustSampled = 0; }
        else if (now - sLastSend >= 50000) { send_inputs(); sLastSend = now; }
    }
    for (s = 0; s < sPlayers; s++) {
        if (!have_input(s, sFrame)) {
            sStalls++;
            if (sStalls - sLastStallLog >= 200) { sLastStallLog = sStalls; PORT_LOG("net: frame %u waiting for slot %d (%u stalls so far)\n", (unsigned) sFrame, s, (unsigned) sStalls); }
            return 0;
        }
    }
    return 1;
}

void port_net_pads(OSContPad* pads) {
    int s;
    for (s = 0; s < NET_MAX_PLAYERS; s++) {
        if (s < sPlayers) {
            const NetInput* in = &sRing[s][sFrame & (RING - 1)];
            pads[s].button = in->button;
            pads[s].stick_x = in->sx;
            pads[s].stick_y = in->sy;
            pads[s].errno = 0;
        } else {
            pads[s].button = 0; pads[s].stick_x = pads[s].stick_y = 0;
            pads[s].errno = CONT_NO_RESPONSE_ERROR;
        }
    }
    /* No frame advance here: the game may read the pads more than once per
     * iteration (it does during init), and every read must see the same frame. */
}

void port_net_frame_end(void) {
    sFrame++;
}
#endif
