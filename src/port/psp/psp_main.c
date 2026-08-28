/**
 * PSP entry point and game loop host.
 *
 * On the N64 main_func() spins up idle/video/audio/game threads that talk
 * through message queues and RSP tasks.  Here the game loop runs on the main
 * thread: each iteration reads the pad, runs one game tick (which builds a
 * display list), executes that display list on the GE, mixes the audio for
 * the frame and waits for vsync.
 */
#include <ultra64.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <pspge.h>
#include <pspgu.h>
#include <stdio.h>
#include <string.h>

#include <ultra64.h>
#include <macros.h>
#include "port.h"
#include "gfx_pc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"

PSP_MODULE_INFO("MK64", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

extern struct GfxWindowManagerAPI gfx_psp;
extern void port_audio_out_init(void);
extern struct GfxRenderingAPI gfx_opengl_api; // gfx_scegu.c keeps the sm64-port name

/* Game-side entry points (main.c, TARGET_PSP variants). */
extern void port_game_init(void);
extern void port_game_loop_one_iteration(void);

static int sRunning = 1;

static int exit_callback(UNUSED int arg1, UNUSED int arg2, UNUSED void* common) {
    sRunning = 0;
    sceKernelExitGame();
    return 0;
}

static int callback_thread(UNUSED SceSize args, UNUSED void* argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, 0);
    }
}

void port_gfx_run(Gfx* dl) {
    gfx_run(dl);
}

void port_gfx_start_frame(void) {
    gfx_start_frame();
}

void port_gfx_end_frame(void) {
    gfx_end_frame();
}


void port_input_poll(void) {
}

/* Called by audio_psp.c's init; the mixer thread arrives with the audio work. */
void init_audiomanager(void) {
}

/* ------------------------------------------------------------------------- */
/* On-screen FPS counter: a 4x6 digit font drawn as 2D sprites at frame end.  */
/* ------------------------------------------------------------------------- */
static const u8 sDigitFont[11][6] = { // 4 wide x 6 tall, MSB = leftmost
    { 0x60, 0x90, 0x90, 0x90, 0x90, 0x60 }, // 0
    { 0x20, 0x60, 0x20, 0x20, 0x20, 0x70 }, // 1
    { 0x60, 0x90, 0x10, 0x20, 0x40, 0xF0 }, // 2
    { 0xE0, 0x10, 0x60, 0x10, 0x10, 0xE0 }, // 3
    { 0x90, 0x90, 0x90, 0xF0, 0x10, 0x10 }, // 4
    { 0xF0, 0x80, 0xE0, 0x10, 0x10, 0xE0 }, // 5
    { 0x60, 0x80, 0xE0, 0x90, 0x90, 0x60 }, // 6
    { 0xF0, 0x10, 0x20, 0x20, 0x40, 0x40 }, // 7
    { 0x60, 0x90, 0x60, 0x90, 0x90, 0x60 }, // 8
    { 0x60, 0x90, 0x90, 0x70, 0x10, 0x60 }, // 9
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x20 }, // .
};
static u16 sFontTex[64 * 8] __attribute__((aligned(16))); // 11 glyphs of 4x6 in a 64x8 5551 texture
static u32 sFontTexId;
static u32 sFpsShown; // fps * 10

static void overlay_build_font(void) {
    int g, y, x;
    for (g = 0; g < 11; g++) {
        for (y = 0; y < 6; y++) {
            for (x = 0; x < 4; x++) {
                sFontTex[y * 64 + g * 5 + x] = (sDigitFont[g][y] & (0x80 >> x)) ? 0xFFFF : 0x0000; // 5551: opaque white / transparent
            }
        }
    }
}

void port_gfx_overlay(void) {
    extern void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t len, size_t n);
    struct GfxRenderingAPI* r = &gfx_opengl_api;
    static u32 sLastT, sFrames;
    static int sInited;
    u32 now = sceKernelGetSystemTimeLow();
    char text[8];
    int i, x = 4;
    if (!sInited) {
        overlay_build_font();
        sInited = 1;
        sLastT = now;
    }
    sFrames++;
    if (now - sLastT >= 500000) { // update twice a second
        sFpsShown = (u32) ((u64) sFrames * 10000000 / (now - sLastT));
        sFrames = 0;
        sLastT = now;
    }
    snprintf(text, sizeof(text), "%u.%u", sFpsShown / 10, sFpsShown % 10);
    {
        struct ShaderProgram* prg = r->lookup_shader(0x05000045); // texture only, alpha test (texture edge)
        if (prg == NULL) {
            prg = r->create_and_load_new_shader(0x05000045);
        } else {
            r->load_shader(prg);
        }
    }
    r->set_depth_test(false);
    r->set_use_alpha(true);
    r->set_viewport(0, 0, 480, 272);
    r->set_scissor(0, 0, 480, 272);
    // The texture arena is recycled behind our back: upload the 1 KB font every frame.
    sFontTexId = r->new_texture();
    r->select_texture(0, sFontTexId);
    r->upload_texture((const u8*) sFontTex, 64, 8, 1 /* GU_PSM_5551 */);
    r->set_sampler_parameters(0, false, 2 /* G_TX_CLAMP */, 2);
    r->set_depth_mask(false);
    r->set_zmode_decal(false);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0x55, 0xff);
    sceGuEnable(GU_BLEND);
    for (i = 0; text[i] != 0; i++) {
        int g = text[i] == '.' ? 10 : text[i] - '0';
        struct { u16 u, v; u32 color; u16 x, y, z; } spr[2] = {
            { (u16) (g * 5), 0, 0xFF00FF00, (u16) x, 4, 0 },
            { (u16) (g * 5 + 4), 6, 0xFF00FF00, (u16) (x + 4), 10, 0 } // 1x: 4x6 px glyphs
        };
        gfx_scegu_draw_triangles_2d((float*) spr, 0, 1);
        x += 5;
    }
    // Force the interpreter to re-apply its own state next frame.
    {
        extern void gfx_overlay_state_dirty(void);
        gfx_overlay_state_dirty();
    }
}

u32 port_time_us(void) {
    return sceKernelGetSystemTimeLow();
}

/* Frame profile: accumulate per-slot microseconds, report every 300 frames. */
void port_profile_add(int slot, u32 us) {
    static u32 sum[8], frames;
    sum[slot] += us;
    if (slot == 3 && ++frames == 300) {
        extern u32 gfx_prof_cmds, gfx_prof_tris, gfx_prof_opcount[256];
        int k, best;
        extern u32 gfx_prof_rebuilds;
        extern u32 port_audio_out_underruns(void);
        PORT_LOG("audio underruns so far: %u\n", port_audio_out_underruns());
PORT_LOG("profile us/frame: logic %u  dl->ge %u (ge+vsync %u)  audio %u  total %u  (%u rebuilds)\n",
                 sum[0] / 300, sum[1] / 300, sum[2] / 300, sum[3] / 300,
                 (sum[0] + sum[1] + sum[2] + sum[3]) / 300, gfx_prof_rebuilds / 300);
#ifdef PORT_PROFILE_DL
        {
            extern u32 gfx_prof_vtx, gfx_prof_cullclip, gfx_prof_state, gfx_prof_emit, gfx_prof_flush;
            PORT_LOG("  dl breakdown us/frame: vtxproc %u  cull+clip %u  staterebuild %u  emit %u  flush %u\n",
                     gfx_prof_vtx / 300, gfx_prof_cullclip / 300, gfx_prof_state / 300, gfx_prof_emit / 300, gfx_prof_flush / 300);
            {
                extern u32 gfx_prof_tri1calls, gfx_prof_clipcalls, gfx_prof_vtxcount;
                PORT_LOG("  counts/frame: tri1 %u  clipped %u  verts %u\n", gfx_prof_tri1calls / 300, gfx_prof_clipcalls / 300, gfx_prof_vtxcount / 300);
                gfx_prof_tri1calls = gfx_prof_clipcalls = gfx_prof_vtxcount = 0;
            }
            gfx_prof_vtx = gfx_prof_cullclip = gfx_prof_state = gfx_prof_emit = gfx_prof_flush = 0;
        }
#endif
        gfx_prof_rebuilds = 0;
        for (k = 0; k < 10; k++) {
            int i;
            best = 0;
            for (i = 1; i < 256; i++) if (gfx_prof_opcount[i] > gfx_prof_opcount[best]) best = i;
            if (gfx_prof_opcount[best] == 0) break;
            PORT_LOG("  op %02X: %u/frame\n", best, gfx_prof_opcount[best] / 300);
            gfx_prof_opcount[best] = 0;
        }
        memset(gfx_prof_opcount, 0, sizeof(gfx_prof_opcount));
        gfx_prof_cmds = gfx_prof_tris = 0;
        memset(sum, 0, sizeof(sum));
        frames = 0;
    }
}

static char sEbootDir[192] = "ms0:/PSP/GAME/MK64Portable/";
static char sSaveDir[200] = "ms0:/PSP/GAME/MK64Portable/data/";
const char* port_eboot_dir(void) { return sEbootDir; }
const char* port_save_dir(void) { return sSaveDir; }
const char* port_save_path(const char* name) {
    static char buf[4][256];
    static int i;
    char* b = buf[i++ & 3];
    snprintf(b, 256, "%s%s", sSaveDir, name);
    return b;
}
void port_set_save_dir(const char* argv0) {
    const char* slash = argv0 ? strrchr(argv0, '/') : NULL;
    if (slash != NULL && (size_t) (slash + 1 - argv0) < sizeof(sEbootDir)) {
        memcpy(sEbootDir, argv0, slash + 1 - argv0);
        sEbootDir[slash + 1 - argv0] = 0;
    }
    {
        char dir[200];
        SceUID d;
        snprintf(dir, sizeof(dir), "%sdata", sEbootDir); /* no trailing slash: the PSP's FAT driver rejects it */
        sceIoMkdir(dir, 0777);                            /* harmless if it exists */
        d = sceIoDopen(dir);
        if (d >= 0) {
            sceIoDclose(d);
            snprintf(sSaveDir, sizeof(sSaveDir), "%s/", dir);
        } else {
            snprintf(sSaveDir, sizeof(sSaveDir), "%s", sEbootDir); /* could not create data/: write beside the EBOOT */
        }
    }
}
void port_fs_init(void) {
    /* data/ is created by port_set_save_dir(); nothing else to prepare. */
}

void port_fs_mkdir(const char* path) {
    sceIoMkdir(path, 0777);
}

/* Read the frame currently on screen back out of VRAM and write a binary PPM. */
static void port_dump_buffer(int index, const void* topaddr, int bufferwidth, int pixelformat);

/* Copy the displayed frame out of VRAM with the GE (sceGuCopyImage): reading
 * VRAM from the CPU is unreliable in emulators. */
void port_screenshot(int index) {
    void* topaddr = NULL;
    int bufferwidth = 0;
    int pixelformat = 0;
    static u16 sCopy[512 * 272] __attribute__((aligned(64)));
    extern unsigned int list[];

    sceDisplayGetFrameBuf(&topaddr, &bufferwidth, &pixelformat, PSP_DISPLAY_SETBUF_IMMEDIATE);
    if (topaddr == NULL) {
        return;
    }
    sceGuStart(GU_DIRECT, list);
    sceGuCopyImage(GU_PSM_5650, 0, 0, 480, 272, bufferwidth, (void*) ((uintptr_t) topaddr & 0x1FFFFFFF | 0x04000000), 0, 0, 512, sCopy);
    sceGuFinish();
    sceGuSync(0, 0);
    sceKernelDcacheInvalidateRange(sCopy, sizeof(sCopy));
    port_dump_buffer(index, sCopy, 512, PSP_DISPLAY_PIXEL_FORMAT_565);
}

/* Debug: dump the 16-bit z-buffer as a grey PPM (near = bright). */
void port_depthshot(int index) {
    extern void* gu_zbp;
    extern unsigned int list[];
    static u16 sCopy[512 * 272] __attribute__((aligned(64)));
    char name[64];
    FILE* fp;
    int x, y;
    static u8 row[480 * 3];
    void* zaddr = (void*) (((uintptr_t) gu_zbp & 0x1FFFFF) | 0x04000000);
    sceGuStart(GU_DIRECT, list);
    sceGuCopyImage(GU_PSM_4444, 0, 0, 480, 272, 512, zaddr, 0, 0, 512, sCopy);
    sceGuFinish();
    sceGuSync(0, 0);
    sceKernelDcacheInvalidateRange(sCopy, sizeof(sCopy));
    snprintf(name, sizeof(name), "%s" "depth%03d.ppm", port_save_dir(), index);
    fp = fopen(name, "wb");
    if (fp == NULL) {
        return;
    }
    fprintf(fp, "P6\n480 272\n255\n");
    for (y = 0; y < 272; y++) {
        for (x = 0; x < 480; x++) {
            u8 v = sCopy[y * 512 + x] >> 8;
            row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = v;
        }
        fwrite(row, 1, sizeof(row), fp);
    }
    fclose(fp);
    PORT_LOG("wrote %s\n", name);
}

static void port_dump_buffer(int index, const void* topaddr, int bufferwidth, int pixelformat) {
    char name[64];
    FILE* fp;
    int x, y;
    static u8 row[480 * 3];

    snprintf(name, sizeof(name), "%s" "shot%03d.ppm", port_save_dir(), index);
    fp = fopen(name, "wb");
    if (fp == NULL) {
        return;
    }
    fprintf(fp, "P6\n480 272\n255\n");
    for (y = 0; y < 272; y++) {
        const u16* src = (const u16*) ((const u8*) topaddr + y * bufferwidth * 2);
        for (x = 0; x < 480; x++) {
            u16 p = src[x];
            if (pixelformat == PSP_DISPLAY_PIXEL_FORMAT_565) {
                row[x * 3 + 0] = (p & 0x1F) << 3;
                row[x * 3 + 1] = ((p >> 5) & 0x3F) << 2;
                row[x * 3 + 2] = ((p >> 11) & 0x1F) << 3;
            } else { // 5551
                row[x * 3 + 0] = (p & 0x1F) << 3;
                row[x * 3 + 1] = ((p >> 5) & 0x1F) << 3;
                row[x * 3 + 2] = ((p >> 10) & 0x1F) << 3;
            }
        }
        fwrite(row, 1, sizeof(row), fp);
    }
    fclose(fp);
    PORT_LOG("wrote %s\n", name);
}


static u32 sFrame;
extern int gfx_debug_frame;
static void run_one_iteration(void) {
#ifdef PORT_GFX_DEBUG
#ifdef PORT_INPUT_SCRIPT
    gfx_debug_frame = 0;
#else
    gfx_debug_frame = 0;
#endif
#endif
#ifdef PORT_INPUT_SCRIPT
    {
        extern int gfx_dump_textures;
        extern void gfx_debug_flush_texture_cache(void);
        gfx_dump_textures = (sFrame >= 955 && sFrame <= 975) || (sFrame >= 1300 && sFrame <= 1320);
        if (sFrame == 2039) {
            gfx_debug_flush_texture_cache();
        }
    }
#endif
    port_game_loop_one_iteration();
#ifdef PORT_INPUT_SCRIPT
    if (sFrame == 1919) {
        extern void texman_debug_readback(unsigned int num, void* dst, unsigned int bytes);
        static u16 buf[512 * 2] __attribute__((aligned(64)));
        FILE* fp;
        texman_debug_readback(5, buf, 2048);
        fp = fopen(port_save_path("tex5_vram.bin"), "wb");
        if (fp) { fwrite(buf, 1, 2048, fp); fclose(fp); }
    }
#endif
#ifdef PORT_INPUT_SCRIPT
    if (sFrame == 1321) {
        // Debug: draw a captured 32x32 ground texture as a 2D sprite and as a
        // tiled 3D quad on top of the game frame.
        extern uint16_t gDebugTex32[32 * 32];
        extern int gDebugTex32Valid;
        extern void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t len, size_t n);
        extern float identity_matrix[4][4];
        struct GfxRenderingAPI* r = &gfx_opengl_api;
        if (gDebugTex32Valid) {
            struct { u16 u, v; u32 color; u16 x, y, z; } spr[2] = { { 0, 0, 0xFFFFFFFF, 300, 10, 0 }, { 32, 32, 0xFFFFFFFF, 364, 74, 0 } };
            struct { float u, v; u32 color; float x, y, z; } tri[6] = {
                { 0, 0, 0xFFFFFFFF, 0.2f, -0.9f, 0.5f }, { 4, 0, 0xFFFFFFFF, 0.9f, -0.9f, 0.5f }, { 4, 4, 0xFFFFFFFF, 0.9f, -0.2f, 0.5f },
                { 0, 0, 0xFFFFFFFF, 0.2f, -0.9f, 0.5f }, { 4, 4, 0xFFFFFFFF, 0.9f, -0.2f, 0.5f }, { 0, 4, 0xFFFFFFFF, 0.2f, -0.2f, 0.5f } };
            u32 id;
            r->start_frame();
            r->load_shader(r->create_and_load_new_shader(0x45));
            r->set_depth_test(false);
            r->set_use_alpha(false);
            r->set_viewport(0, 0, 480, 272);
            r->set_scissor(0, 0, 480, 272);
            id = r->new_texture();
            r->select_texture(0, id);
            r->upload_texture((const u8*) gDebugTex32, 32, 32, 1);
            r->set_sampler_parameters(0, false, 0, 0);
            gfx_scegu_draw_triangles_2d((float*) spr, 0, 1);
            sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4*) identity_matrix);
            sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4*) identity_matrix);
            sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4*) identity_matrix);
            r->draw_triangles((float*) tri, 0, 2);
            r->end_frame();
            r->finish_render();
            port_screenshot(1321);
            PORT_LOG("debug 32x32 blit done\n");
        }
    }
    if (sFrame == 2041) {
        // Debug: blit the kart textures imported last frame as plain 2D sprites.
        extern uint16_t gDebugKartTex[2][64 * 32];
        extern int gDebugKartTexCount;
        extern void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t len, size_t n);
        struct GfxRenderingAPI* r = &gfx_opengl_api;
        int i;
        r->start_frame();
        r->load_shader(r->create_and_load_new_shader(0x45));
        r->set_depth_test(false);
        r->set_use_alpha(false);
        r->set_viewport(0, 0, 480, 272);
        r->set_scissor(0, 0, 480, 272);
        for (i = 0; i < gDebugKartTexCount; i++) {
            struct { u16 u, v; u32 color; u16 x, y, z; } spr[2] = { { 0, 0, 0xFFFFFFFF, 300, 10 + i * 70, 0 }, { 64, 32, 0xFFFFFFFF, 428, 74 + i * 70, 0 } };
            u32 id = r->new_texture();
            r->select_texture(0, id);
            r->upload_texture((const u8*) gDebugKartTex[i], 64, 32, 1 /* GU_PSM_5551 */);
            r->set_sampler_parameters(0, false, 0, 0);
            gfx_scegu_draw_triangles_2d((float*) spr, 0, 1);
        }
        r->end_frame();
        r->finish_render();
        port_screenshot(2041);
        PORT_LOG("kart blit done (%d textures)\n", gDebugKartTexCount);
    }
#endif
    sFrame++;
#ifdef PORT_GFX_DEBUG
    if (sFrame == 100 || (sFrame % 300) == 0) {
        port_screenshot((int) sFrame);
    }
#endif
}


int main(UNUSED int argc, char** argv) {
    extern void port_assets_load(const char* argv0);
    setup_callbacks();
    scePowerSetClockFrequency(333, 333, 166);
    pspDebugScreenInit();
    port_set_save_dir(argc > 0 ? argv[0] : NULL); // everything lives next to the EBOOT
    port_assets_load(argc > 0 ? argv[0] : NULL); // ROM-derived data lives outside the EBOOT
    // (no boot banner: the debug console is only initialised so the screen is black until the GE draws)
    port_fs_init();
    port_audio_out_init();
    PORT_LOG("boot\n");

    gfx_init(&gfx_psp, &gfx_opengl_api, "MK64 Portable", false);
#ifdef PORT_GFX_SELFTEST
    {
        // Draw a red textured sprite and a green triangle straight through the
        // backend and dump the frame: separates texture/data problems from GU state.
        static u16 tex[16 * 16];
        struct { u16 u, v; u32 color; u16 x, y, z; } spr[2] = { { 0, 0, 0xFFFFFFFF, 100, 100, 0 }, { 16, 16, 0xFFFFFFFF, 200, 200, 0 } };
        struct { float u, v; u32 color; float x, y, z; } tri[3] = { { 0, 0, 0xFF00FF00, -0.5f, -0.5f, 0.5f }, { 0, 0, 0xFF00FF00, 0.5f, -0.5f, 0.5f }, { 0, 0, 0xFF00FF00, 0.0f, 0.5f, 0.5f } };
        extern void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t len, size_t n);
        extern float identity_matrix[4][4];
        struct GfxRenderingAPI* r = &gfx_opengl_api;
        int i;
        u32 id;
        for (i = 0; i < 256; i++) tex[i] = 0x801F; // 5551: A=1, R=31
        r->start_frame();
        id = r->new_texture();
        r->select_texture(0, id);
        r->upload_texture((const u8*) tex, 16, 16, 1 /* GU_PSM_5551 */);
        r->set_sampler_parameters(0, false, 0, 0);
        r->load_shader(r->create_and_load_new_shader(0x45)); // texture * shade
        r->set_depth_test(false);
        r->set_use_alpha(false);
        r->set_viewport(0, 0, 480, 272);
        r->set_scissor(0, 0, 480, 272);
        gfx_scegu_draw_triangles_2d((float*) spr, 0, 1);
        {
            // non-aligned 20x3 blue texture through the pitched path
            static u16 tex2[20 * 3];
            struct { u16 u, v; u32 color; u16 x, y, z; } spr2[2] = { { 0, 0, 0xFFFFFFFF, 300, 100, 0 }, { 20, 3, 0xFFFFFFFF, 400, 130, 0 } };
            for (i = 0; i < 60; i++) tex2[i] = 0xFC00; // A=1, B=31
            id = r->new_texture();
            r->select_texture(0, id);
            r->upload_texture((const u8*) tex2, 20, 3, 1);
            r->set_sampler_parameters(0, false, 0, 0);
            gfx_scegu_draw_triangles_2d((float*) spr2, 0, 1);
        }
        r->load_shader(r->create_and_load_new_shader(0x200)); // shade only
        sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4*) identity_matrix);
        sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4*) identity_matrix);
        sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4*) identity_matrix);
        r->draw_triangles((float*) tri, 0, 1);
        r->end_frame();
        r->finish_render();
        port_screenshot(999);
        PORT_LOG("selftest done\n");
    }
#endif

    port_game_init();
    PORT_LOG("game init done\n");

    while (sRunning) {
        run_one_iteration();
    }

    sceKernelExitGame();
    return 0;
}
