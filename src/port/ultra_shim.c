/**
 * libultra replacement for the PSP port.
 *
 * The game runs single-threaded: the port's main loop calls the game loop
 * directly, so message queues never block (osRecvMesg returns -1 when empty),
 * DMAs are memcpys (the "ROM" is linked into the executable), the VI/AI/SP
 * calls are no-ops or hooks into the port backends, and EEPROM is a file.
 */
#include <ultra64.h>
#include <PR/os.h>
#include <macros.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "port.h"

#include <pspkernel.h>
#include <psprtc.h>

/* ------------------------------------------------------------------------- */
/* Globals libultra normally provides                                          */
/* ------------------------------------------------------------------------- */

u64 osClockRate = 62500000;
u32 osTvType = 1; // OS_TV_TYPE_NTSC
u32 osResetType = 0; // cold boot
s32 osAppNmiBuffer[16];
OSViMode osViModeTable[32];

/* Referenced by src/os/math (sinf/cosf). */
typedef union {
    int i;
    float f;
} port_fu;
#undef NAN
const port_fu NAN = { 0x7f810000 };

/* ------------------------------------------------------------------------- */
/* Init / threads / timers                                                      */
/* ------------------------------------------------------------------------- */

void osInitialize(void) {
}

void osCreateThread(UNUSED OSThread* thread, UNUSED OSId id, UNUSED void (*entry)(void*), UNUSED void* arg,
                    UNUSED void* sp, UNUSED OSPri pri) {
}
void osStartThread(UNUSED OSThread* thread) {
}
void osDestroyThread(UNUSED OSThread* thread) {
}
void osYieldThread(void) {
}
void osSetThreadPri(UNUSED OSThread* thread, UNUSED OSPri pri) {
}
OSPri osGetThreadPri(UNUSED OSThread* thread) {
    return 0;
}
OSThread* __osGetCurrFaultedThread(void) {
    return NULL;
}

/* osGetTime counts at osClockRate (62.5 MHz); derive it from the system
 * microsecond clock so time deltas stay meaningful. */
static u64 sTimeBase;

OSTime osGetTime(void) {
    u64 now = sceKernelGetSystemTimeWide(); // microseconds
    return (OSTime) ((now - sTimeBase) * 62.5);
}

void osSetTime(OSTime time) {
    sTimeBase = sceKernelGetSystemTimeWide() - (u64) (time / 62.5);
}

u32 osGetCount(void) {
    // 46.875 MHz on the N64; system clock is 1 MHz, scale to keep ratios sane.
    return (u32) (sceKernelGetSystemTimeWide() * 46);
}

u32 osSetTimer(UNUSED OSTimer* timer, UNUSED OSTime countdown, UNUSED OSTime interval, UNUSED OSMesgQueue* mq,
               UNUSED OSMesg msg) {
    return 0;
}

void osSetEventMesg(UNUSED OSEvent e, UNUSED OSMesgQueue* mq, UNUSED OSMesg msg) {
}

/* ------------------------------------------------------------------------- */
/* Message queues (never block)                                               */
/* ------------------------------------------------------------------------- */

void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msgBuf, s32 count) {
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msgBuf;
}

s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, UNUSED s32 flag) {
    s32 index;
    if (mq->validCount >= mq->msgCount) {
        return -1;
    }
    index = (mq->first + mq->validCount) % mq->msgCount;
    mq->msg[index] = msg;
    mq->validCount++;
    return 0;
}

s32 osJamMesg(OSMesgQueue* mq, OSMesg msg, UNUSED s32 flag) {
    if (mq->validCount >= mq->msgCount) {
        return -1;
    }
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;
    return 0;
}

s32 osRecvMesg(OSMesgQueue* mq, OSMesg* msg, UNUSED s32 flag) {
    if (mq->validCount == 0) {
        return -1;
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Cache / memory                                                             */
/* ------------------------------------------------------------------------- */

void osInvalDCache(UNUSED void* a, UNUSED size_t b) {
}
void osInvalICache(UNUSED void* a, UNUSED size_t b) {
}
void osWritebackDCache(UNUSED void* a, UNUSED size_t b) {
}
void osWritebackDCacheAll(void) {
}
uintptr_t osVirtualToPhysical(void* addr) {
    return (uintptr_t) addr;
}

/* ------------------------------------------------------------------------- */
/* "PI" DMA: the cartridge is in memory                                       */
/* ------------------------------------------------------------------------- */

void osCreatePiManager(UNUSED OSPri pri, UNUSED OSMesgQueue* cmdQ, UNUSED OSMesg* cmdBuf, UNUSED s32 cmdMsgCnt) {
}

s32 osPiStartDma(UNUSED OSIoMesg* mb, UNUSED s32 priority, UNUSED s32 direction, uintptr_t devAddr, void* vAddr,
                 size_t nbytes, OSMesgQueue* mq) {
    if (nbytes != 0) {
        memcpy(vAddr, (const void*) devAddr, nbytes);
    }
    if (mq != NULL) {
        osSendMesg(mq, NULL, OS_MESG_NOBLOCK); // completion message
    }
    return 0;
}

s32 osEPiStartDma(UNUSED OSPiHandle* pihandle, OSIoMesg* mb, s32 direction) {
    return osPiStartDma(mb, 0, direction, mb->devAddr, mb->dramAddr, mb->size, mb->hdr.retQueue);
}

OSPiHandle* osCartRomInit(void) {
    static OSPiHandle handle;
    return &handle;
}

/* ------------------------------------------------------------------------- */
/* VI                                                                         */
/* ------------------------------------------------------------------------- */

void osCreateViManager(UNUSED OSPri pri) {
}
void osViSetMode(UNUSED OSViMode* mode) {
}
void osViSetEvent(UNUSED OSMesgQueue* mq, UNUSED OSMesg msg, UNUSED u32 retraceCount) {
}
void osViBlack(UNUSED u8 active) {
}
void osViSetSpecialFeatures(UNUSED u32 func) {
}
void osViSwapBuffer(UNUSED void* vaddr) {
}

/* ------------------------------------------------------------------------- */
/* SP tasks: graphics go through port_gfx_run(), audio through the mixer      */
/* ------------------------------------------------------------------------- */

void osSpTaskLoad(UNUSED OSTask* task) {
}
void osSpTaskStartGo(UNUSED OSTask* task) {
}
void osSpTaskYield(void) {
}
OSYieldResult osSpTaskYielded(UNUSED OSTask* task) {
    return 0;
}

/* ------------------------------------------------------------------------- */
/* AI                                                                         */
/* ------------------------------------------------------------------------- */

extern void port_audio_out_push(const s16* samples, u32 bytes);
extern u32 port_audio_out_queued_bytes(void);

s32 osAiSetFrequency(u32 freq) {
    extern void port_audio_out_set_rate(u32 freq);
    port_audio_out_set_rate(freq); // the output stage resamples to the PSP rate
    return (s32) freq;
}
s32 osAiSetNextBuffer(void* buf, u32 size) {
    port_audio_out_push((const s16*) buf, size);
    return 0;
}
u32 osAiGetLength(void) {
    return port_audio_out_queued_bytes();
}

/* ------------------------------------------------------------------------- */
/* Controllers                                                                */
/* ------------------------------------------------------------------------- */

extern void controller_psp_init(void);
extern void controller_psp_read(OSContPad* pad);

s32 osContInit(UNUSED OSMesgQueue* mq, u8* bitpattern, OSContStatus* status) {
    controller_psp_init();
    *bitpattern = 1; // one controller plugged in
    status[0].type = CONT_TYPE_NORMAL;
    status[0].status = 0;
    status[0].errnum = 0;
    status[1].errnum = status[2].errnum = status[3].errnum = CONT_NO_RESPONSE_ERROR;
    return 0;
}

s32 osContStartReadData(OSMesgQueue* mq) {
    osSendMesg(mq, NULL, OS_MESG_NOBLOCK);
    return 0;
}

void osContGetReadData(OSContPad* pad) {
    controller_psp_read(&pad[0]);
#ifdef PORT_INPUT_SCRIPT
    {
        extern void port_input_script(OSContPad* pad);
        port_input_script(&pad[0]);
    }
#endif
    pad[1].button = pad[2].button = pad[3].button = 0;
    pad[1].stick_x = pad[2].stick_x = pad[3].stick_x = 0;
    pad[1].stick_y = pad[2].stick_y = pad[3].stick_y = 0;
    pad[1].errno = pad[2].errno = pad[3].errno = CONT_NO_RESPONSE_ERROR;
}

/* ------------------------------------------------------------------------- */
/* EEPROM: a 512 byte file next to the EBOOT                                  */
/* ------------------------------------------------------------------------- */

#define EEPROM_FILE PORT_SAVE_DIR "eeprom.bin"
#define EEPROM_SIZE 512

static u8 sEeprom[EEPROM_SIZE];
static u8 sEepromLoaded;

static void eeprom_load(void) {
    FILE* fp;
    if (sEepromLoaded) {
        return;
    }
    sEepromLoaded = 1;
    memset(sEeprom, 0, sizeof(sEeprom));
    fp = fopen(EEPROM_FILE, "rb");
    if (fp != NULL) {
        fread(sEeprom, 1, sizeof(sEeprom), fp);
        fclose(fp);
    }
}

static s32 eeprom_save(void) {
    FILE* fp = fopen(EEPROM_FILE, "wb");
    if (fp == NULL) {
        return -1;
    }
    fwrite(sEeprom, 1, sizeof(sEeprom), fp);
    fclose(fp);
    return 0;
}

s32 osEepromProbe(UNUSED OSMesgQueue* mq) {
    return EEPROM_TYPE_4K;
}

s32 osEepromLongRead(UNUSED OSMesgQueue* mq, u8 address, u8* buffer, int nbytes) {
    eeprom_load();
    if (address * 8 + nbytes > EEPROM_SIZE) {
        return -1;
    }
    memcpy(buffer, sEeprom + address * 8, nbytes);
    return 0;
}

s32 osEepromLongWrite(UNUSED OSMesgQueue* mq, u8 address, u8* buffer, int nbytes) {
    eeprom_load();
    if (address * 8 + nbytes > EEPROM_SIZE) {
        return -1;
    }
    memcpy(sEeprom + address * 8, buffer, nbytes);
    return eeprom_save();
}

s32 osEepromRead(OSMesgQueue* mq, u8 address, u8* buffer) {
    return osEepromLongRead(mq, address, buffer, 8);
}

s32 osEepromWrite(OSMesgQueue* mq, u8 address, u8* buffer) {
    return osEepromLongWrite(mq, address, buffer, 8);
}

/* ------------------------------------------------------------------------- */
/* Controller Pak (ghost data): not present                                   */
/* ------------------------------------------------------------------------- */

s32 osPfsInit(UNUSED OSMesgQueue* mq, UNUSED OSPfs* pfs, UNUSED int channel) {
    return PFS_ERR_NOPACK;
}
s32 osPfsIsPlug(UNUSED OSMesgQueue* mq, u8* pattern) {
    *pattern = 0;
    return 0;
}
s32 osPfsFindFile(UNUSED OSPfs* pfs, UNUSED u16 company, UNUSED u32 game, UNUSED u8* gameName, UNUSED u8* extName,
                  UNUSED s32* fileNo) {
    return PFS_ERR_NOPACK;
}
s32 osPfsAllocateFile(UNUSED OSPfs* pfs, UNUSED u16 company, UNUSED u32 game, UNUSED u8* gameName,
                      UNUSED u8* extName, UNUSED int length, UNUSED s32* fileNo) {
    return PFS_ERR_NOPACK;
}
s32 osPfsDeleteFile(UNUSED OSPfs* pfs, UNUSED u16 company, UNUSED u32 game, UNUSED u8* gameName,
                    UNUSED u8* extName) {
    return PFS_ERR_NOPACK;
}
s32 osPfsReadWriteFile(UNUSED OSPfs* pfs, UNUSED s32 fileNo, UNUSED u8 flag, UNUSED int offset, UNUSED int nbytes,
                       UNUSED u8* data) {
    return PFS_ERR_NOPACK;
}
s32 osPfsFileState(UNUSED OSPfs* pfs, UNUSED s32 fileNo, UNUSED OSPfsState* state) {
    return PFS_ERR_NOPACK;
}
s32 osPfsFreeBlocks(UNUSED OSPfs* pfs, s32* bytes) {
    *bytes = 0;
    return PFS_ERR_NOPACK;
}
s32 osPfsNumFiles(UNUSED OSPfs* pfs, s32* max, s32* used) {
    *max = 0;
    *used = 0;
    return PFS_ERR_NOPACK;
}

/* ------------------------------------------------------------------------- */
/* Debug output                                                               */
/* ------------------------------------------------------------------------- */

void rmonPrintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void osSyncPrintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
