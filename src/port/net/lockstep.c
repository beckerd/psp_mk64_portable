/**
 * Lockstep session: every machine simulates all players; only the controller
 * inputs travel.  Frame F of the simulation uses, for every slot, the input
 * that slot sampled at its frame F - INPUT_DELAY; a machine that lacks an
 * input for F stalls until it arrives.  Each packet repeats the last
 * REDUNDANCY frames so a lost packet costs nothing.  Every CHECK_EVERY frames
 * a checksum of the players' state rides along; a mismatch is logged as a
 * desync.  Slot 0 is the host; the host assigns slots in join order.
 *
 * The host also relays every slot's inputs it knows (star topology on top of
 * the broadcast), and decides drop-outs: a slot whose input the host has
 * waited DROP_AFTER_US for is declared dropped from that frame on and reads as
 * a neutral pad on every machine from that same frame, so the survivors stay
 * in step.  A client that hears nothing from the host for GIVEUP_AFTER_US
 * drops everyone else and plays on alone.
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
#define DROP_AFTER_US (10u * 1000000u)
#define GIVEUP_AFTER_US (15u * 1000000u)
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
    u8 players, delay, dropped, pad;     /* dropped: bitmask (host's word) */
    u32 drop_frame[NET_MAX_PLAYERS];     /* first neutral frame per dropped slot */
    NetInput in[REDUNDANCY];
} NetPkt;

static int sRole = NET_ROLE_NONE, sRunning, sPlayers = 1, sSlot, sAutoStart;
static u32 sFrame;
static u8 sIds[NET_MAX_PLAYERS][NET_ID_LEN];
static int sKnown[NET_MAX_PLAYERS]; /* host: slot has a peer */
static NetInput sRing[NET_MAX_PLAYERS][RING];
static u32 sHave[NET_MAX_PLAYERS][RING]; /* frame stored in that ring slot, ~0 = none */
static struct { u32 frame, sum; } sMyCheck[16];
static u32 sLastReported[NET_MAX_PLAYERS];
static u32 sStalls, sLastStallLog;
static u8 sDropped;                          /* bitmask */
static u32 sDropFrame[NET_MAX_PLAYERS];
static u32 sStallSinceUs, sLastHostPktUs;    /* wall clock */
static int sStallSlot = -1;
int gPortNetDesync;

extern s32 gGlobalTimer;
extern void controller_psp_read(OSContPad* pad);

static u32 now_us(void) { return sceKernelGetSystemTimeLow(); }

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
    for (s = 0; s < NET_MAX_PLAYERS; s++) {
        if ((s == 0 || sKnown[s] || sRole == NET_ROLE_CLIENT) && memcmp(sIds[s], id, NET_ID_LEN) == 0) return s;
    }
    return -1;
}

static void store_input(int slot, u32 frame, const NetInput* in) {
    sRing[slot][frame & (RING - 1)] = *in;
    sHave[slot][frame & (RING - 1)] = frame;
}

static int is_dropped(int slot, u32 frame) {
    return (sDropped & (1 << slot)) && frame >= sDropFrame[slot];
}

static int have_input(int slot, u32 frame) {
    return is_dropped(slot, frame) || sHave[slot][frame & (RING - 1)] == frame;
}

static void fill_drop(NetPkt* p) {
    int s;
    p->dropped = sDropped;
    for (s = 0; s < NET_MAX_PLAYERS; s++) p->drop_frame[s] = sDropFrame[s];
}

static void send_pkt(NetPkt* p) {
    p->magic = NET_MAGIC;
    net_transport_send(p, sizeof(*p));
}

static void send_start(void) {
    NetPkt p;
    memset(&p, 0, sizeof(p));
    p.type = PKT_START;
    p.slot = (u8) sSlot;
    p.players = (u8) sPlayers;
    p.delay = INPUT_DELAY;
    memcpy(p.ids, sIds, sizeof(sIds));
    fill_drop(&p);
    send_pkt(&p);
}

static void send_hello(void) {
    NetPkt p;
    memset(&p, 0, sizeof(p));
    p.type = PKT_HELLO;
    p.slot = (u8) sSlot;
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

/* The last REDUNDANCY frames of `slot`'s inputs that we hold, ending at the
 * newest one we have at or before `last`. */
static void send_slot_inputs(int slot, u32 last) {
    NetPkt p;
    u32 f, first;
    while (last > 0 && !have_input(slot, last)) last--;
    if (!have_input(slot, last)) return;
    first = last >= REDUNDANCY - 1 ? last - (REDUNDANCY - 1) : 0;
    while (first < last && !have_input(slot, first)) first++;
    memset(&p, 0, sizeof(p));
    p.type = PKT_INPUT;
    p.slot = (u8) slot;
    p.frame = first;
    for (f = first; f <= last; f++) {
        p.in[p.count++] = sRing[slot][f & (RING - 1)];
    }
    latest_check(&p.check_frame, &p.checksum);
    fill_drop(&p);
    send_pkt(&p);
}

static void send_inputs(void) {
    int s;
    send_slot_inputs(sSlot, sFrame + INPUT_DELAY);
    if (sRole == NET_ROLE_HOST) { /* relay what we know of everyone else */
        for (s = 1; s < sPlayers; s++) {
            if (s != sSlot && !(sDropped & (1 << s))) send_slot_inputs(s, sFrame + INPUT_DELAY);
        }
    }
}

static void apply_drops(const NetPkt* p) {
    int s;
    for (s = 1; s < NET_MAX_PLAYERS; s++) {
        if ((p->dropped & (1 << s)) && !(sDropped & (1 << s))) {
            sDropped |= 1 << s;
            sDropFrame[s] = p->drop_frame[s];
            PORT_LOG("net: host dropped slot %d from frame %u\n", s, (unsigned) sDropFrame[s]);
        }
    }
}

static void handle_pkt(const NetPkt* p, const u8 from[NET_ID_LEN]) {
    int slot, from_host;
    if (p->magic != NET_MAGIC) return;
    from_host = memcmp(sIds[0], from, NET_ID_LEN) == 0;
    if (from_host && sRole == NET_ROLE_CLIENT) sLastHostPktUs = now_us();
    if (p->type == PKT_HELLO) {
        if (sRole != NET_ROLE_HOST) return;
        slot = slot_of(from);
        if (slot < 0) {
            if (sRunning) return; /* no late joins */
            for (slot = 1; slot < NET_MAX_PLAYERS; slot++) {
                if (!sKnown[slot]) { memcpy(sIds[slot], from, NET_ID_LEN); sKnown[slot] = 1; break; }
            }
            if (slot >= NET_MAX_PLAYERS) return; /* full */
            PORT_LOG("net: peer %02X%02X%02X%02X%02X%02X -> slot %d\n", from[0], from[1], from[2], from[3], from[4], from[5], slot);
        }
        if (sRunning) send_start(); /* a late/lost START */
        return;
    }
    if (p->type == PKT_START) {
        if (sRole != NET_ROLE_CLIENT) return;
        /* Before START we do not know the host's id: the packet carries it. */
        if (memcmp(p->ids[0], from, NET_ID_LEN) != 0) return;
        if (sRunning) { apply_drops(p); return; }
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
        if (slot < 0 || slot >= sPlayers || slot == sSlot) return;
        /* the slot's own machine, or the host relaying it */
        if (memcmp(sIds[slot], from, NET_ID_LEN) != 0 && !from_host) return;
        if (from_host && sRole == NET_ROLE_CLIENT) apply_drops(p);
        if (sSeen++ < 3) PORT_LOG("net: INPUT for slot %d frames %u..%u%s (we are at %u)\n", slot, (unsigned) p->frame, (unsigned) (p->frame + p->count - 1), from_host && slot != 0 ? " (relayed)" : "", (unsigned) sFrame);
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

/* data/netrole.bin: 1 host (the lobby: Cross starts), 2..4 join, 0x10|n host
 * that starts by itself once n players are in (scripted tests).  Buttons: L
 * held at boot hosts, R joins. */
static int pick_role(void) {
    FILE* f = fopen(port_save_path("netrole.bin"), "rb");
    SceCtrlData d;
    if (f != NULL) {
        int c = fgetc(f);
        fclose(f);
        if (c == 1) return NET_ROLE_HOST;
        if (c >= 2 && c <= NET_MAX_PLAYERS) return c;
        if ((c & 0xF0) == 0x10 && (c & 0x0F) >= 2 && (c & 0x0F) <= NET_MAX_PLAYERS) { sAutoStart = c & 0x0F; return NET_ROLE_HOST; }
    }
    sceCtrlPeekBufferPositive(&d, 1);
    if (d.Buttons & PSP_CTRL_LTRIGGER) return NET_ROLE_HOST;
    if (d.Buttons & PSP_CTRL_RTRIGGER) return NET_ROLE_CLIENT;
    return NET_ROLE_NONE;
}

int port_net_boot(void) {
    int role = pick_role(), i, s, iter = 0, cross_was_down = 1;
    NetInput neutral = { 0, 0, 0 };
    if (role == NET_ROLE_NONE) return 1;
    PORT_LOG("net: role %d\n", role);
    sRole = role == NET_ROLE_HOST ? NET_ROLE_HOST : NET_ROLE_CLIENT;
    sPlayers = NET_MAX_PLAYERS; /* while joining: any slot may fill; START fixes the count */
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
    if (sRole == NET_ROLE_HOST) {
        memcpy(sIds[0], net_transport_local_id(), NET_ID_LEN);
        sKnown[0] = 1;
        sSlot = 0;
    }
    while (!sRunning) {
        poll();
        if (sRole == NET_ROLE_HOST) {
            SceCtrlData d;
            int n = 0, start = 0;
            for (s = 1; s < NET_MAX_PLAYERS; s++) n += sKnown[s];
            sceCtrlPeekBufferPositive(&d, 1);
            if (!(d.Buttons & PSP_CTRL_CROSS)) cross_was_down = 0;
            if (n >= 1 && (d.Buttons & PSP_CTRL_CROSS) && !cross_was_down) start = 1; /* a fresh press */
            if (sAutoStart && n >= sAutoStart - 1) start = 1;
            if (n == NET_MAX_PLAYERS - 1) start = 1;
            if (start) {
                sPlayers = 1 + n;
                sRunning = 1;
                send_start();
            } else if ((iter % 30) == 0) {
                pspDebugScreenSetXY(0, 5);
                pspDebugScreenPrintf("%d player(s) joined.  %s   ", n, n >= 1 ? "Press X to start." : "Waiting...");
            }
        } else {
            if ((iter % 15) == 0) send_hello();
            if ((iter % 30) == 0) { pspDebugScreenSetXY(0, 5); pspDebugScreenPrintf("waiting for the host to start...   "); }
        }
        iter++;
        sceKernelDelayThread(33 * 1000);
    }
    sLastHostPktUs = now_us();
    PORT_LOG("net: session started: %d players, slot %d, delay %d\n", sPlayers, sSlot, INPUT_DELAY);
    pspDebugScreenSetXY(0, 6);
    pspDebugScreenPrintf("connected: %d players, you are player %d\n", sPlayers, sSlot + 1);
    return sPlayers;
}

int port_net_active(void) { return sRole != NET_ROLE_NONE && sRunning; }
int port_net_players(void) { return sPlayers; }
int port_net_local_slot(void) { return sSlot; }

static void drop_slot(int s, u32 frame, const char* why) {
    sDropped |= 1 << s;
    sDropFrame[s] = frame;
    PORT_LOG("net: slot %d dropped from frame %u (%s)\n", s, (unsigned) frame, why);
}

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
        u32 now = now_us();
        if (sSampled == sFrame && sJustSampled) { send_inputs(); sLastSend = now; sJustSampled = 0; }
        else if (now - sLastSend >= 50000) { send_inputs(); sLastSend = now; }
    }
    for (s = 0; s < sPlayers; s++) {
        if (!have_input(s, sFrame)) {
            u32 now = now_us();
            if (sStallSlot != s) { sStallSlot = s; sStallSinceUs = now; }
            sStalls++;
            if (sStalls - sLastStallLog >= 200) { sLastStallLog = sStalls; PORT_LOG("net: frame %u waiting for slot %d (%u stalls so far)\n", (unsigned) sFrame, s, (unsigned) sStalls); }
            if (sRole == NET_ROLE_HOST && now - sStallSinceUs >= DROP_AFTER_US) {
                drop_slot(s, sFrame, "no input for 10 s"); /* from this frame on: neutral, on every machine */
                continue;
            }
            if (sRole == NET_ROLE_CLIENT && now - sLastHostPktUs >= GIVEUP_AFTER_US) {
                int o;
                for (o = 0; o < sPlayers; o++) if (o != sSlot && !(sDropped & (1 << o))) drop_slot(o, sFrame, "lost the host");
                continue;
            }
            return 0;
        }
    }
    sStallSlot = -1;
    return 1;
}

void port_net_pads(OSContPad* pads) {
    int s;
    for (s = 0; s < NET_MAX_PLAYERS; s++) {
        if (s < sPlayers && !is_dropped(s, sFrame)) {
            const NetInput* in = &sRing[s][sFrame & (RING - 1)];
            pads[s].button = in->button;
            pads[s].stick_x = in->sx;
            pads[s].stick_y = in->sy;
            pads[s].errno = 0;
        } else {
            pads[s].button = 0; pads[s].stick_x = pads[s].stick_y = 0;
            pads[s].errno = s < sPlayers ? 0 : CONT_NO_RESPONSE_ERROR; /* a dropped player: a plugged-in, idle pad */
        }
    }
    /* No frame advance here: the game may read the pads more than once per
     * iteration (it does during init), and every read must see the same frame. */
}

void port_net_frame_end(void) {
    sFrame++;
}
#endif
