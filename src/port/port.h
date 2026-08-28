/**
 * Mario Kart 64 -> PSP port: shared declarations for the port layer.
 *
 * The N64 game addresses much of its data through RSP "segments"
 * (0xSSOOOOOO = segment SS, offset OOOOOO).  On the PSP the same data is
 * linked straight into the executable, so a segmented address is resolved
 * either linearly (segment base + offset, for buffers the game fills at run
 * time: vertices, decompressed textures, the gfx pool) or through a table of
 * {offset, symbol} pairs generated from the decomp's own naming (course
 * display lists, torch assets), see tools/psp/gen_seg_tables.py.
 */
#ifndef PORT_H
#define PORT_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>

typedef struct {
    u32 offset;      // offset inside the segment
    u32 size;        // bytes covered from `offset` (up to the next entry)
    const void* ptr; // where that data lives on the PSP
} PortSegEntry;

typedef struct {
    const PortSegEntry* entries; // sorted by offset
    s32 count;
} PortSegTable;

typedef struct {
    PortSegTable seg6; // course_data.c      (0x06xxxxxx)
    PortSegTable seg7; // course_displaylists (0x07xxxxxx)
} PortCourseSegTables;

extern const PortCourseSegTables gPortCourseSegTables[]; // indexed by course id
extern const PortSegTable gPortCommonDataSegTable;       // segment 0x0D
extern const PortSegTable gPortCeremonyDataSegTable;     // segment 0x0B (ending)
extern const PortSegTable gPortStartupLogoSegTable;      // segment 0x06 (boot logo)

extern uintptr_t gSegmentTable[16];

void port_set_segment_table(s32 segment, const PortSegTable* table);
void port_clear_segment_table(s32 segment);
/** Segmented address (or real pointer) -> real pointer.  NULL if unmapped. */
void* port_seg_to_ptr(uintptr_t addr);
/** Real pointers pass straight through, everything else is treated as segmented. */
static inline int port_is_real_ptr(uintptr_t addr) {
    // PSP user memory starts at 0x08800000; MK64 only forms segments 0..0xF with
    // small offsets, so anything above the segment window is a real pointer.
    return addr >= 0x08800000u || addr < 0x00010000u ? (addr >= 0x08800000u) : 0;
}

/* Memory pool the game allocates course/texture data from (replaces the N64's
 * fixed RAM regions between the code segments). */
#define PORT_MEMORY_POOL_SIZE 0x300000
extern u8 gPortMemoryPool[PORT_MEMORY_POOL_SIZE];

/* Where the port keeps its files (save, log, screenshots). */
#define PORT_SAVE_DIR "ms0:/MK64/"
void port_fs_init(void);

/* Dump the displayed frame to PORT_SAVE_DIR/shotNNN.ppm (debugging). */
void port_screenshot(int index);

/* Logging (goes to stdout / psplink / a file depending on the backend). */
void port_log(const char* fmt, ...);
#define PORT_LOG(...) port_log(__VA_ARGS__)

/* Frame hooks implemented by the platform backend. */
void port_gfx_run(Gfx* dl);        // execute a display list (F3DEX -> sceGu)
void port_gfx_start_frame(void);
void port_gfx_end_frame(void);     // present + vsync
void port_audio_frame(void);       // mix one game frame worth of audio
void port_input_poll(void);        // fill controller state

#endif /* PORT_H */
