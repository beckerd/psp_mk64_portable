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
#include <defines.h>
#include "port_net.h"
#include "../port.h"
#include "main.h"
#include "menus.h"
#include "buffers.h"
#include "menu_items.h"
#include "audio/external.h"
#include <sounds.h>
#include "replays.h"
#include "racing/race_logic.h"
#include "code_800029B0.h"

#define RING 128
#define INPUT_DELAY 2
#define REDUNDANCY 8
#define CHECK_EVERY 30
#define NET_MAGIC 0xA7
#ifdef PORT_NET_ADHOC
#define DROP_AFTER_US (5u * 1000000u)   /* the radio answers in milliseconds: a silent peer is gone */
#define GIVEUP_AFTER_US (6u * 1000000u)
#else
#define DROP_AFTER_US (10u * 1000000u)  /* the file mailbox stalls for seconds on its own */
#define GIVEUP_AFTER_US (15u * 1000000u)
#endif
enum { PKT_HELLO = 1, PKT_START = 2, PKT_INPUT = 3, PKT_ADVERT = 4 };

/* flags travel in the host's inputs only: the frame they apply to is the
 * frame every machine acts on them, so pauses and the end stay in step. */
#define NETIN_PAUSE 1 /* the host's drop-out prompt is up: the game is paused */
#define NETIN_END 2   /* the host chose EXIT: the session ends at this frame */
typedef struct { u16 button; s8 sx, sy; u8 flags; } NetInput;
/* Naturally aligned (both machines run the same binary); no packing: a packed
 * u32 member would be an unaligned store on MIPS. */
typedef struct NetPktTag {
    u8 magic, type, slot, count;
    u32 frame;       /* INPUT: frame of in[0].  START: 0 */
    u32 check_frame; /* latest state checksum the sender has */
    u32 checksum;
    u32 parts[4];    /* its components: players, RNG seed, timers, game state -- a DESYNC names the culprit */
    u8 ids[NET_MAX_PLAYERS][NET_ID_LEN]; /* START: slot -> transport id */
    u8 players, delay, dropped, mode;    /* dropped: bitmask (host's word); mode/cc: the race (ADVERT/HELLO/START) */
    u8 cc, pad0, pad1, pad2;
    u16 seed, pad3;                      /* START: gRandomSeed16 */
    s32 gtimer, flash, timing;           /* START: gGlobalTimer, gCycleFlashMenu, gMenuTimingCounter */
    u32 drop_frame[NET_MAX_PLAYERS];     /* first neutral frame per dropped slot */
    NetInput in[REDUNDANCY];
} NetPkt;

static int sRole = NET_ROLE_NONE, sRunning, sPlayers = 1, sSlot, sAutoStart;
static u32 sFrame;
static u8 sIds[NET_MAX_PLAYERS][NET_ID_LEN];
static int sKnown[NET_MAX_PLAYERS]; /* host: slot has a peer */
static NetInput sRing[NET_MAX_PLAYERS][RING];
static u32 sHave[NET_MAX_PLAYERS][RING]; /* frame stored in that ring slot, ~0 = none */
static struct { u32 frame, sum, parts[4]; } sMyCheck[16];
static u32 sLastReported[NET_MAX_PLAYERS];
static u32 sStalls, sLastStallLog;
static u8 sDropped;                          /* bitmask */
static u32 sDropFrame[NET_MAX_PLAYERS];
static u32 sStallSinceUs, sLastHostPktUs;    /* wall clock */
static int sStallSlot = -1;
static u8 sHostId[NET_ID_LEN];
static u8 sCritPlayers, sCritMode, sCritCc;
static int sLobby, sHaveHost, sJoined; /* client: a matching host was found / it lists us in its advert */
/* The drop-out prompts (docs/adhoc.md).  HOST_DROP: the host's CONTINUE /
 * EXIT choice; RESUMING / LEAVING: chosen, waiting for the flag's frame;
 * WAIT_HOST: a joiner while the host decides; HOST_GONE / DROPPED: a joiner
 * with only MAIN MENU left. */
enum { MODAL_NONE, MODAL_HOST_DROP, MODAL_RESUMING, MODAL_LEAVING, MODAL_WAIT_HOST, MODAL_HOST_GONE, MODAL_DROPPED };
static int sModal, sModalSel, sModalSlot;
static u8 sHostFlags;      /* host: the flags it puts in its outgoing inputs */
static int sEnded;         /* the session is over but the prompt is still up: local pad, everyone else neutral, full-screen view kept */
static int sAppliedPause;  /* the host's PAUSE flag as applied on this machine */
static u16 sModalPrevButtons;
static void modal_open(int which);
static void end_session_to_menu(void);
static u32 sLastHelloUs;
enum { LOBBY_NONE, LOBBY_CHOICE, LOBBY_CONNECT_HOST, LOBBY_CONNECT_JOIN, LOBBY_HOSTING, LOBBY_SEARCHING, LOBBY_ERROR };
static int criteria_match(const struct NetPktTag* p);
static void session_begin(void);
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
static u32 state_checksum(u32 parts[4]) {
    u32 h = 2166136261u, hp;
    int i;
    for (i = 0; i < 8; i++) {
        h = fnv(h, gPlayers[i].pos, sizeof(gPlayers[i].pos));
        h = fnv(h, gPlayers[i].velocity, sizeof(gPlayers[i].velocity));
        h = fnv(h, &gPlayers[i].speed, sizeof(gPlayers[i].speed));
        h = fnv(h, gPlayers[i].rotation, sizeof(gPlayers[i].rotation));
    }
    parts[0] = h;
    hp = fnv(2166136261u, &gRandomSeed16, sizeof(gRandomSeed16));
    parts[1] = hp;
    h = fnv(h, &gRandomSeed16, sizeof(gRandomSeed16));
    hp = fnv(2166136261u, &gCourseTimer, sizeof(gCourseTimer));
    hp = fnv(hp, &gGlobalTimer, sizeof(gGlobalTimer));
    parts[2] = hp;
    h = fnv(h, &gCourseTimer, sizeof(gCourseTimer));
    hp = fnv(2166136261u, &gGamestate, sizeof(gGamestate));
    hp = fnv(hp, &gMenuSelection, sizeof(gMenuSelection));
    parts[3] = hp;
    h = fnv(h, &gGamestate, sizeof(gGamestate));
    h = fnv(h, &gMenuSelection, sizeof(gMenuSelection));
    h = fnv(h, &gGlobalTimer, sizeof(gGlobalTimer));
    return h;
}
/* Full player-struct dump for pinpointing a desync: two machines' logs diffed
 * at the last synced checkpoint show the first field that drifted, before it
 * reached the position the narrow checksum watches.  gPlayers is a static, at
 * the same address on both PSPs, so pointer fields that target other statics
 * match and only the truly diverging bytes differ. */
static void dump_player(u32 frame, int slot) {
    const unsigned char* b = (const unsigned char*) &gPlayers[slot];
    unsigned n = (unsigned) sizeof(gPlayers[slot]);
    unsigned off;
    for (off = 0; off < n; off += 16) {
        char line[64]; int i, k = 0;
        for (i = 0; i < 16 && off + i < n; i++) k += snprintf(line + k, sizeof(line) - k, "%02X", b[off + i]);
        PORT_LOG("PDUMP f%u p%d +%04X %s\n", (unsigned) frame, slot, off, line);
    }
}
/* The checked state in the clear, for comparing two machines' logs by eye. */
static void log_state(const char* why, u32 frame) {
    PORT_LOG("net: state %s f%u: seed %04X gt %d ct %d gs %d menu %d race %d p0 %.2f %.2f %.2f s %.2f p1 %.2f %.2f %.2f s %.2f\n", why,
             (unsigned) frame, (unsigned) gRandomSeed16, (int) gGlobalTimer, (int) gCourseTimer, (int) gGamestate, (int) gMenuSelection,
             (int) gRaceState, gPlayers[0].pos[0], gPlayers[0].pos[1], gPlayers[0].pos[2], gPlayers[0].speed, gPlayers[1].pos[0],
             gPlayers[1].pos[1], gPlayers[1].pos[2], gPlayers[1].speed);
}

static int slot_of(const u8 id[NET_ID_LEN]) {
    int s;
    for (s = 0; s < NET_MAX_PLAYERS; s++) {
        if ((s == 0 || sKnown[s] || sRole == NET_ROLE_CLIENT) && (s < sPlayers || sRole == NET_ROLE_CLIENT) &&
            memcmp(sIds[s], id, NET_ID_LEN) == 0) return s;
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
    p.players = sCritPlayers; p.mode = sCritMode; p.cc = sCritCc;
    memcpy(p.ids[0], sHostId, NET_ID_LEN); /* the host we mean */
    send_pkt(&p);
}

static void latest_check(u32* frame, u32* sum, u32 parts[4]) {
    int i, best = -1;
    for (i = 0; i < 16; i++) {
        if (sMyCheck[i].frame != ~0u && (best < 0 || sMyCheck[i].frame > sMyCheck[best].frame)) best = i;
    }
    if (best < 0) { *frame = ~0u; *sum = 0; return; }
    *frame = sMyCheck[best].frame;
    *sum = sMyCheck[best].sum;
    memcpy(parts, sMyCheck[best].parts, sizeof(sMyCheck[best].parts));
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
    latest_check(&p.check_frame, &p.checksum, p.parts);
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
    if (sRole == NET_ROLE_CLIENT && (p->dropped & (1 << sSlot)) && sModal != MODAL_DROPPED) {
        /* The host gave up on us (a long radio gap) while we are still here:
         * everyone else plays on with our kart parked.  Stop, say so. */
        PORT_LOG("net: the host dropped us from frame %u\n", (unsigned) p->drop_frame[sSlot]);
        modal_open(MODAL_DROPPED);
        return;
    }
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
    if (p->type == PKT_ADVERT) {
        if (sRole != NET_ROLE_CLIENT || sRunning || sLobby != LOBBY_SEARCHING) return;
        if (!criteria_match(p) || memcmp(p->ids[0], from, NET_ID_LEN) != 0) return;
        if (!sHaveHost) {
            memcpy(sHostId, from, NET_ID_LEN);
            memcpy(sIds[0], from, NET_ID_LEN);
            sHaveHost = 1;
            PORT_LOG("net: found a matching race at %02X%02X%02X%02X%02X%02X\n", from[0], from[1], from[2], from[3], from[4], from[5]);
            send_hello();
            sLastHelloUs = now_us();
        } else if (memcmp(sHostId, from, NET_ID_LEN) == 0) {
            /* The host's advert lists its filled slots: are we in, or is it full without us? */
            static const u8 zero[NET_ID_LEN];
            int s, in = 0, filled = 1;
            for (s = 1; s < p->players && s < NET_MAX_PLAYERS; s++) {
                if (memcmp(p->ids[s], net_transport_local_id(), NET_ID_LEN) == 0) in = 1;
                if (memcmp(p->ids[s], zero, NET_ID_LEN) != 0) filled++;
            }
            if (in && !sJoined) { sJoined = 1; PORT_LOG("net: joined the race\n"); }
            if (!in && filled >= p->players) { /* full without us: look for another host */
                sHaveHost = 0; sJoined = 0;
                memset(sIds[0], 0, NET_ID_LEN);
                PORT_LOG("net: that race filled up without us; searching again\n");
            }
        }
        return;
    }
    if (p->type == PKT_HELLO) {
        if (sRole != NET_ROLE_HOST) return;
        if (!criteria_match(p) || memcmp(p->ids[0], sIds[0], NET_ID_LEN) != 0) return; /* another race, or not for us */
        slot = slot_of(from);
        if (slot < 0) {
            if (sRunning) return; /* no late joins */
            for (slot = 1; slot < sPlayers; slot++) {
                if (!sKnown[slot]) { memcpy(sIds[slot], from, NET_ID_LEN); sKnown[slot] = 1; break; }
            }
            if (slot >= sPlayers) return; /* full */
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
        if (sLobby != LOBBY_SEARCHING || !sHaveHost || memcmp(sHostId, from, NET_ID_LEN) != 0) return;
        sPlayers = p->players;
        memcpy(sIds, p->ids, sizeof(sIds));
        slot = slot_of(net_transport_local_id());
        if (slot < 0) { PORT_LOG("net: START without our id\n"); return; }
        sSlot = slot;
        /* The host's state at its OK press: what the character select and
         * everything after it derive from. */
        gRandomSeed16 = p->seed;
        gGlobalTimer = p->gtimer;
        gCycleFlashMenu = p->flash;
        gMenuTimingCounter = p->timing;
        PORT_LOG("net: START: %d players, we are slot %d\n", sPlayers, sSlot);
        session_begin();
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
                PORT_LOG("net: DESYNC at frame %u: ours %08X, slot %d %08X -- differs in:%s%s%s%s\n", (unsigned) p->check_frame, (unsigned) sMyCheck[idx].sum, slot, (unsigned) p->checksum,
                         sMyCheck[idx].parts[0] != p->parts[0] ? " players" : "", sMyCheck[idx].parts[1] != p->parts[1] ? " seed" : "",
                         sMyCheck[idx].parts[2] != p->parts[2] ? " timers" : "", sMyCheck[idx].parts[3] != p->parts[3] ? " gamestate" : "");
                log_state("at desync", sFrame);
                { static int dumped; if (!dumped) { dumped = 1; dump_player(sFrame, 0); dump_player(sFrame, 1); } }
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

/* ----------------------------------------------------------------------------
 * The lobby.  Opened by the game-select OK press for 2-4 players
 * (menus.c): a modal over the frozen menu -- HOST / JOIN / CANCEL.  The host
 * advertises the race it set up (players, mode, class); a joiner that set up
 * the same race finds it and asks for a slot; once the slots are full the
 * host sends START with its selections, RNG seed and timers, and every
 * machine makes the OK transition into the character select in lockstep
 * frame 0.  data/netrole.bin (scripted tests): 0x1N = choose HOST, 2..4 =
 * choose JOIN, by itself.
 * ------------------------------------------------------------------------ */
static int sChoice, sAuto, sCancelSel; /* sCancelSel: the CANCEL line is highlighted in the waiting states */
static u32 sLastAdvertUs;
static char sErr[64];

extern void func_8009E1C0(void);
extern void setup_selected_game_mode(void);
extern s8 gGameModeMenuColumn[];
extern s8 gGameModeSubMenuColumn[4][3];
extern s32 gCycleFlashMenu;
extern s32 gMenuTimingCounter;
extern s32 gMatrixEffectCount;
extern void func_80095AE0(void* m, f32 x, f32 y, f32 sx, f32 sy); /* translate + scale (menu_items.c) */

static void criteria_now(void) {
    int n = gPlayerCount < 1 ? 1 : gPlayerCount > 4 ? 4 : gPlayerCount;
    sCritPlayers = (u8) n;
    sCritMode = (u8) gModeSelection;
    sCritCc = (u8) gGameModeSubMenuColumn[n - 1][gGameModeMenuColumn[n - 1]];
}

static int criteria_match(const NetPkt* p) {
    return p->players == sCritPlayers && p->mode == sCritMode && p->cc == sCritCc;
}

static void session_reset(void) {
    int i, s;
    NetInput neutral = { 0, 0, 0, 0 };
    sRunning = 0; sFrame = 0; sDropped = 0; sStalls = 0; sLastStallLog = 0; sStallSlot = -1;
    sModal = MODAL_NONE; sHostFlags = 0; sAppliedPause = 0; sEnded = 0;
    memset(sHave, 0xFF, sizeof(sHave));
    memset(sKnown, 0, sizeof(sKnown));
    memset(sIds, 0, sizeof(sIds));
    for (i = 0; i < 16; i++) sMyCheck[i].frame = ~0u;
    for (s = 0; s < NET_MAX_PLAYERS; s++) {
        sLastReported[s] = 0; sDropFrame[s] = 0;
        for (i = 0; i < INPUT_DELAY; i++) store_input(s, (u32) i, &neutral); /* nobody's input exists for the first frames */
    }
}

/* Both machines: the OK press's transition, in lockstep frame 0.  This runs
 * inside a game iteration (the menu update), so the iteration's frame_end
 * must not count it: frame 0 begins with the next iteration's frame_begin. */
static int sBeganMidIteration;
static void session_begin(void) {
    sRunning = 1;
    sBeganMidIteration = 1;
    sLastHostPktUs = now_us();
    sLobby = LOBBY_NONE;
    PORT_LOG("net: session started: %d players, slot %d, delay %d, seed %04X timer %d\n", sPlayers, sSlot, INPUT_DELAY, gRandomSeed16, (int) gGlobalTimer);
    func_8009E1C0();
    setup_selected_game_mode();
}

static void fill_sync(NetPkt* p) {
    p->players = (u8) sPlayers;
    p->mode = sCritMode;
    p->cc = sCritCc;
    p->seed = gRandomSeed16;
    p->gtimer = gGlobalTimer;
    p->flash = gCycleFlashMenu;
    p->timing = gMenuTimingCounter;
}

static void send_advert(void) {
    NetPkt p;
    memset(&p, 0, sizeof(p));
    p.type = PKT_ADVERT;
    p.players = sCritPlayers; p.mode = sCritMode; p.cc = sCritCc;
    memcpy(p.ids, sIds, sizeof(sIds)); /* [0] is us; the filled slots let a joiner see it is in */
    send_pkt(&p);
}

static void lobby_transport(int host) {
    sCancelSel = 1; /* the waiting screens: CANCEL is the only line, selected by default */
    if (!net_transport_init(host ? NET_ROLE_HOST : NET_ROLE_CLIENT, "MK64")) {
        snprintf(sErr, sizeof(sErr), "%s", net_transport_status());
        PORT_LOG("net: transport init failed: %s\n", sErr);
        sLobby = LOBBY_ERROR;
        return;
    }
    sCancelSel = 1;
    session_reset();
    if (host) {
        sRole = NET_ROLE_HOST; sSlot = 0; sPlayers = sCritPlayers;
        memcpy(sIds[0], net_transport_local_id(), NET_ID_LEN);
        sKnown[0] = 1;
        sLobby = LOBBY_HOSTING;
    } else {
        sRole = NET_ROLE_CLIENT; sHaveHost = 0; sJoined = 0;
        sLobby = LOBBY_SEARCHING;
    }
    sLastAdvertUs = sLastHelloUs = 0;
}

static void lobby_cancel(void) {
    if (sLobby == LOBBY_HOSTING || sLobby == LOBBY_SEARCHING || sLobby == LOBBY_ERROR) {
        net_transport_term();
    }
    sRole = NET_ROLE_NONE;
    session_reset();
    sLobby = LOBBY_NONE;
    PORT_LOG("net: lobby cancelled\n");
}

void port_net_lobby_open(void) {
    FILE* f;
    criteria_now();
    sChoice = 0;
    sAuto = 0;
    f = fopen(port_save_path("netrole.bin"), "rb");
    if (f != NULL) {
        int c = fgetc(f);
        fclose(f);
        if ((c & 0xF0) == 0x10) sAuto = 1;         /* host, by itself */
        else if (c >= 2 && c <= NET_MAX_PLAYERS) sAuto = 2; /* join, by itself */
    }
    sCancelSel = 0;
    sLobby = LOBBY_CHOICE;
    PORT_LOG("net: lobby: %d players, mode %d, class %d%s\n", sCritPlayers, sCritMode, sCritCc, sAuto == 1 ? " (auto host)" : sAuto == 2 ? " (auto join)" : "");
}

int port_net_lobby_active(void) { return sLobby != LOBBY_NONE; }

void port_net_lobby_update(void) {
    u16 pressed = gControllerOne->buttonPressed | gControllerOne->stickPressed;
    u32 now = now_us();
    static int sLastLogged = -1;
    if (sLobby != sLastLogged) {
        static const char* names[] = { "none", "choice", "connect-host", "connect-join", "hosting", "searching", "error" };
        sLastLogged = sLobby;
        PORT_LOG("net: lobby -> %s\n", (unsigned) sLobby < 7 ? names[sLobby] : "?");
    }
    switch (sLobby) {
        case LOBBY_CHOICE:
            if (sAuto) { sLobby = sAuto == 1 ? LOBBY_CONNECT_HOST : LOBBY_CONNECT_JOIN; break; }
            if (pressed & (U_JPAD | D_JPAD)) {
                sChoice = (pressed & U_JPAD) ? (sChoice + 2) % 3 : (sChoice + 1) % 3;
                play_sound2(SOUND_MENU_CURSOR_MOVE);
            }
            if (pressed & B_BUTTON) { play_sound2(SOUND_MENU_GO_BACK); lobby_cancel(); break; }
            if (pressed & A_BUTTON) {
                if (sChoice == 2) { play_sound2(SOUND_MENU_GO_BACK); lobby_cancel(); }
                else { play_sound2(SOUND_MENU_OK_CLICKED); sLobby = sChoice == 0 ? LOBBY_CONNECT_HOST : LOBBY_CONNECT_JOIN; } /* one frame of "starting" text first */
            }
            break;
        case LOBBY_CONNECT_HOST:
            lobby_transport(1);
            break;
        case LOBBY_CONNECT_JOIN:
            lobby_transport(0);
            break;
        case LOBBY_HOSTING: {
            int s, n = 0;
            if (pressed & (U_JPAD | D_JPAD)) {
                int sel = (pressed & D_JPAD) ? 1 : 0;
                if (sel != sCancelSel) play_sound2(SOUND_MENU_CURSOR_MOVE);
                sCancelSel = sel;
            }
            if ((pressed & B_BUTTON) || ((pressed & A_BUTTON) && sCancelSel)) { play_sound2(SOUND_MENU_GO_BACK); lobby_cancel(); break; }
            poll();
            if (now - sLastAdvertUs >= 500000) { send_advert(); sLastAdvertUs = now; }
            for (s = 1; s < sPlayers; s++) n += sKnown[s];
            if (n == sPlayers - 1) {
                NetPkt p;
                memset(&p, 0, sizeof(p));
                p.type = PKT_START;
                memcpy(p.ids, sIds, sizeof(sIds));
                fill_sync(&p);
                p.delay = INPUT_DELAY;
                send_pkt(&p);
                session_begin();
            }
            break;
        }
        case LOBBY_SEARCHING:
            if (pressed & (U_JPAD | D_JPAD)) {
                int sel = (pressed & D_JPAD) ? 1 : 0;
                if (sel != sCancelSel) play_sound2(SOUND_MENU_CURSOR_MOVE);
                sCancelSel = sel;
            }
            if ((pressed & B_BUTTON) || ((pressed & A_BUTTON) && sCancelSel)) { play_sound2(SOUND_MENU_GO_BACK); lobby_cancel(); break; }
            poll();
            if (sHaveHost && now - sLastHelloUs >= 500000) { send_hello(); sLastHelloUs = now; }
            break;
        case LOBBY_ERROR:
            if (pressed & (A_BUTTON | B_BUTTON)) { play_sound2(SOUND_MENU_GO_BACK); lobby_cancel(); }
            break;
        default:
            break;
    }
}

static const char* mode_name(int mode) {
    switch (mode) { case 0: return "GRAND PRIX"; case 1: return "TIME TRIALS"; case 2: return "VS"; case 3: return "BATTLE"; }
    return "";
}
static const char* cc_name(int mode, int cc) {
    if (mode == 3) return "";
    switch (cc) { case 0: return "50CC"; case 1: return "100CC"; case 2: return "150CC"; default: return "EXTRA"; }
}

/* After the menu render (main.c): the modal over the frozen game select.
 * Heading 1.0, subheading 0.75, status 0.65, menu lines 0.9; everything
 * inside the box. */
#define LB_X0 49
#define LB_Y0 50
#define LB_X1 271
#define LB_Y1 226
/* A translucent quad through the same ortho projection the menu font uses
 * (draw_box goes through the 2D rectangle path, which the port stretches to
 * the full width -- it and the text would not line up). */
static Vtx sPanelQuads[2][4] __attribute__((aligned(16))); /* the display list reads them after this frame's draw code ran */
static int sPanelQuadN;
static void lobby_panel(int x0, int y0, int x1, int y1, int alpha) {
    Vtx* sQuad = sPanelQuads[sPanelQuadN++ & 1];
    Mtx* m;
    int i;
    for (i = 0; i < 4; i++) {
        sQuad[i].v.ob[0] = (short) ((i == 0 || i == 3) ? x0 : x1);
        sQuad[i].v.ob[1] = (short) ((i < 2) ? y0 : y1);
        sQuad[i].v.ob[2] = 0;
        sQuad[i].v.flag = 0;
        sQuad[i].v.tc[0] = sQuad[i].v.tc[1] = 0;
        sQuad[i].v.cn[0] = sQuad[i].v.cn[1] = sQuad[i].v.cn[2] = 0;
        sQuad[i].v.cn[3] = (unsigned char) alpha;
    }
    m = &gGfxPool->mtxEffect[gMatrixEffectCount++];
    func_80095AE0((void*) m, 0.0f, 0.0f, 1.0f, 1.0f); /* identity: the quad is in screen units already */
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(m), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gDPPipeSync(gDisplayListHead++);
    gSPClearGeometryMode(gDisplayListHead++, G_ZBUFFER | G_LIGHTING | G_CULL_BOTH | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR);
    gSPSetGeometryMode(gDisplayListHead++, G_SHADE | G_SHADING_SMOOTH);
    gSPTexture(gDisplayListHead++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(gDisplayListHead++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(sQuad), 4, 0);
    gSP2Triangles(gDisplayListHead++, 0, 1, 2, 0, 0, 2, 3, 0);
    gDPPipeSync(gDisplayListHead++);
}
static void lobby_line(int y, const char* text, f32 scale, int colour) {
    set_text_color(colour);
    print_text1_center_mode_1((LB_X0 + LB_X1) / 2, y, (char*) text, 1, scale, scale);
}
/* Menu lines are a left-aligned column like the OPTION screen: the selected
 * one cycles colour and gets the spinning diamond (func_800A66A8, the same
 * cursor the OPTION screen draws; it wants a MenuItem for its spin rate). */
#define LB_CX ((LB_X0 + LB_X1) / 2)
static MenuItem sCursorItem;
/* The width print_text1 would centre with (glyph widths at the item scale). */
static int lobby_text_width(const char* text, f32 scaleX) {
    char* p = (char*) text;
    int w = 0;
    while (*p != 0) {
        s32 g = char_to_glyph_index(p);
        if (g >= 0) w += (int) (gGlyphDisplayWidth[g] * scaleX);
        else if (g == -1) w += (int) (7 * scaleX);
        else break;
        p += g >= 0x30 ? 2 : 1;
    }
    return w;
}
static void lobby_item(int y, const char* text, int selected) {
    set_text_color(selected ? TEXT_BLUE_GREEN_RED_CYCLE_2 : TEXT_BLUE);
    print_text1_center_mode_1(LB_CX, y, (char*) text, 0, 0.9f, 1.0f);
}
static void lobby_cursor(int y, const char* text) {
    Unk_D_800E70A0 at;
    at.column = (s16) (LB_CX - lobby_text_width(text, 0.9f) / 2 - 10); /* left of the first glyph (the model is centred on its origin) */
    at.row = (s16) (y - 9);                                              /* on the text's middle (the row is the baseline) */
    at.pad0 = at.pad1 = 0;
    func_800A66A8(&sCursorItem, &at);
}
void port_net_lobby_draw(void) {
    char line[48];
    int s, n = 0, cursorY = -1; /* the diamond goes on the selected line, drawn last */
    const char* cursorText = "";
    if (sLobby == LOBBY_NONE) return;
#ifdef PORT_INPUT_SCRIPT
    { /* debug: one screenshot per lobby state (a frame after it first draws) */
        static int shot[8], seen[8];
        if (seen[sLobby]++ == 3 && !shot[sLobby]) { shot[sLobby] = 1; port_screenshot(8000 + sLobby); }
    }
#endif
    gDisplayListHead = draw_box(gDisplayListHead, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 0, 0x90); /* dim the menu */
    sPanelQuadN = 0;
    lobby_panel(LB_X0, LB_Y0, LB_X1, LB_Y1, 0xF4);
    lobby_line(LB_Y0 + 34, "AD HOC PLAY", 1.0f, TEXT_YELLOW);
    snprintf(line, sizeof(line), "%dP %s %s", sCritPlayers, mode_name(sCritMode), cc_name(sCritMode, sCritCc));
    lobby_line(LB_Y0 + 56, line, 0.75f, TEXT_RED);
    switch (sLobby) {
        case LOBBY_CHOICE: {
            static const char* items[3] = { "HOST RACE", "JOIN RACE", "CANCEL" };
            for (s = 0; s < 3; s++) {
                lobby_item(LB_Y0 + 90 + s * 22, items[s], s == sChoice);
            }
            cursorY = LB_Y0 + 90 + sChoice * 22;
            cursorText = items[sChoice];
            break;
        }
        case LOBBY_CONNECT_HOST:
        case LOBBY_CONNECT_JOIN:
            lobby_line(LB_Y0 + 104, "STARTING WLAN...", 0.65f, TEXT_PORT_GREY_PULSE);
            break;
        case LOBBY_HOSTING:
            for (s = 1; s < sPlayers; s++) n += sKnown[s];
            snprintf(line, sizeof(line), "WAITING FOR %d PLAYER%s", sPlayers - 1 - n, sPlayers - 1 - n == 1 ? "" : "S");
            lobby_line(LB_Y0 + 94, line, 0.65f, TEXT_PORT_GREY_PULSE);
            lobby_item(LB_Y0 + 138, "CANCEL", sCancelSel);
            if (sCancelSel) { cursorY = LB_Y0 + 138; cursorText = "CANCEL"; }
            break;
        case LOBBY_SEARCHING:
            lobby_line(LB_Y0 + 94, sJoined ? "JOINED RACE" : sHaveHost ? "JOINING..." : "SEARCHING...", 0.65f, TEXT_PORT_GREY_PULSE);
            lobby_item(LB_Y0 + 138, "CANCEL", sCancelSel);
            if (sCancelSel) { cursorY = LB_Y0 + 138; cursorText = "CANCEL"; }
            break;
        case LOBBY_ERROR:
            lobby_line(LB_Y0 + 90, "WLAN FAILED", 0.9f, TEXT_RED);
            lobby_line(LB_Y0 + 110, sErr, 0.55f, TEXT_BLUE);
            lobby_item(LB_Y0 + 138, "BACK", 1);
            cursorY = LB_Y0 + 138;
            cursorText = "BACK";
            break;
    }
    if (cursorY >= 0) lobby_cursor(cursorY, cursorText);
}

/* ---- the drop-out prompts ------------------------------------------------ */

/* The race's own pause (what START does), when the race allows it; in the
 * menus the overlay freezes them instead (main.c). */
static int race_can_pause(void) { return gGamestate == RACING && gRaceState < RACE_HUMAN_FINISHED && !gIsInQuitToMenuTransition; }
static void net_pause(int on) {
    if (on && gIsGamePaused == 0 && race_can_pause()) {
        func_8028DF00();
        gIsGamePaused = 1;
        func_800C9F90(1);
        gPauseTriggered = 1;
    } else if (!on && gIsGamePaused != 0 && gGamestate == RACING) {
        gIsGamePaused = 0;
        func_8028DF38();
        func_800C9F90(0);
    }
}

static void modal_open(int which) {
    OSContPad pad;
    port_local_pad(&pad);
    sModalPrevButtons = pad.button; /* a button already down does not count */
    sModal = which;
    sModalSel = 0;
    if (which == MODAL_HOST_DROP) {
        sHostFlags |= NETIN_PAUSE; /* pauses everyone when that input's frame comes round */
    } else if (which == MODAL_HOST_GONE || which == MODAL_DROPPED) {
        sEnded = 1; /* no more lockstep; the view and the pads stay as they were until MAIN MENU */
        net_pause(1);
    }
    PORT_LOG("net: prompt %d\n", which);
}

/* Leave the session and go to the main menu (the pause menu's QUIT path in
 * the race; its own transition in the menus). */
static void end_session_to_menu(void) {
    sModal = MODAL_NONE;
    sRunning = 0;
    sRole = NET_ROLE_NONE;
    net_transport_term();
    if (gGamestate == RACING) {
        gIsGamePaused = 0;
        func_80290338();
    } else {
        gGamestateNext = MAIN_MENU_FROM_QUIT;
        gGamestate = 255;
        gIsInQuitToMenuTransition = 0;
        gQuitToMenuTransitionCounter = 0;
        gFadeModeSelection = FADE_MODE_MAIN;
        gMenuSelection = MAIN_MENU;
    }
    PORT_LOG("net: session over, to the main menu\n");
}

int port_net_modal_active(void) { return sModal != MODAL_NONE; }

void port_net_modal_update(void) {
    OSContPad pad;
    u16 pressed;
    if (sModal == MODAL_NONE) return;
    port_local_pad(&pad);
    pressed = pad.button & ~sModalPrevButtons;
    sModalPrevButtons = pad.button;
    switch (sModal) {
        case MODAL_HOST_DROP:
            if (pressed & (U_JPAD | D_JPAD)) { sModalSel ^= 1; play_sound2(SOUND_MENU_CURSOR_MOVE); }
            if (pressed & A_BUTTON) {
                if (sModalSel == 0) { play_sound2(SOUND_MENU_OK_CLICKED); sHostFlags &= ~NETIN_PAUSE; sModal = MODAL_RESUMING; }
                else { play_sound2(SOUND_MENU_GO_BACK); sHostFlags = NETIN_END; sModal = MODAL_LEAVING; }
            }
            break;
        case MODAL_HOST_GONE:
        case MODAL_DROPPED:
            if (pressed & (A_BUTTON | START_BUTTON)) { play_sound2(SOUND_MENU_OK_CLICKED); end_session_to_menu(); }
            break;
        default:
            break;
    }
}

#define MD_X0 64
#define MD_Y0 78
#define MD_X1 256
#define MD_Y1 176
static void modal_line(int y, const char* text, f32 scale, int colour) {
    set_text_color(colour);
    print_text1_center_mode_1((MD_X0 + MD_X1) / 2, y, (char*) text, 1, scale, scale);
}
static void modal_item(int y, const char* text, int selected) {
    set_text_color(selected ? TEXT_BLUE_GREEN_RED_CYCLE_2 : TEXT_BLUE);
    print_text1_center_mode_1((MD_X0 + MD_X1) / 2, y, (char*) text, 0, 0.9f, 1.0f);
    if (selected) {
        Unk_D_800E70A0 at;
        at.column = (s16) ((MD_X0 + MD_X1) / 2 - lobby_text_width(text, 0.9f) / 2 - 10);
        at.row = (s16) (y - 9);
        at.pad0 = at.pad1 = 0;
        func_800A66A8(&sCursorItem, &at);
    }
}
void port_net_modal_draw(void) {
    char line[40];
    if (sModal == MODAL_NONE || sModal == MODAL_RESUMING || sModal == MODAL_LEAVING) return;
    gDisplayListHead = draw_box(gDisplayListHead, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 0, 0x70);
    sPanelQuadN = 0;
    lobby_panel(MD_X0, MD_Y0, MD_X1, MD_Y1, 0xF4);
    snprintf(line, sizeof(line), "PLAYER %d LEFT THE RACE", sModalSlot + 1);
    switch (sModal) {
        case MODAL_HOST_DROP:
            modal_line(MD_Y0 + 30, line, 0.7f, TEXT_RED);
            modal_item(MD_Y0 + 62, "CONTINUE", sModalSel == 0);
            modal_item(MD_Y0 + 84, "EXIT", sModalSel == 1);
            break;
        case MODAL_WAIT_HOST:
            modal_line(MD_Y0 + 30, line, 0.7f, TEXT_RED);
            modal_line(MD_Y0 + 66, "WAITING FOR HOST...", 0.65f, TEXT_PORT_GREY_PULSE);
            break;
        case MODAL_HOST_GONE:
            modal_line(MD_Y0 + 30, "HOST EXITED THE GAME", 0.7f, TEXT_RED);
            modal_item(MD_Y0 + 74, "MAIN MENU", 1);
            break;
        case MODAL_DROPPED:
            modal_line(MD_Y0 + 24, "YOU WERE DROPPED", 0.7f, TEXT_RED);
            modal_line(MD_Y0 + 42, "FROM THE RACE", 0.7f, TEXT_RED);
            modal_item(MD_Y0 + 74, "MAIN MENU", 1);
            break;
    }
}

int port_net_boot(void) { return 1; } /* the session starts from the game select now */

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
    if (sEnded) { /* the prompt over the frozen race: our pad only, no network */
        if (sSampled != sFrame) {
            sSampled = sFrame;
            port_local_pad(&pad);
            in.button = pad.button; in.sx = pad.stick_x; in.sy = pad.stick_y; in.flags = 0;
            store_input(sSlot, sFrame, &in);
        }
        return 1;
    }
    poll();
    if (sRole == NET_ROLE_HOST && sFrame < 90) send_start(); /* cover a lost START */
    /* Our input for frame F + delay: sampled once per frame (a stall retry
     * must not re-read the pad -- the debug input script counts frames by it). */
    if (sSampled != sFrame) {
        sSampled = sFrame;
        sJustSampled = 1;
        port_local_pad(&pad);
        in.button = pad.button; in.sx = pad.stick_x; in.sy = pad.stick_y;
        in.flags = sRole == NET_ROLE_HOST ? sHostFlags : 0;
        store_input(sSlot, sFrame + INPUT_DELAY, &in);
        if ((sFrame % 60) == 0) { extern u32 port_log_cost_ms(void); u32 lm = port_log_cost_ms(); PORT_LOG("net: frame %u (%u stalls) %s logms %u\n", (unsigned) sFrame, (unsigned) sStalls, net_transport_stats(), (unsigned) lm); }
    }
    if ((sFrame % CHECK_EVERY) == 0) {
        int idx = (sFrame / CHECK_EVERY) % 16;
        sMyCheck[idx].frame = sFrame;
        sMyCheck[idx].sum = state_checksum(sMyCheck[idx].parts);
        if (sFrame <= 90 || (sFrame % 600) == 0) log_state("check", sFrame); /* the first four checks, then every 20 s */
        if (sFrame == 600 || sFrame == 900 || sFrame == 1200 || sFrame == 1500 || sFrame == 1800) {
            dump_player(sFrame, 0); dump_player(sFrame, 1); /* checkpoints to diff two machines' state before the drift reaches position */
        }
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
            if (s == sSlot) { PORT_LOG("net: BUG: no local input for frame %u\n", (unsigned) sFrame); store_input(s, sFrame, &in); continue; }
            if (sStallSlot != s) { sStallSlot = s; sStallSinceUs = now; }
            sStalls++;
            if (sStalls - sLastStallLog >= 200) { sLastStallLog = sStalls; PORT_LOG("net: frame %u waiting for slot %d (%u stalls so far, %u ms) %s\n", (unsigned) sFrame, s, (unsigned) sStalls, (unsigned) ((now - sStallSinceUs) / 1000u), net_transport_stats()); }
            if (sRole == NET_ROLE_HOST && now - sStallSinceUs >= DROP_AFTER_US) {
                drop_slot(s, sFrame, "no input"); /* from this frame on: neutral, on every machine */
                sModalSlot = s;
                if (sModal == MODAL_NONE || sModal == MODAL_RESUMING) modal_open(MODAL_HOST_DROP); /* pause everyone, ask */
                continue;
            }
            if (sRole == NET_ROLE_CLIENT && now - sLastHostPktUs >= GIVEUP_AFTER_US) {
                PORT_LOG("net: lost the host at frame %u\n", (unsigned) sFrame);
                modal_open(MODAL_HOST_GONE); /* the session is over: MAIN MENU is all that is left */
                return 1;
            }
            return 0;
        }
    }
    sStallSlot = -1;
    { /* the host's flags for this frame, applied on every machine at this same frame */
        const NetInput* h = &sRing[0][sFrame & (RING - 1)];
        int pause = (h->flags & NETIN_PAUSE) != 0;
        if (h->flags & NETIN_END) {
            PORT_LOG("net: the host ended the session at frame %u\n", (unsigned) sFrame);
            if (sRole == NET_ROLE_HOST) { send_inputs(); send_inputs(); end_session_to_menu(); }
            else modal_open(MODAL_HOST_GONE);
            return 1;
        }
        if (pause != sAppliedPause) {
            sAppliedPause = pause;
            net_pause(pause);
            if (sRole == NET_ROLE_CLIENT) sModal = pause ? MODAL_WAIT_HOST : MODAL_NONE;
            else if (!pause && sModal == MODAL_RESUMING) sModal = MODAL_NONE;
        }
    }
    return 1;
}

void port_net_pads(OSContPad* pads) {
    int s;
    for (s = 0; s < NET_MAX_PLAYERS; s++) {
        if (s < sPlayers && !is_dropped(s, sFrame) && (!sEnded || s == sSlot)) {
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
    if (sBeganMidIteration) { sBeganMidIteration = 0; return; }
    sFrame++;
}
#endif
