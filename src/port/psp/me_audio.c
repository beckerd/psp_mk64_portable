/**
 * The mixer on the PSP's Media Engine (port/audio/mix_jobs.h).
 *
 * The ME is a second MIPS core with its own caches and no operating system.
 * libme-core (mcidclan's me-core-mapper, with its kernel helper embedded)
 * boots it into meLibOnProcess() below, which runs straight out of our image:
 * a loop that takes mixer jobs off the shared ring.  Rules on that side: no
 * system call, no VFPU, and main-RAM data only through the uncached alias --
 * the main CPU writes back its data cache before it hands a job over.  The
 * mixer's own work area and tables are touched by the ME alone once it runs,
 * so they stay cached there.
 *
 * Built with -DPORT_ME_AUDIO.  Never run on hardware as of this commit.
 */
#include <stdint.h>
#include "../audio/mix_jobs.h"
#include "../port.h"

#ifdef PORT_ME_AUDIO
#include <me-core-mapper/me-core.h>

static int sStarted;

void meLibOnProcess(void) {
    MixShared* sh = (MixShared*) ((uintptr_t) &gMixShared | 0x40000000u);
    sh->alive = 1;
    while (!sh->stop) {
        uint32_t done = sh->completed;
        if (done != sh->submitted) {
            mix_job_execute((const MixJob*) ((uintptr_t) &gMixJobs[done % MIXJ_JOBS] | 0x40000000u), 0x40000000u);
            sh->completed = done + 1;
        } else {
            meLibDelayPipeline();
        }
    }
    sh->alive = 0;
    for (;;) { /* parked until the main CPU puts this core back into reset (port_me_stop) */
        meLibDelayPipeline();
    }
}

int port_me_start(void) {
    extern unsigned int port_time_us(void);
    extern int sceKernelDelayThread(unsigned int us);
    MixShared* sh = (MixShared*) ((uintptr_t) &gMixShared | 0x40000000u);
    unsigned int t0;
    sh->alive = 0; sh->stop = 0;
    sceKernelDcacheWritebackInvalidateAll(); /* our image and data as the ME will read them */
    if (meLibDefaultInit() < 0) {
        PORT_LOG("me: init failed (the kernel helper did not load)\n");
        return 0;
    }
    t0 = port_time_us();
    while (!sh->alive) { /* an emulator, or a firmware that keeps the ME: never answers */
        if (port_time_us() - t0 > 500000u) {
            PORT_LOG("me: no answer from the Media Engine\n");
            return 0;
        }
        sceKernelDelayThread(1000);
    }
    PORT_LOG("me: the Media Engine is running the mixer\n");
    sStarted = 1;
    return 1;
}

void port_me_stop(void) {
    if (sStarted) {
        extern unsigned int port_time_us(void);
        MixShared* sh = (MixShared*) ((uintptr_t) &gMixShared | 0x40000000u);
        unsigned int t0 = port_time_us();
        sh->stop = 1;
        while (sh->alive && port_time_us() - t0 < 50000u) {
        }
        meLibHalt(); /* hold the ME in reset: it must not run our image once we are gone */
        sStarted = 0;
    }
}
#else
int port_me_start(void) { return 0; }
void port_me_stop(void) {}
#endif
