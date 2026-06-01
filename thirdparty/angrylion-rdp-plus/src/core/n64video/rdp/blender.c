#ifdef N64VIDEO_C

static int32_t blenderone = 0xff;

static uint8_t bldiv_hwaccurate_table[0x8000];

#define BL_RGB_IN  0
#define BL_RGB_MEM 1
#define BL_RGB_BL  2
#define BL_RGB_FOG 3

#define BL_A_IN    0
#define BL_A_FOG   1
#define BL_A_SHADE 2
#define BL_A_0     3

#define BL_A_1MA 0
#define BL_A_MEM 1
#define BL_A_1   2

static INLINE void
set_blender_input(struct rdp_state *wstate, int cycle, int which, int32_t **input_r, int32_t **input_g,
                  int32_t **input_b, int32_t **input_a, int a, int b)
{
    // In the first cycle, RGB_IN comes from the final combiner stage.
    // In the second cycle, it comes from the first blender cycle
    struct color *cycle_color = (cycle == 0) ? &wstate->pixel_color : &wstate->blended_pixel_color;

    // Select color input, same for p or m
    switch (a & 3) {
        case_no_default;

        case BL_RGB_IN:
            *input_r = &cycle_color->r;
            *input_g = &cycle_color->g;
            *input_b = &cycle_color->b;
            break;
        case BL_RGB_MEM:
            *input_r = &wstate->memory_color.r;
            *input_g = &wstate->memory_color.g;
            *input_b = &wstate->memory_color.b;
            break;
        case BL_RGB_BL:
            *input_r = &wstate->blend_color.r;
            *input_g = &wstate->blend_color.g;
            *input_b = &wstate->blend_color.b;
            break;
        case BL_RGB_FOG:
            *input_r = &wstate->fog_color.r;
            *input_g = &wstate->fog_color.g;
            *input_b = &wstate->fog_color.b;
            break;
    }

    // Select alpha input, different for a or b
    if (which == 0) {
        // clang-format off
        switch (b & 3) {
            case BL_A_IN:    *input_a = &wstate->pixel_color.a;       break;
            case BL_A_FOG:   *input_a = &wstate->fog_color.a;         break;
            case BL_A_SHADE: *input_a = &wstate->blender_shade_alpha; break;
            case BL_A_0:     *input_a = &zero_color;                  break;
            case_no_default;
        }
        // clang-format on
    } else {
        // clang-format off
        switch (b & 3) {
            case BL_A_1MA: *input_a = &wstate->inv_pixel_color.a; break;
            case BL_A_MEM: *input_a = &wstate->memory_color.a;    break;
            case BL_A_1:   *input_a = &blenderone;                break;
            case BL_A_0:   *input_a = &zero_color;                break;
            case_no_default;
        }
        // clang-format on
    }
}

/**
 * Compute alpha compare result.
 */
static STRICTINLINE int
alpha_compare(struct rdp_state *wstate, int32_t comb_alpha)
{
    int32_t threshold;

    // Always pass when alpha compare is disabled
    if (!wstate->other_modes.alpha_compare_en)
        return 1;

    // Select source to compare to
    //  - dither    = random
    //  - no dither = blend color register alpha
    if (wstate->other_modes.dither_alpha_en)
        threshold = irand(&wstate->rseed) & 0xff;
    else
        threshold = wstate->blend_color.a;

    // If pixel alpha is >= comparison, this pixel passes
    return comb_alpha >= threshold;
}

/**
 * Blender output for the g channel only, in the final stage.
 */
static STRICTINLINE int
blender_equation_cycle_gval(struct rdp_state *wstate, int cycle)
{
    int blend1a = *wstate->blender1b_a[cycle] >> 3;
    int blend2a = *wstate->blender2b_a[cycle] >> 3;
    if (wstate->blender2b_a[cycle] == &wstate->memory_color.a) {
        blend1a = (blend1a >> wstate->blshifta) & 0x3C;
        blend2a = (blend2a >> wstate->blshiftb) | 3;
    }

    int mulb = blend2a + 1;
    int blg = (*wstate->blender1a_g[cycle]) * blend1a + (*wstate->blender2a_g[cycle]) * mulb;

    if (wstate->other_modes.force_blend) {
        return blg >> 5 & 0xff;
    } else {
        int sum = ((blend1a & ~3) + (blend2a & ~3) + 4) << 9;
        return bldiv_hwaccurate_table[sum | ((blg >> 2) & 0x7ff)];
    }
}

/**
 * Blender output for the first cycle, g channel only.
 */
static STRICTINLINE void
blender_2cycle_cycle0_gval(struct rdp_state *wstate, uint32_t curpixel)
{
    int fbsel = wstate->fb_size;

    if (wstate->fb_size == PIXEL_SIZE_8BIT) {
        uint32_t fb = wstate->fb_address + curpixel;
        if (!(fb & 1))
            fbsel--;
    }

    if (fbsel & 1) {
        wstate->inv_pixel_color.a = (~(*wstate->blender1b_a[0])) & 0xff;

        int blend1a = *wstate->blender1b_a[0] >> 3;
        int blend2a = *wstate->blender2b_a[0] >> 3;
        if (wstate->blender2b_a[0] == &wstate->memory_color.a) {
            blend1a = (blend1a >> wstate->pastblshifta) & 0x3C;
            blend2a = (blend2a >> wstate->pastblshiftb) | 3;
        }

        int mulb = blend2a + 1;
        int g = (*wstate->blender1a_g[0]) * blend1a + (*wstate->blender2a_g[0]) * mulb;

        wstate->blended_pixel_color.g = g >> 5 & 0xff;
    }
}

/**
 * Blender final stage.
 * Second cycle for 2-cycle mode, or only cycle of 1-cycle mode.
 */
static STRICTINLINE void
blender_finalstage(struct rdp_state *wstate, uint32_t *fr, uint32_t *fg, uint32_t *fb, int dith, uint32_t blend_en,
                   uint32_t prewrap, bool partialreject, bool cycle)
{
    int r, g, b;

    // Prewrap is whether coverage overflowed
    if (!wstate->other_modes.color_on_cvg || prewrap) {
        // Either color_on_cvg is disabled or coverage overflowed, perform blending
        if (!blend_en || (partialreject && wstate->pixel_color.a >= 0xff)) {
            // Blender disabled or formula is trivial, take 1A input as-is
            r = *wstate->blender1a_r[cycle];
            g = *wstate->blender1a_g[cycle];
            b = *wstate->blender1a_b[cycle];
        } else {
            // Compute value for "1 - Alpha" input
            // Notice that it inverts the selected input, it is not fixed to pixel alpha
            wstate->inv_pixel_color.a = (~(*wstate->blender1b_a[cycle])) & 0xff;

            // Run the blending formula

            int blend1a = *wstate->blender1b_a[cycle] >> 3;
            int blend2a = *wstate->blender2b_a[cycle] >> 3;
            if (wstate->blender2b_a[cycle] == &wstate->memory_color.a) {
                blend1a = (blend1a >> wstate->blshifta) & 0x3C;
                blend2a = (blend2a >> wstate->blshiftb) | 3;
            }

            int mulb = blend2a + 1;
            int blr = (*wstate->blender1a_r[cycle]) * blend1a + (*wstate->blender2a_r[cycle]) * mulb;
            int blg = (*wstate->blender1a_g[cycle]) * blend1a + (*wstate->blender2a_g[cycle]) * mulb;
            int blb = (*wstate->blender1a_b[cycle]) * blend1a + (*wstate->blender2a_b[cycle]) * mulb;

            if (wstate->other_modes.force_blend) {
                r = blr >> 5 & 0xff;
                g = blg >> 5 & 0xff;
                b = blb >> 5 & 0xff;
            } else {
                int sum = ((blend1a & ~3) + (blend2a & ~3) + 4) << 9;
                r = bldiv_hwaccurate_table[sum | ((blr >> 2) & 0x7ff)];
                g = bldiv_hwaccurate_table[sum | ((blg >> 2) & 0x7ff)];
                b = bldiv_hwaccurate_table[sum | ((blb >> 2) & 0x7ff)];
            }
        }
    } else {
        // Take blender 2A input as-is for color-on-cvg when no overflow took place
        r = *wstate->blender2a_r[cycle];
        g = *wstate->blender2a_g[cycle];
        b = *wstate->blender2a_b[cycle];
    }

    // Apply dither
    if (wstate->other_modes.rgb_dither_sel != 3)
        rgb_dither(wstate->other_modes.rgb_dither_sel, &r, &g, &b, dith);

    *fr = r;
    *fg = g;
    *fb = b;
}

/**
 * First cycle for 2-cycle mode
 */
static STRICTINLINE void
blender_2cycle_cycle0(struct rdp_state *wstate)
{
    // Invert pixel alpha (for G_BL_A_IN input)
    wstate->inv_pixel_color.a = (~(*wstate->blender1b_a[0])) & 0xff;

    // Get alpha channel values from selected inputs, 5 most significant bits
    int blend1a = *wstate->blender1b_a[0] >> 3;
    int blend2a = *wstate->blender2b_a[0] >> 3;

    // If memory coverage is used, shift the alpha inputs. This is for reducing punch-through
    // artifacts by prioritizing blending of the surface with smaller dz.
    // The shift will be anywhere from 0 to 4 depending on:
    //  - the dz difference (if z_cmp && z_src_sel == PIXEL)
    //  - saved dz (if !z_cmp && z_src_sel == PIXEL)
    //  - prim dz (if z_src_sel == PRIM)
    if (wstate->blender2b_a[0] == &wstate->memory_color.a) {
        blend1a = (blend1a >> wstate->pastblshifta) & 0x3C;
        blend2a = (blend2a >> wstate->pastblshiftb) | 3;
    }

    // Compute p * a + m * b
    int mulb = blend2a + 1;
    int r = (*wstate->blender1a_r[0]) * blend1a + (*wstate->blender2a_r[0]) * mulb;
    int g = (*wstate->blender1a_g[0]) * blend1a + (*wstate->blender2a_g[0]) * mulb;
    int b = (*wstate->blender1a_b[0]) * blend1a + (*wstate->blender2a_b[0]) * mulb;
    // Since p and m are 0.8 fixed point and a and b are 0.5 and 1.5 respectively,
    // the result is a 13-bit value. We want an 8 bit result, so take the 8 most
    // significant bits and ignore the 5 least significant bits.
    wstate->blended_pixel_color.r = r >> 5 & 0xff;
    wstate->blended_pixel_color.g = g >> 5 & 0xff;
    wstate->blended_pixel_color.b = b >> 5 & 0xff;
}

static void
blender_init_lut(void)
{
    // Precomputes values for the fixed-point division of an
    // q0.11 number and a q1.3 number

    for (int i = 0; i < 0x8000; i++) {
        uint8_t quotient = 0;
        int dividend = i & 0x7ff;      // 0.11 fixed point
        int divisor = (i >> 11) & 0xf; // 1.3 fixed point
        int inv_divisor = (~divisor) & 0xf;

        int temp = inv_divisor + (dividend >> 8) + 1;
        int ps[9];
        ps[0] = temp & 7;

        for (int k = 0; k < 8; k++) {
            int nbit = (dividend >> (7 - k)) & 1;

            if (quotient & (0x100 >> k))
                temp = inv_divisor + (ps[k] << 1) + nbit + 1;
            else
                temp = divisor + (ps[k] << 1) + nbit + 0;

            ps[k + 1] = temp & 7;
            if (temp & 0x10)
                quotient |= (1 << (7 - k));
        }
        bldiv_hwaccurate_table[i] = quotient;
    }
}

void
rdp_set_fog_color(struct rdp_state *wstate, const uint32_t *args)
{
    wstate->fog_color.r = RGBA32_R(args[1]);
    wstate->fog_color.g = RGBA32_G(args[1]);
    wstate->fog_color.b = RGBA32_B(args[1]);
    wstate->fog_color.a = RGBA32_A(args[1]);
}

void
rdp_set_blend_color(struct rdp_state *wstate, const uint32_t *args)
{
    wstate->blend_color.r = RGBA32_R(args[1]);
    wstate->blend_color.g = RGBA32_G(args[1]);
    wstate->blend_color.b = RGBA32_B(args[1]);
    wstate->blend_color.a = RGBA32_A(args[1]);
}

#endif // N64VIDEO_C
