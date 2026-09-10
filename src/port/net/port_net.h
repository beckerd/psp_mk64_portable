/**
 * Ad hoc multiplayer for the PSP port: every machine runs the whole game and
 * they exchange controller inputs in lockstep (see docs/adhoc.md).
 *
 *   transport   net_file.c   mailbox files on the shared memory stick (PPSSPP
 *                            tests: two instances on one host), -DPORT_NET_FILE
 *               net_adhoc.c  PSP ad hoc PDP broadcast, -DPORT_NET_ADHOC
 *   session     lockstep.c   slots, input delay, redundancy, state checksums
 *
 * Everything is compiled only with -DPORT_NET.
 */
#ifndef PORT_NET_H
#define PORT_NET_H

#include <PR/ultratypes.h>
#include <PR/os_cont.h>

#define NET_MAX_PLAYERS 4
#define NET_ID_LEN 6 /* MAC address (ad hoc) or a fixed name (files) */

enum { NET_ROLE_NONE = 0, NET_ROLE_HOST = 1, NET_ROLE_CLIENT = 2 };

/* Transport ---------------------------------------------------------------- */
/* role: NET_ROLE_HOST or a client number 2..NET_MAX_PLAYERS. */
int net_transport_init(int role, const char* group);
void net_transport_term(void);
const u8* net_transport_local_id(void);
int net_transport_send(const void* pkt, int len);                /* to every peer */
int net_transport_recv(void* buf, int max, u8 from[NET_ID_LEN]); /* bytes, 0 = nothing pending */
const char* net_transport_status(void);                          /* one line for the waiting screen */

/* Session ------------------------------------------------------------------ */
/* Before port_game_init(): pick the role (L held at boot = host, R = join, or
 * data/netrole.bin: 1 host, 2 join), connect and wait for the session to
 * start.  Returns the player count (1 = no network session). */
int port_net_boot(void);
int port_net_active(void);
int port_net_players(void);
int port_net_local_slot(void);
/* Once per game iteration, before the game reads the pads: 1 = every slot's
 * input for this frame is here, 0 = stall (call again after a short delay). */
int port_net_frame_begin(void);
/* Fill pads[0..3] for the current frame (osContGetReadData; any number of times). */
void port_net_pads(OSContPad* pads);
/* After the game iteration: the next frame begins. */
void port_net_frame_end(void);
/* The local pad as the game would read it without a session (shim). */
void port_local_pad(OSContPad* pad);

/* Game side (skybox_and_splitscreen.c): draw the local slot's camera full screen
 * in the single-player layout; install its HUD coordinates after init_hud(). */
void port_render_local_player(void);
void port_net_hud_layout(void);

#endif
