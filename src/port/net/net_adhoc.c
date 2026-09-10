/**
 * PSP ad hoc transport: one PDP socket, packets broadcast to the group.  The
 * kernel modules are loaded at init and every step logs the free memory, so a
 * PSP-1000 run doubles as the memory probe (docs/adhoc.md).
 */
#if defined(PORT_NET) && defined(PORT_NET_ADHOC)
#include <ultra64.h>
#include <macros.h>
#include <stdio.h>
#include <string.h>
#include <pspkernel.h>
#include <pspsysmem.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <pspnet.h>
#include <pspnet_adhoc.h>
#include <pspnet_adhocctl.h>
#include <pspwlan.h>
#include "port_net.h"
#include "../port.h"

#define ADHOC_PORT 0x4D4B /* 'MK' */
#define ADHOC_BUFSIZE 0x2000
#define ADHOC_GROUP "MK64"

static int sPdp = -1;
static u8 sMac[NET_ID_LEN];
static char sStatus[96];
static int sLoaded[2];

static void log_mem(const char* where) {
    PORT_LOG("net: %s: free %u KB, largest block %u KB\n", where, (unsigned) sceKernelTotalFreeMemSize() / 1024u,
             (unsigned) sceKernelMaxFreeMemSize() / 1024u);
}

static void fail(const char* what, int rc) {
    snprintf(sStatus, sizeof(sStatus), "%s failed (%08X)", what, (unsigned) rc);
    PORT_LOG("net: %s\n", sStatus);
}

int net_transport_init(UNUSED int role, const char* group) {
    int rc, state = 0, tries;
    struct productStruct product;
    log_mem("before net modules");
    if (sceWlanGetSwitchState() != 1) { fail("WLAN switch is off", 0); return 0; }
    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (rc < 0) { fail("load NET_COMMON", rc); return 0; }
    sLoaded[0] = 1;
    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_ADHOC);
    if (rc < 0) { fail("load NET_ADHOC", rc); return 0; }
    sLoaded[1] = 1;
    log_mem("after net modules");
    rc = sceNetInit(0x20000, 0x20, 0x1000, 0x20, 0x1000); /* 128 KB pool */
    if (rc < 0) { fail("sceNetInit", rc); return 0; }
    rc = sceNetAdhocInit();
    if (rc < 0) { fail("sceNetAdhocInit", rc); return 0; }
    memset(&product, 0, sizeof(product));
    product.unknown = 0;
    memcpy(product.product, "MK64PORTA", 9);
    rc = sceNetAdhocctlInit(0x2000, 0x20, &product);
    if (rc < 0) { fail("sceNetAdhocctlInit", rc); return 0; }
    log_mem("after net init");
    rc = sceNetAdhocctlConnect(group != NULL ? group : ADHOC_GROUP);
    if (rc < 0) { fail("sceNetAdhocctlConnect", rc); return 0; }
    for (tries = 0; tries < 300; tries++) { /* ~15 s */
        sceNetAdhocctlGetState(&state);
        if (state == 1) break; /* ADHOCCTL_STATE_CONNECTED */
        sceKernelDelayThread(50 * 1000);
    }
    if (state != 1) { fail("adhocctl connect timeout", state); return 0; }
    sceWlanGetEtherAddr(sMac);
    sPdp = sceNetAdhocPdpCreate(sMac, ADHOC_PORT, ADHOC_BUFSIZE, 0);
    if (sPdp < 0) { fail("sceNetAdhocPdpCreate", sPdp); return 0; }
    log_mem("after adhoc connect");
    snprintf(sStatus, sizeof(sStatus), "ad hoc group %s, %02X:%02X:%02X:%02X:%02X:%02X", group != NULL ? group : ADHOC_GROUP,
             sMac[0], sMac[1], sMac[2], sMac[3], sMac[4], sMac[5]);
    PORT_LOG("net: %s\n", sStatus);
    return 1;
}

void net_transport_term(void) {
    if (sPdp >= 0) { sceNetAdhocPdpDelete(sPdp, 0); sPdp = -1; }
    sceNetAdhocctlDisconnect();
    sceNetAdhocctlTerm();
    sceNetAdhocTerm();
    sceNetTerm();
    if (sLoaded[1]) sceUtilityUnloadNetModule(PSP_NET_MODULE_ADHOC);
    if (sLoaded[0]) sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
}

const u8* net_transport_local_id(void) {
    return sMac;
}

int net_transport_send(const void* pkt, int len) {
    static u8 bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    int rc = sceNetAdhocPdpSend(sPdp, bcast, ADHOC_PORT, (void*) pkt, (unsigned) len, 0, 1);
    return rc < 0 ? rc : len;
}

int net_transport_recv(void* buf, int max, u8 from[NET_ID_LEN]) {
    unsigned short port;
    int len = max;
    int rc = sceNetAdhocPdpRecv(sPdp, from, &port, buf, &len, 0, 1);
    if (rc < 0) return 0; /* nothing pending (0x80410709) or an error: treat as no data */
    return len;
}

const char* net_transport_status(void) {
    return sStatus;
}
#endif
