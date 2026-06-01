#ifdef N64VIDEO_C

// anamorphic NTSC resolution
#define H_RES_NTSC 640
#define V_RES_NTSC 480

// anamorphic PAL resolution
#define H_RES_PAL 768
#define V_RES_PAL 576

// typical VI_V_SYNC values for NTSC and PAL
#define V_SYNC_NTSC 525
#define V_SYNC_PAL  625

// maximum possible size of the prescale area
#define PRESCALE_WIDTH  H_RES_NTSC
#define PRESCALE_HEIGHT V_SYNC_PAL

enum vi_type {
    VI_TYPE_BLANK,    // no data, no sync
    VI_TYPE_RESERVED, // unused, should never be set
    VI_TYPE_RGBA5551, // 16 bit color (internally 18 bit RGBA5553)
    VI_TYPE_RGBA8888  // 32 bit color
};

enum vi_aa {
    VI_AA_RESAMP_EXTRA_ALWAYS, // resample and AA (always fetch extra lines)
    VI_AA_RESAMP_EXTRA,        // resample and AA (fetch extra lines if needed)
    VI_AA_RESAMP_ONLY,         // only resample (treat as all fully covered)
    VI_AA_REPLICATE            // replicate pixels, no interpolation
};

struct vi_reg_ctrl {
    uint8_t type;
    bool gamma_dither_enable;
    bool gamma_enable;
    bool divot_enable;
    bool vbus_clock_enable;
    bool serrate;
    bool test_mode;
    uint8_t aa_mode;
    bool kill_we;
    uint8_t pixel_advance;
    bool dither_filter_enable;
};

typedef void (*vi_fetch_filter_func)(struct n64video_pixel *, uint32_t, uint32_t, struct vi_reg_ctrl, uint32_t,
                                     uint32_t);

#include "vi/gamma.c"
#include "vi/lerp.c"
#include "vi/divot.c"
#include "vi/video.c"
#include "vi/restore.c"
#include "vi/fetch.c"

// states
static int32_t prevvicurrent;
static int32_t emucontrolsvicurrent;
static bool prevserrate;
static bool lowerfield;
static int32_t oldvstart;
static bool prevwasblank;
static int32_t vactivelines;
static bool ispal;
static int32_t minhpass;
static int32_t maxhpass;
static uint32_t x_add;
static uint32_t x_start;
static uint32_t y_add;
static uint32_t y_start;
static int32_t v_sync;
static int32_t vi_width_low;
static uint32_t frame_buffer;
static uint32_t tvfadeoutstate[PRESCALE_HEIGHT];
static uint32_t zb_address;
static int32_t vinnglitch;

// prescale buffer
static struct n64video_pixel prescale[PRESCALE_WIDTH * PRESCALE_HEIGHT];
static uint32_t prescale_ptr;
static int32_t linecount;

// parsed VI registers
static uint32_t **vi_reg_ptr;
static struct vi_reg_ctrl ctrl;
static int32_t hres, vres;
static int32_t hres_raw, vres_raw;
static int32_t v_start;
static int32_t h_start;
static int32_t v_current_line;

static void
vi_init(void)
{
    vi_gamma_init();
    vi_restore_init();

    memset(prescale, 0, sizeof(prescale));

    prevvicurrent = 0;
    emucontrolsvicurrent = -1;
    prevserrate = false;
    oldvstart = 1337;
    prevwasblank = false;
    zb_address = 0;
}

static void
vi_process_full_parallel(uint32_t worker_id)
{
    int32_t y;
    struct n64video_pixel *viaa_array = state[worker_id].viaa_array;
    struct n64video_pixel *divot_array = state[worker_id].divot_array;

    int32_t cache_marker = 0, cache_next_marker = 0, divot_cache_marker = 0, divot_cache_next_marker = 0;

    // integer part of x offset from X_SCALE ?
    int32_t cache_marker_init = (x_start >> 10) - 1;

    struct n64video_pixel *viaa_cache = &viaa_array[0];
    struct n64video_pixel *viaa_cache_next = &viaa_array[0xa10];
    struct n64video_pixel *divot_cache = &divot_array[0];
    struct n64video_pixel *divot_cache_next = &divot_array[0xa10];

    struct n64video_pixel color, nextcolor, scancolor, scannextcolor;

    vi_fetch_filter_func vi_fetch_filter_ptr = ctrl.type & 1 ? vi_fetch_filter32 : vi_fetch_filter16;

    uint32_t pixels = 0, nextpixels = 0, fetchbugstate = 0;

    int32_t xfrac = 0, yfrac = 0;
    int32_t line_x = 0, next_line_x = 0, prev_line_x = 0, far_line_x = 0;
    int32_t prev_scan_x = 0, scan_x = 0, next_scan_x = 0, far_scan_x = 0;
    int32_t prev_x = 0, cur_x = 0, next_x = 0, far_x = 0;

    bool cache_init = false;

    pixels = 0;

    int32_t y_begin = 0;
    int32_t y_end = vres;
    int32_t y_inc = 1;

    if (config.parallel) {
        y_begin = worker_id;
        y_inc = parallel_num_workers();
    }

    // For each line ((V_END - V_START) >> 1) (full lines rather than half-lines)
    for (y = y_begin; y < y_end; y += y_inc) {
        int32_t x;

        // y_start is y offset frm Y_SCALE register, plus the current line (y_add is Y_SCALE)
        // current line
        uint32_t curry = y_start + y * y_add; // 2.10 + 2.10 * (dimensionless) = 2.10
        // next line
        uint32_t nexty = y_start + (y + 1) * y_add;
        // integer part of current line?
        uint32_t prevy = curry >> 10;

        cache_marker = cache_next_marker = cache_marker_init;
        if (ctrl.divot_enable) {
            divot_cache_marker = divot_cache_next_marker = cache_marker_init;
        }

        struct n64video_pixel *pixel_row = &prescale[prescale_ptr + linecount * y];

        // yfrac is top 5 fraction bits
        yfrac = (curry >> 5) & 0x1f;

        // vi_width_low is fb width (VI_WIDTH reg)
        // "pixels" is the start of the current line as an index
        pixels = vi_width_low * prevy;
        // "nextpixels" is the start of the next line as an index
        nextpixels = vi_width_low + pixels;

        // check if prevy and nexty are equal (same integer coordinate)
        if (prevy == (nexty >> 10)) {
            // fetch bug will affect the next output line
            fetchbugstate = 2;
        } else {
            // update fetch bug state, 2 -> 1 (1 is active), then 1 -> 0 (back to inactive)
            fetchbugstate >>= 1;
        }

        // x_start is x offset from X_SCALE register
        uint32_t x_offs = x_start;

        // for x in H_END - H_START
        for (x = 0; x < hres; x++, x_offs += x_add) {
            // line_x is the integer part of the 2.10 fixed point x position
            line_x = x_offs >> 10;
            // previous x
            prev_line_x = line_x - 1;
            // next x
            next_line_x = line_x + 1;
            // far x
            far_line_x = line_x + 2;

            // (prev, cur, next, far) for current line
            prev_x = pixels + prev_line_x;
            cur_x = pixels + line_x;
            next_x = pixels + next_line_x;
            far_x = pixels + far_line_x;

            // (prev, cur, next, far) for next line
            prev_scan_x = nextpixels + prev_line_x;
            scan_x = nextpixels + line_x;
            next_scan_x = nextpixels + next_line_x;
            far_scan_x = nextpixels + far_line_x;

            prev_line_x++;
            line_x++;
            next_line_x++;
            far_line_x++;

            // 5 most significant bits of fraction
            xfrac = (x_offs >> 5) & 0x1f;

            if (prev_line_x > cache_marker) {
                // (Current line) Previous pos not in cache, need to fetch 3 pixels
                vi_fetch_filter_ptr(&viaa_cache[prev_line_x], frame_buffer, prev_x, ctrl, vi_width_low, 0);
                vi_fetch_filter_ptr(&viaa_cache[line_x], frame_buffer, cur_x, ctrl, vi_width_low, 0);
                vi_fetch_filter_ptr(&viaa_cache[next_line_x], frame_buffer, next_x, ctrl, vi_width_low, 0);
                cache_marker = next_line_x;
            } else if (line_x > cache_marker) {
                // (Current line) Current pos not in cache, need to fetch 2 pixels
                vi_fetch_filter_ptr(&viaa_cache[line_x], frame_buffer, cur_x, ctrl, vi_width_low, 0);
                vi_fetch_filter_ptr(&viaa_cache[next_line_x], frame_buffer, next_x, ctrl, vi_width_low, 0);
                cache_marker = next_line_x;
            } else if (next_line_x > cache_marker) {
                // (Current line) Next pos not in cache, need to fetch 1 pixel
                vi_fetch_filter_ptr(&viaa_cache[next_line_x], frame_buffer, next_x, ctrl, vi_width_low, 0);
                cache_marker = next_line_x;
            }

            if (prev_line_x > cache_next_marker) {
                // (Next line) Previous pos not in cache, need to fetch 3 pixels
                vi_fetch_filter_ptr(&viaa_cache_next[prev_line_x], frame_buffer, prev_scan_x, ctrl, vi_width_low,
                                    fetchbugstate);
                vi_fetch_filter_ptr(&viaa_cache_next[line_x], frame_buffer, scan_x, ctrl, vi_width_low, fetchbugstate);
                vi_fetch_filter_ptr(&viaa_cache_next[next_line_x], frame_buffer, next_scan_x, ctrl, vi_width_low,
                                    fetchbugstate);
                cache_next_marker = next_line_x;
            } else if (line_x > cache_next_marker) {
                // (Next line) Current pos not in cache, need to fetch 2 pixels
                vi_fetch_filter_ptr(&viaa_cache_next[line_x], frame_buffer, scan_x, ctrl, vi_width_low, fetchbugstate);
                vi_fetch_filter_ptr(&viaa_cache_next[next_line_x], frame_buffer, next_scan_x, ctrl, vi_width_low,
                                    fetchbugstate);
                cache_next_marker = next_line_x;
            } else if (next_line_x > cache_next_marker) {
                // (Next line) Next pos not in cache, need to fetch 1 pixel
                vi_fetch_filter_ptr(&viaa_cache_next[next_line_x], frame_buffer, next_scan_x, ctrl, vi_width_low,
                                    fetchbugstate);
                cache_next_marker = next_line_x;
            }

            // If divot is enabled, run it now that we have pixels in the cache

            if (ctrl.divot_enable) {
                // Need the far pixels for current and next lines, make sure we have them
                if (far_line_x > cache_marker) {
                    vi_fetch_filter_ptr(&viaa_cache[far_line_x], frame_buffer, far_x, ctrl, vi_width_low, 0);
                    cache_marker = far_line_x;
                }

                if (far_line_x > cache_next_marker) {
                    vi_fetch_filter_ptr(&viaa_cache_next[far_line_x], frame_buffer, far_scan_x, ctrl, vi_width_low,
                                        fetchbugstate);
                    cache_next_marker = far_line_x;
                }

                if (line_x > divot_cache_marker) {
                    // (Current line) Divot result not cached, run for current pixel and next pixel
                    divot_filter(&divot_cache[line_x], viaa_cache[line_x], viaa_cache[prev_line_x],
                                 viaa_cache[next_line_x]);
                    divot_filter(&divot_cache[next_line_x], viaa_cache[next_line_x], viaa_cache[line_x],
                                 viaa_cache[far_line_x]);
                    divot_cache_marker = next_line_x;
                } else if (next_line_x > divot_cache_marker) {
                    // (Current line) Next divot result not cached, run for next pixel
                    divot_filter(&divot_cache[next_line_x], viaa_cache[next_line_x], viaa_cache[line_x],
                                 viaa_cache[far_line_x]);
                    divot_cache_marker = next_line_x;
                }

                if (line_x > divot_cache_next_marker) {
                    // (Next line) Divot result not cached, run for current pixel and next pixel
                    divot_filter(&divot_cache_next[line_x], viaa_cache_next[line_x], viaa_cache_next[prev_line_x],
                                 viaa_cache_next[next_line_x]);
                    divot_filter(&divot_cache_next[next_line_x], viaa_cache_next[next_line_x], viaa_cache_next[line_x],
                                 viaa_cache_next[far_line_x]);
                    divot_cache_next_marker = next_line_x;
                } else if (next_line_x > divot_cache_next_marker) {
                    // (Next line) Next divot result not cached, run for next pixel
                    divot_filter(&divot_cache_next[next_line_x], viaa_cache_next[next_line_x], viaa_cache_next[line_x],
                                 viaa_cache_next[far_line_x]);
                    divot_cache_next_marker = next_line_x;
                }

                // Output divot-filtered pixel
                color = divot_cache[line_x];
            } else {
                // Carry pixel from AA+restore
                color = viaa_cache[line_x];
            }

            // Whether the scaler is enabled
            bool lerping = ctrl.aa_mode != VI_AA_REPLICATE && (xfrac || yfrac);

            if (lerping) {
                // Scaler is enabled

                if (ctrl.divot_enable) {
                    // Divot was enabled, use divot cache
                    nextcolor = divot_cache[next_line_x];
                    scancolor = divot_cache_next[line_x];
                    scannextcolor = divot_cache_next[next_line_x];
                } else {
                    // Divot was not enabled, don't use divot cache
                    nextcolor = viaa_cache[next_line_x];
                    scancolor = viaa_cache_next[line_x];
                    scannextcolor = viaa_cache_next[next_line_x];
                }

                // LERP colors based on x and y fractions, LERP first in y then in x as in
                // LERP(LERP(a,c), LERP(b,d))
                // a  b
                // |--|
                // c  d
                vi_vl_lerp(&color, scancolor, yfrac);
                vi_vl_lerp(&nextcolor, scannextcolor, yfrac);
                vi_vl_lerp(&color, nextcolor, xfrac);
            } else if (vinnglitch) {
                // Replicate glitch state

                if (prev_line_x & vinnglitch) {
                    // every 64 (rgba16) or 32 (rgba32) pixels, output 0?
                    color.r = color.g = color.b = 0;
                } else {
                    // only sample first 64/32 pixels of the framebuffer
                    cur_x = pixels + (prev_line_x & (vinnglitch - 1));
                    vi_fetch_filter_ptr(&color, frame_buffer, cur_x, ctrl, vres, 0);

                    if (ctrl.divot_enable) {
                        // if enabled, do divot for this pixel
                        struct n64video_pixel prevcol, nextcol;
                        prev_x = pixels + ((prev_line_x - 1) & (vinnglitch - 1));
                        next_x = pixels + (line_x & (vinnglitch - 1));
                        vi_fetch_filter_ptr(&prevcol, frame_buffer, prev_x, ctrl, vres, 0);
                        vi_fetch_filter_ptr(&nextcol, frame_buffer, next_x, ctrl, vres, 0);
                        divot_filter(&color, color, prevcol, nextcol);
                    }
                }
            }

            struct n64video_pixel *pixel = &pixel_row[x];

            if (x >= minhpass && x < maxhpass) {
                // if x is not in the "overscan" region, apply gamma filters
                *pixel = color;
                gamma_filters(pixel, ctrl.gamma_enable, ctrl.gamma_dither_enable, &state[worker_id].vi_rseed);
            } else {
                // if x is in the "overscan" region, black it out
                pixel->r = pixel->g = pixel->b = 0;
            }
        }

        // if the Y_SCALE is 1.0 carry cache results to next output line?
        if (!cache_init && y_add == 0x400) {
            cache_marker = cache_next_marker;
            cache_next_marker = cache_marker_init;

            struct n64video_pixel *tempccvgptr = viaa_cache;
            viaa_cache = viaa_cache_next;
            viaa_cache_next = tempccvgptr;
            if (ctrl.divot_enable) {
                divot_cache_marker = divot_cache_next_marker;
                divot_cache_next_marker = cache_marker_init;
                tempccvgptr = divot_cache;
                divot_cache = divot_cache_next;
                divot_cache_next = tempccvgptr;
            }

            cache_init = true;
        }
    }
}

static bool
vi_process_full(struct n64video_frame_buffer *fb)
{
    bool isblank = (ctrl.type & 2) == 0;
    bool validinterlace = !isblank && ctrl.serrate;

    if (validinterlace) {
        if (prevserrate && emucontrolsvicurrent < 0) {
            emucontrolsvicurrent = v_current_line != prevvicurrent;
        }

        if (emucontrolsvicurrent == 1) {
            lowerfield = v_current_line ^ 1;
        } else if (!emucontrolsvicurrent) {
            if (v_start == oldvstart) {
                lowerfield ^= true;
            } else {
                lowerfield = v_start < oldvstart;
            }
        }

        prevvicurrent = v_current_line;
    }

    oldvstart = v_start;
    prevserrate = validinterlace;

    bool validh = hres > 0 && h_start < PRESCALE_WIDTH;
    int32_t h_end = hres + h_start; // note: the result appears to be different to VI_H_END
    int32_t hrightblank = PRESCALE_WIDTH - h_end;

    if (isblank && prevwasblank) {
        return false;
    }

    prevwasblank = isblank;

    linecount = PRESCALE_WIDTH << ctrl.serrate;
    prescale_ptr = v_start * linecount + h_start + (lowerfield ? PRESCALE_WIDTH : 0);

    int32_t i;
    if (isblank) {
        // blank signal, clear entire screen buffer
        memset(tvfadeoutstate, 0, PRESCALE_HEIGHT * sizeof(uint32_t));
        memset(prescale, 0, sizeof(prescale));
    } else {
        // clear left border
        int32_t j;
        if (h_start > 0 && h_start < PRESCALE_WIDTH) {
            for (i = 0; i < vactivelines; i++) {
                memset(&prescale[i * PRESCALE_WIDTH], 0, h_start * sizeof(uint32_t));
            }
        }

        // clear right border
        if (h_end >= 0 && h_end < PRESCALE_WIDTH) {
            for (i = 0; i < vactivelines; i++) {
                memset(&prescale[i * PRESCALE_WIDTH + h_end], 0, hrightblank * sizeof(uint32_t));
            }
        }

        // clear top border
        for (i = 0; i < ((v_start << ctrl.serrate) + lowerfield); i++) {
            if (tvfadeoutstate[i]) {
                tvfadeoutstate[i]--;
                if (!tvfadeoutstate[i]) {
                    if (validh) {
                        memset(&prescale[i * PRESCALE_WIDTH + h_start], 0, hres * sizeof(uint32_t));
                    } else {
                        memset(&prescale[i * PRESCALE_WIDTH], 0, PRESCALE_WIDTH * sizeof(uint32_t));
                    }
                }
            }
        }

        if (!ctrl.serrate) {
            for (j = 0; j < vres; j++) {
                if (validh) {
                    tvfadeoutstate[i] = 2;
                } else if (tvfadeoutstate[i]) {
                    tvfadeoutstate[i]--;
                    if (!tvfadeoutstate[i]) {
                        memset(&prescale[i * PRESCALE_WIDTH], 0, PRESCALE_WIDTH * sizeof(uint32_t));
                    }
                }

                i++;
            }
        } else {
            for (j = 0; j < vres; j++) {
                if (validh) {
                    tvfadeoutstate[i] = 2;
                } else if (tvfadeoutstate[i]) {
                    tvfadeoutstate[i]--;
                    if (!tvfadeoutstate[i]) {
                        memset(&prescale[i * PRESCALE_WIDTH], 0, PRESCALE_WIDTH * sizeof(uint32_t));
                    }
                }

                if (tvfadeoutstate[i + 1]) {
                    tvfadeoutstate[i + 1]--;
                    if (!tvfadeoutstate[i + 1]) {
                        if (validh) {
                            memset(&prescale[(i + 1) * PRESCALE_WIDTH + h_start], 0, hres * sizeof(uint32_t));
                        } else {
                            memset(&prescale[(i + 1) * PRESCALE_WIDTH], 0, PRESCALE_WIDTH * sizeof(uint32_t));
                        }
                    }
                }

                i += 2;
            }
        }

        // clear bottom border
        for (; i < vactivelines; i++) {
            if (tvfadeoutstate[i]) {
                tvfadeoutstate[i]--;
            }
            if (!tvfadeoutstate[i]) {
                if (validh) {
                    memset(&prescale[i * PRESCALE_WIDTH + h_start], 0, hres * sizeof(uint32_t));
                } else {
                    memset(&prescale[i * PRESCALE_WIDTH], 0, PRESCALE_WIDTH * sizeof(uint32_t));
                }
            }
        }
    }

    if (!validh) {
        return false;
    }

    // run filter update in parallel if enabled
    if (config.parallel) {
        parallel_run(vi_process_full_parallel);
    } else {
        vi_process_full_parallel(0);
    }

    // finish and send buffer to screen
    fb->pixels = prescale;
    fb->pitch = PRESCALE_WIDTH;

    if (config.vi.hide_overscan) {
        // crop away overscan area from prescale
        fb->width = maxhpass - minhpass;
        fb->height = vres << ctrl.serrate;
        fb->height_out = (vres << 1) * V_SYNC_NTSC / v_sync;
        int32_t x = h_start + minhpass;
        int32_t y = (v_start + (emucontrolsvicurrent ? lowerfield : 0)) << ctrl.serrate;
        fb->pixels += x + y * fb->pitch;
    } else {
        // use entire prescale buffer
        fb->width = PRESCALE_WIDTH;
        fb->height = (ispal ? V_RES_PAL : V_RES_NTSC) >> !ctrl.serrate;
        fb->height_out = V_RES_NTSC;
    }

    // convert to 16:9 if enabled
    if (config.vi.widescreen) {
        fb->height_out = fb->height_out * 3 / 4;
    }

    return fb->width > 0 && fb->height > 0;
}

static void
vi_process_fast_parallel(uint32_t worker_id)
{
    int32_t y;
    int32_t y_begin = 0;
    int32_t y_end = vres_raw;
    int32_t y_inc = 1;

    // drop every other interlaced frame to avoid "wobbly" output due to the
    // vertical offset
    // TODO: completely skip rendering these frames in unfiltered to improve
    // performance?
    if (ctrl.serrate && v_current_line) {
        return;
    }

    if (config.parallel) {
        y_begin = worker_id;
        y_inc = parallel_num_workers();
    }

    for (y = y_begin; y < y_end; y += y_inc) {
        int32_t x;
        int32_t line = y * vi_width_low;

        struct n64video_pixel *pixel_row = &prescale[y * hres_raw];

        for (x = 0; x < hres_raw; x++) {
            struct n64video_pixel *pixel = &pixel_row[x];

            switch (config.vi.mode) {
                case VI_MODE_NORMAL:
                case VI_MODE_NUM:
                    break;

                case VI_MODE_COLOR:
                    switch (ctrl.type) {
                        case VI_TYPE_RGBA5551:
                            {
                                uint16_t pix = rdram_read_idx16((frame_buffer >> 1) + line + x);
                                pixel->r = (uint8_t)RGBA16_R(pix);
                                pixel->g = (uint8_t)RGBA16_G(pix);
                                pixel->b = (uint8_t)RGBA16_B(pix);
                                break;
                            }

                        case VI_TYPE_RGBA8888:
                            {
                                uint32_t pix = rdram_read_idx32((frame_buffer >> 2) + line + x);
                                pixel->r = (uint8_t)RGBA32_R(pix);
                                pixel->g = (uint8_t)RGBA32_G(pix);
                                pixel->b = (uint8_t)RGBA32_B(pix);
                                break;
                            }

                        default:
                            return;
                    }
                    gamma_filters(pixel, ctrl.gamma_enable, false, &state[worker_id].vi_rseed);
                    break;

                case VI_MODE_DEPTH:
                    if (zb_address) {
                        pixel->r = pixel->g = pixel->b = rdram_read_idx16((zb_address >> 1) + line + x) >> 8;
                    }
                    break;

                case VI_MODE_COVERAGE:
                    {
                        // TODO: incorrect for RGBA8888?
                        uint8_t hval;
                        uint16_t pix;
                        rdram_read_pair16(&pix, &hval, (frame_buffer >> 1) + line + x);
                        pixel->r = pixel->g = pixel->b = (((pix & 1) << 2) | hval) << 5;
                    }
                    break;
            }
        }
    }
}

static bool
vi_process_fast(struct n64video_frame_buffer *fb)
{
    // note: this is probably a very, very crude method to get the frame size,
    // but should hopefully work most of the time
    hres_raw = (int32_t)x_add * hres / 1024;
    vres_raw = (int32_t)y_add * vres / 1024;

    // skip invalid frame sizes
    if (hres_raw <= 0 || vres_raw <= 0) {
        return false;
    }

    // skip blank/invalid modes
    if (!(ctrl.type & 2)) {
        return false;
    }

    // run filter update in parallel if enabled
    if (config.parallel) {
        parallel_run(vi_process_fast_parallel);
    } else {
        vi_process_fast_parallel(0);
    }

    // finish and send buffer to screen
    fb->pixels = prescale;
    fb->width = hres_raw;
    fb->height = vres_raw;
    fb->pitch = hres_raw;

    // get display size of filtered mode
    int32_t filtered_width = maxhpass - minhpass;
    int32_t filtered_height = (vres << 1) * V_SYNC_NTSC / v_sync;

    // re-calculate cropped 8 pixel area on the left and right from filtered mode
    int32_t border_width = (hres - filtered_width) * hres_raw / hres;
    fb->pixels += (border_width / 2) + 1;
    fb->width -= border_width;

    // force aspect ratio of filtered mode
    fb->height_out = fb->width * filtered_height / filtered_width;

    // convert to 16:9 if enabled
    if (config.vi.widescreen) {
        fb->height_out = fb->height_out * 3 / 4;
    }

    return fb->width > 0 && fb->height > 0;
}

static void
vi_set_zbuffer_address(uint32_t address)
{
    zb_address = address;
}

void
n64video_update_screen(struct n64video_frame_buffer *fb)
{
    // check for configuration errors
    if (config.vi.mode >= VI_MODE_NUM) {
        msg_error("Invalid VI mode: %d", config.vi.mode);
    }

    // parse and check some common registers
    vi_reg_ptr = config.gfx.vi_reg;

    v_start = (*vi_reg_ptr[VI_V_START] >> 16) & 0x3ff;
    h_start = (*vi_reg_ptr[VI_H_START] >> 16) & 0x3ff;

    int32_t v_end = *vi_reg_ptr[VI_V_START] & 0x3ff;
    int32_t h_end = *vi_reg_ptr[VI_H_START] & 0x3ff;

    hres = h_end - h_start;
    vres = (v_end - v_start) >> 1; // vertical is measured in half-lines

    x_add = *vi_reg_ptr[VI_X_SCALE] & 0xfff;           // X_SCALE
    x_start = (*vi_reg_ptr[VI_X_SCALE] >> 16) & 0xfff; // X_OFFSET

    y_add = *vi_reg_ptr[VI_Y_SCALE] & 0xfff;           // Y_SCALE
    y_start = (*vi_reg_ptr[VI_Y_SCALE] >> 16) & 0xfff; // Y_OFFSET

    v_sync = *vi_reg_ptr[VI_V_SYNC] & 0x3ff;
    v_current_line = *vi_reg_ptr[VI_V_CURRENT_LINE] & 1;

    vi_width_low = *vi_reg_ptr[VI_WIDTH] & 0xfff;
    frame_buffer = *vi_reg_ptr[VI_ORIGIN] & 0xffffff;

    // If the AA mode is replicate, the VI is enabled, h_start is sufficiently small, and the x scale is sufficiently
    // small, the VI generates corrupted output
    if (ctrl.aa_mode == VI_AA_REPLICATE && (ctrl.type & 2) && h_start < (ctrl.type == VI_TYPE_RGBA5551 ? 0x80 : 0x40) &&
        x_add <= 0x200) {
        vinnglitch = ctrl.type == VI_TYPE_RGBA5551 ? 0x40 : 0x20;
    } else {
        vinnglitch = 0;
    }

    // cancel if the frame buffer contains no valid address
    if (!frame_buffer) {
        fb->valid = false;
        return;
    }

    // split up VI_CONTROL bits
    uint32_t vi_control = *vi_reg_ptr[VI_STATUS];
    ctrl.type = vi_control & 3;
    ctrl.gamma_dither_enable = (vi_control >> 2) & 1;
    ctrl.gamma_enable = (vi_control >> 3) & 1;
    ctrl.divot_enable = (vi_control >> 4) & 1;
    ctrl.vbus_clock_enable = (vi_control >> 5) & 1;
    ctrl.serrate = (vi_control >> 6) & 1;
    ctrl.test_mode = (vi_control >> 7) & 1;
    ctrl.aa_mode = (vi_control >> 8) & 3;
    ctrl.kill_we = (vi_control >> 11) & 1;
    ctrl.pixel_advance = (vi_control >> 12) & 0x7;
    ctrl.dither_filter_enable = (vi_control >> 16) & 1;

    // check for unexpected VI type bits set
    if (ctrl.type & ~3) {
        msg_error("Unknown framebuffer format %d", ctrl.type);
    }

    // check for the dangerous vbus_clock_enable flag. it was introduced to
    // configure Ultra 64 prototypes and enabling it on final hardware will
    // enable two output drivers on the same bus at the same time
    if (ctrl.vbus_clock_enable && !onetimewarnings.vbusclock) {
        msg_warning("vi_update: vbus_clock_enable bit set in VI_CONTROL_REG "
                    "register. Never run this code on your N64! It's rumored "
                    "that turning this bit on will result in permanent damage "
                    "to the hardware! Emulation will now continue.");
        onetimewarnings.vbusclock = true;
    }

    // adjust sizes and offsets
    // this code is surely not accurate to hardware and is based on an incomplete understanding of the VI
    ispal = v_sync > (V_SYNC_NTSC + 25);
    h_start -= (ispal ? 128 : 108);

    bool h_start_clamped = false;

    if (h_start < 0) {
        // H_START is negative, adjust X_OFFSET by X_SCALE * -H_START
        x_start += (x_add * (-h_start));
        // hres = H_END
        hres += h_start;
        // clamp to 0
        h_start = 0;
        h_start_clamped = true;
    }

    int32_t vstartoffset = ispal ? 44 : 34;
    v_start = (v_start - vstartoffset) / 2;

    if (v_start < 0) {
        y_start += (y_add * (uint32_t)(-v_start));
        v_start = 0;
    }

    bool hres_clamped = false;

    if ((hres + h_start) > PRESCALE_WIDTH) {
        // if H_END > 640, clamp to 640
        hres = PRESCALE_WIDTH - h_start;
        hres_clamped = true;
    }

    if ((vres + v_start) > PRESCALE_HEIGHT) {
        vres = PRESCALE_HEIGHT - v_start;
        msg_warning("vres = %d v_start = %d v_video_start = %d", vres, v_start,
                    (*vi_reg_ptr[VI_V_START] >> 16) & 0x3ff);
    }

    vactivelines = v_sync - vstartoffset;

    if (vactivelines > PRESCALE_HEIGHT) {
        msg_error("VI_V_SYNC_REG too big");
    }

    fb->valid = true;

    if (vactivelines >= 0) {
        uint32_t lineshifter = !ctrl.serrate;
        vactivelines >>= lineshifter;

        // if H_START was clamped, the first 8 pixels are sampled properly otherwise they aren't sampled in time to be
        // displayed?
        minhpass = h_start_clamped ? 0 : 8;
        // if H_END was clamped, the last 7 pixels are sampled properly otherwise they're cut off?
        maxhpass = hres_clamped ? hres : (hres - 7);

        // run filter update in parallel if enabled
        if (config.vi.mode == VI_MODE_NORMAL) {
            fb->valid = vi_process_full(fb);
        } else {
            fb->valid = vi_process_fast(fb);
        }
    }
}

static void
vi_close(void)
{
}

#endif // N64VIDEO_C
