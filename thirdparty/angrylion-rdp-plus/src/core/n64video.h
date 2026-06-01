#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#define RDRAM_MAX_SIZE 0x800000

// register enums
enum dp_register {
    DP_START,
    DP_END,
    DP_CURRENT,
    DP_STATUS,
    DP_CLOCK,
    DP_BUFBUSY,
    DP_PIPEBUSY,
    DP_TMEM,
    DP_NUM_REG
};

enum vi_register {
    VI_STATUS, // aka VI_CONTROL
    VI_ORIGIN, // aka VI_DRAM_ADDR
    VI_WIDTH,
    VI_INTR,
    VI_V_CURRENT_LINE,
    VI_TIMING,
    VI_V_SYNC,
    VI_H_SYNC,
    VI_LEAP,    // aka VI_H_SYNC_LEAP
    VI_H_START, // aka VI_H_VIDEO
    VI_V_START, // aka VI_V_VIDEO
    VI_V_BURST,
    VI_X_SCALE,
    VI_Y_SCALE,
    VI_NUM_REG
};

// config enums
enum vi_mode {
    VI_MODE_NORMAL,   // color buffer with VI filter
    VI_MODE_COLOR,    // direct color buffer, unfiltered
    VI_MODE_DEPTH,    // depth buffer as grayscale
    VI_MODE_COVERAGE, // coverage as grayscale
    VI_MODE_NUM
};

enum vi_interp {
    VI_INTERP_NEAREST,
    VI_INTERP_LINEAR,
    VI_INTERP_HYBRID,
    VI_INTERP_NUM
};

enum dp_compat_profile {
    DP_COMPAT_LOW,
    DP_COMPAT_MEDIUM,
    DP_COMPAT_HIGH,
    DP_COMPAT_NUM
};

struct n64video_pixel {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

struct n64video_frame_buffer {
    struct n64video_pixel *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t height_out;
    uint32_t pitch;
    bool valid;
};

struct n64video_config {
    struct {
        uint8_t *rdram;           // RDRAM pointer
        uint32_t rdram_size;      // size of RDRAM, typically 4 or 8 MiB
        uint8_t *dmem;            // RSP data memory pointer
        uint32_t **vi_reg;        // video interface registers
        uint32_t **dp_reg;        // display processor registers
        uint32_t *mi_intr_reg;    // MIPS interface interrupt register
        void (*mi_intr_cb)(void); // interrupt callback function
    } gfx;
    struct {
        enum vi_mode mode;     // output mode
        enum vi_interp interp; // output interpolation method
        bool widescreen;       // force 16:9 aspect ratio if true
        bool hide_overscan;    // crop to visible area if true
        bool vsync;            // enable vsync if true
        bool exclusive;        // run in exclusive mode when in fullscreen if true
        bool integer_scaling;  // one native pixel is displayed as a multiple of a screen pixel if true
    } vi;
    struct {
        enum dp_compat_profile compat; // multithreading compatibility mode
    } dp;
    bool parallel;        // use multithreaded renderer if true
    bool busyloop;        // use a busyloop while waiting for work
    uint32_t num_workers; // number of rendering workers
};

// Rendering metrics (overdraw etc.). Counters accumulate until reset; the embedder
// typically snapshots + resets once per displayed frame. The per-pixel/per-command
// increments are only compiled when N64VIDEO_METRICS is defined (zero cost otherwise),
// but this API is always available.
struct n64video_metrics {
    uint64_t pixels_drawn;   // framebuffer pixels written by primitives (blender path)
    uint64_t pixels_filled;  // framebuffer pixels written by fill-mode rectangles
    uint64_t cmd_count[64];  // number of executed RDP commands, indexed by opcode
};
void
n64video_metrics_get(struct n64video_metrics *out);
void
n64video_metrics_reset(void);

// Framebuffer access heatmap: two saturating uint8 counters per 16-bit RDRAM word,
// indexed by (framebuffer byte address >> 1). 'writes' counts framebuffer pixel writes
// (overdraw); 'reads' counts framebuffer reads (the read-modify-write blend path, i.e.
// where blending costs performance). The embedder clears these once per frame and renders
// them as a heatmap. Only populated when N64VIDEO_METRICS is defined; otherwise the
// pointers are NULL.
struct n64video_fb_heatmap {
    const uint8_t *writes;
    const uint8_t *reads;
    uint32_t entries;  // counter count in each buffer (== RDRAM size in 16-bit words)
};
void
n64video_fb_heatmap_get(struct n64video_fb_heatmap *out);
void
n64video_fb_heatmap_clear(void);

// Hidden RDRAM: the 9th-bit / dz coverage bits angrylion maintains internally, one byte per
// 16-bit RDRAM word, indexed by (byte address >> 1). Lets the embedder render depth/coverage
// views (the low 2 bits hold the depth dz). Always available.
void
n64video_hidden_rdram_get(const uint8_t **buf, uint32_t *entries);

// Per-pixel overdraw buffer: one saturating uint8 counter per 16-bit RDRAM word,
// indexed by (framebuffer byte address >> 1). Incremented on every framebuffer pixel
// write; the embedder clears it once per frame and renders it as a heatmap. Only
// populated when N64VIDEO_METRICS is defined; otherwise *buf is NULL.
void
n64video_overdraw_get(const uint8_t **buf, uint32_t *entries);
void
n64video_overdraw_clear(void);

// TMEM viewer: snapshot of the 4096-byte texture memory and the 8 tile configurations.
// tmem is a live pointer into the renderer's internal state (WORD_ADDR_XOR byte order);
// read it between frames when the RDP thread is idle. tiles[] mirrors the RDP's SetTile
// state — format, size, stride, palette, coordinate bounds, and clamp/mirror/mask/shift.
#define N64VIDEO_NUM_TILES 8

struct n64video_tile_info {
    uint8_t  format;  // FORMAT_RGBA=0, YUV=1, CI=2, IA=3, I=4
    uint8_t  size;    // PIXEL_SIZE_4BIT=0, 8BIT=1, 16BIT=2, 32BIT=3
    uint16_t tmem;    // start address in TMEM (64-bit word index, 0..511)
    uint16_t line;    // stride in 64-bit words
    uint16_t palette; // palette selector (4 bits)
    uint16_t sl, tl;  // texture coordinate bounds (S low, T low)
    uint16_t sh, th;  // texture coordinate bounds (S high, T high)
    uint8_t  clamp_s, clamp_t;
    uint8_t  mirror_s, mirror_t;
    uint8_t  mask_s, mask_t;
    uint8_t  shift_s, shift_t;
};

struct n64video_tmem_snapshot {
    const uint8_t *tmem;                                    // 4096 bytes
    struct n64video_tile_info tiles[N64VIDEO_NUM_TILES];
};

void
n64video_tmem_snapshot(struct n64video_tmem_snapshot *out);

// Decode a rectangular region of a tile into host RGBA8888 pixels.
// dst must hold w*h u32s. Coordinates are texel indices (0-based, matching
// fetch_texel's s/t parameters). Uses state[0]'s tile config + TLUT.
void
n64video_decode_tile_region(uint32_t tilenum, uint32_t *dst,
                            uint32_t x0, uint32_t y0, uint32_t w, uint32_t h);

void
n64video_config_init(struct n64video_config *config);
void
n64video_init(struct n64video_config *config);
void
n64video_update_screen(struct n64video_frame_buffer *fb);
void
n64video_process_list(void);
// Force any buffered commands to render now (used for command-by-command stepping).
// A no-op in single-threaded mode, where commands run immediately.
void
n64video_flush(void);
void
n64video_close(void);

#ifdef __cplusplus
}
#endif
