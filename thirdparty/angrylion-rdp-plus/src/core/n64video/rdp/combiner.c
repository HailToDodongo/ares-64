#ifdef N64VIDEO_C

static uint32_t special_9bit_clamptable[512];
static int32_t special_9bit_exttable[512];

#define COLORPTR_UNPACK(out, ptr) \
    do {                          \
        *(out##_r) = &(ptr)->r;   \
        *(out##_g) = &(ptr)->g;   \
        *(out##_b) = &(ptr)->b;   \
    } while (0)

#define COLORPTR_DISTRIB(out, value) \
    do {                             \
        *(out##_r) = (value);        \
        *(out##_g) = (value);        \
        *(out##_b) = (value);        \
    } while (0)

static INLINE void
set_suba_rgb_input(struct rdp_state *wstate, int32_t **input_r, int32_t **input_g, int32_t **input_b, int code)
{
    // clang-format off
    switch (code & 0xF) {
        case 0:  COLORPTR_UNPACK(input,  &wstate->combined_color); break;
        case 1:  COLORPTR_UNPACK(input,  &wstate->texel0_color);   break;
        case 2:  COLORPTR_UNPACK(input,  &wstate->texel1_color);   break;
        case 3:  COLORPTR_UNPACK(input,  &wstate->prim_color);     break;
        case 4:  COLORPTR_UNPACK(input,  &wstate->shade_color);    break;
        case 5:  COLORPTR_UNPACK(input,  &wstate->env_color);      break;
        case 6:  COLORPTR_DISTRIB(input, &one_color);              break;
        case 7:  COLORPTR_DISTRIB(input, &wstate->noise);          break;
        default: COLORPTR_DISTRIB(input, &zero_color);             break;
    }
    // clang-format on
}

static INLINE void
set_subb_rgb_input(struct rdp_state *wstate, int32_t **input_r, int32_t **input_g, int32_t **input_b, int code)
{
    // clang-format off
    switch (code & 0xF) {
        case 0:  COLORPTR_UNPACK(input,  &wstate->combined_color); break;
        case 1:  COLORPTR_UNPACK(input,  &wstate->texel0_color);   break;
        case 2:  COLORPTR_UNPACK(input,  &wstate->texel1_color);   break;
        case 3:  COLORPTR_UNPACK(input,  &wstate->prim_color);     break;
        case 4:  COLORPTR_UNPACK(input,  &wstate->shade_color);    break;
        case 5:  COLORPTR_UNPACK(input,  &wstate->env_color);      break;
        case 6:  COLORPTR_UNPACK(input,  &wstate->key_center);     break;
        case 7:  COLORPTR_DISTRIB(input, &wstate->k4);             break;
        default: COLORPTR_DISTRIB(input, &zero_color);             break;
    }
    // clang-format on
}

static INLINE void
set_mul_rgb_input(struct rdp_state *wstate, int32_t **input_r, int32_t **input_g, int32_t **input_b, int code)
{
    // clang-format off
    switch (code & 0x1F) {
        case 0:  COLORPTR_UNPACK(input,  &wstate->combined_color);     break;
        case 1:  COLORPTR_UNPACK(input,  &wstate->texel0_color);       break;
        case 2:  COLORPTR_UNPACK(input,  &wstate->texel1_color);       break;
        case 3:  COLORPTR_UNPACK(input,  &wstate->prim_color);         break;
        case 4:  COLORPTR_UNPACK(input,  &wstate->shade_color);        break;
        case 5:  COLORPTR_UNPACK(input,  &wstate->env_color);          break;
        case 6:  COLORPTR_UNPACK(input,  &wstate->key_scale);          break;
        case 7:  COLORPTR_DISTRIB(input, &wstate->combined_color.a);   break;
        case 8:  COLORPTR_DISTRIB(input, &wstate->texel0_color.a);     break;
        case 9:  COLORPTR_DISTRIB(input, &wstate->texel1_color.a);     break;
        case 10: COLORPTR_DISTRIB(input, &wstate->prim_color.a);       break;
        case 11: COLORPTR_DISTRIB(input, &wstate->shade_color.a);      break;
        case 12: COLORPTR_DISTRIB(input, &wstate->env_color.a);        break;
        case 13: COLORPTR_DISTRIB(input, &wstate->lod_frac);           break;
        case 14: COLORPTR_DISTRIB(input, &wstate->primitive_lod_frac); break;
        case 15: COLORPTR_DISTRIB(input, &wstate->k5);                 break;
        default: COLORPTR_DISTRIB(input, &zero_color);                 break;
    }
    // clang-format on
}

static INLINE void
set_add_rgb_input(struct rdp_state *wstate, int32_t **input_r, int32_t **input_g, int32_t **input_b, int code)
{
    // clang-format off
    switch (code & 7) {
        case 0: COLORPTR_UNPACK(input, &wstate->combined_color); break;
        case 1: COLORPTR_UNPACK(input, &wstate->texel0_color);   break;
        case 2: COLORPTR_UNPACK(input, &wstate->texel1_color);   break;
        case 3: COLORPTR_UNPACK(input, &wstate->prim_color);     break;
        case 4: COLORPTR_UNPACK(input, &wstate->shade_color);    break;
        case 5: COLORPTR_UNPACK(input, &wstate->env_color);      break;
        case 6: COLORPTR_DISTRIB(input, &one_color);             break;
        case 7: COLORPTR_DISTRIB(input, &zero_color);            break;
        case_no_default;
    }
    // clang-format on
}

static INLINE void
set_sub_alpha_input(struct rdp_state *wstate, int32_t **input, int code)
{
    // clang-format off
    switch (code & 7) {
        case 0: *input = &wstate->combined_color.a; break;
        case 1: *input = &wstate->texel0_color.a;   break;
        case 2: *input = &wstate->texel1_color.a;   break;
        case 3: *input = &wstate->prim_color.a;     break;
        case 4: *input = &wstate->shade_color.a;    break;
        case 5: *input = &wstate->env_color.a;      break;
        case 6: *input = &one_color;                break;
        case 7: *input = &zero_color;               break;
        case_no_default;
    }
    // clang-format on
}

static INLINE void
set_mul_alpha_input(struct rdp_state *wstate, int32_t **input, int code)
{
    // clang-format off
    switch (code & 0x7) {
        case 0: *input = &wstate->lod_frac;           break;
        case 1: *input = &wstate->texel0_color.a;     break;
        case 2: *input = &wstate->texel1_color.a;     break;
        case 3: *input = &wstate->prim_color.a;       break;
        case 4: *input = &wstate->shade_color.a;      break;
        case 5: *input = &wstate->env_color.a;        break;
        case 6: *input = &wstate->primitive_lod_frac; break;
        case 7: *input = &zero_color;                 break;
        case_no_default;
    }
    // clang-format on
}

static STRICTINLINE int32_t
color_combiner_equation(int32_t a, int32_t b, int32_t c, int32_t d)
{
    // Sign-extend inputs
    // special_9bit_exttable[i] = ((i & 0x180) == 0x180) ? (i | ~0x1ff) : (i & 0x1ff)
    a = special_9bit_exttable[a];
    b = special_9bit_exttable[b];
    c = SIGNF(c, 9);
    d = special_9bit_exttable[d] << 8;

    // Compute the combiner formula
    a = (a - b) * c + d;

    // Add bias for round to nearest
    return (a + 0x80) & 0x1ffff;
}

// clang-format off
#define CC_CALC_RGB(wstate, cycle, channel)            \
    color_combiner_equation(                           \
        *(wstate)->combiner_rgbsub_a_##channel[cycle], \
        *(wstate)->combiner_rgbsub_b_##channel[cycle], \
        *(wstate)->combiner_rgbmul_##channel[cycle],   \
        *(wstate)->combiner_rgbadd_##channel[cycle]    \
    )
// clang-format on

// clang-format off
#define CC_CALC_ALPHA(wstate, cycle)           \
    color_combiner_equation(                   \
        *(wstate)->combiner_alphasub_a[cycle], \
        *(wstate)->combiner_alphasub_b[cycle], \
        *(wstate)->combiner_alphamul[cycle],   \
        *(wstate)->combiner_alphaadd[cycle]    \
    )
// clang-format on

static STRICTINLINE int32_t
chroma_key_calc_1(int32_t c, int32_t width)
{
    int32_t key = SIGN(c, 17);
    if (key > 0)
        key = ((key & 0xf) == 8) ? (-key + 0x10) : (-key);
    return key + (width << 4);
}

static STRICTINLINE int32_t
chroma_key_min(struct rdp_state *wstate, struct color *col)
{
    int32_t redkey = chroma_key_calc_1(col->r, wstate->key_width.r);
    int32_t greenkey = chroma_key_calc_1(col->g, wstate->key_width.g);
    int32_t bluekey = chroma_key_calc_1(col->b, wstate->key_width.b);

    // clamp(min3(red, green, blue), 0, 255)
    int32_t keyalpha;
    keyalpha = MIN(redkey, greenkey);
    keyalpha = MIN(bluekey, keyalpha);
    return clamp(keyalpha, 0, 0xff);
}

/**
 * Combiner final stage
 */
static STRICTINLINE void
combiner_finalstage(struct rdp_state *wstate, int adseed, uint32_t *curpixel_cvg)
{
    int32_t keyalpha = 0;
    int32_t cvg = 0;

    //
    // Compute Combined Color
    //

    if (wstate->combiner_rgbmul_r[1] != &zero_color) {
        // Compute full combiner formula when multiplier is not zero
        wstate->combined_color.r = CC_CALC_RGB(wstate, 1, r);
        wstate->combined_color.g = CC_CALC_RGB(wstate, 1, g);
        wstate->combined_color.b = CC_CALC_RGB(wstate, 1, b);
    } else {
        // Otherwise take a shortcut
        wstate->combined_color.r = ((special_9bit_exttable[*wstate->combiner_rgbadd_r[1]] << 8) + 0x80) & 0x1ffff;
        wstate->combined_color.g = ((special_9bit_exttable[*wstate->combiner_rgbadd_g[1]] << 8) + 0x80) & 0x1ffff;
        wstate->combined_color.b = ((special_9bit_exttable[*wstate->combiner_rgbadd_b[1]] << 8) + 0x80) & 0x1ffff;
    }

    //
    // Compute Combined Alpha
    //

    if (wstate->combiner_alphamul[1] != &zero_color)
        wstate->combined_color.a = CC_CALC_ALPHA(wstate, 1) >> 8;
    else
        wstate->combined_color.a = special_9bit_exttable[*wstate->combiner_alphaadd[1]] & 0x1ff;

    wstate->pixel_color.a = special_9bit_clamptable[wstate->combined_color.a];
    if (wstate->pixel_color.a == 0xff)
        wstate->pixel_color.a = 0x100;

    //
    // Chroma Key Stages
    //

    struct color chromabypass;
    struct color *result_color = &wstate->combined_color;

    if (wstate->other_modes.key_en) {
        // This becomes pixel alpha later if !alpha_cvg_sel
        // NOTE chroma key operates on combined rgb prior to shifting back to 9 bits
        keyalpha = chroma_key_min(wstate, &wstate->combined_color);

        // When chroma keying is enabled, the resulting pixel color is just the combiner A input.
        chromabypass = (struct color){
            *wstate->combiner_rgbsub_a_r[1],
            *wstate->combiner_rgbsub_a_g[1],
            *wstate->combiner_rgbsub_a_b[1],
            0,
        };
        result_color = &chromabypass;
    }

    wstate->combined_color.r >>= 8;
    wstate->combined_color.g >>= 8;
    wstate->combined_color.b >>= 8;
    wstate->pixel_color.r = special_9bit_clamptable[result_color->r];
    wstate->pixel_color.g = special_9bit_clamptable[result_color->g];
    wstate->pixel_color.b = special_9bit_clamptable[result_color->b];

    //
    // Alpha Fixup Stages
    //

    // cvg_x_alpha logic

    if (wstate->other_modes.cvg_times_alpha) {
        // Multiply alpha (q1.8) and coverage (4 bit, 0 to 8 inclusive)
        cvg = (wstate->pixel_color.a * (*curpixel_cvg) + 4) >> 3;
        // Resulting coverage is the msbits
        *curpixel_cvg = (cvg >> 5) & 0xF;
    }

    // alpha_cvg_select logic

    if (wstate->other_modes.alpha_cvg_select) {
        if (wstate->other_modes.cvg_times_alpha)
            wstate->pixel_color.a = cvg;
        else
            wstate->pixel_color.a = (*curpixel_cvg) << 5;

        if (wstate->pixel_color.a > 0xff)
            wstate->pixel_color.a = 0xff;
    } else {
        if (wstate->other_modes.key_en) {
            wstate->pixel_color.a = keyalpha;
        } else {
            wstate->pixel_color.a += adseed;
            if (wstate->pixel_color.a & 0x100)
                wstate->pixel_color.a = 0xff;
        }
    }

    // Dither shade alpha to blender

    wstate->blender_shade_alpha = wstate->shade_color.a + adseed;
    if (wstate->blender_shade_alpha & 0x100)
        wstate->blender_shade_alpha = 0xff;
}

/**
 * Combiner in 2-cycle mode, first cycle
 */
static STRICTINLINE void
combiner_2cycle_cycle0(struct rdp_state *wstate, int adseed, uint32_t cvg, uint32_t *acalpha)
{
    //
    //  Compute RGB
    //

    if (wstate->combiner_rgbmul_r[0] != &zero_color) {
        wstate->combined_color.r = CC_CALC_RGB(wstate, 0, r);
        wstate->combined_color.g = CC_CALC_RGB(wstate, 0, g);
        wstate->combined_color.b = CC_CALC_RGB(wstate, 0, b);
    } else {
        wstate->combined_color.r = ((special_9bit_exttable[*wstate->combiner_rgbadd_r[0]] << 8) + 0x80) & 0x1ffff;
        wstate->combined_color.g = ((special_9bit_exttable[*wstate->combiner_rgbadd_g[0]] << 8) + 0x80) & 0x1ffff;
        wstate->combined_color.b = ((special_9bit_exttable[*wstate->combiner_rgbadd_b[0]] << 8) + 0x80) & 0x1ffff;
    }

    wstate->combined_color.r >>= 8;
    wstate->combined_color.g >>= 8;
    wstate->combined_color.b >>= 8;

    //
    //  Compute Alpha
    //

    if (wstate->combiner_alphamul[0] != &zero_color)
        wstate->combined_color.a = CC_CALC_ALPHA(wstate, 0) >> 8;
    else
        wstate->combined_color.a = special_9bit_exttable[*wstate->combiner_alphaadd[0]] & 0x1ff;

    //
    //  Do Alpha Compare (hardware pipelining bug)
    //

    if (wstate->other_modes.alpha_compare_en) {
        int32_t preacalpha = special_9bit_clamptable[wstate->combined_color.a];
        if (preacalpha == 0xff)
            preacalpha = 0x100;

        // This repeats some of the alpha fixup logic

        if (wstate->other_modes.alpha_cvg_select) {
            if (wstate->other_modes.cvg_times_alpha)
                preacalpha = (preacalpha * cvg + 4) >> 3;
            else
                preacalpha = cvg << 5;

            if (preacalpha > 0xff)
                preacalpha = 0xff;
        } else {
            preacalpha += adseed;
            if (preacalpha & 0x100)
                preacalpha = 0xff;
        }

        *acalpha = preacalpha;
    }

    //
    //  Dither shade alpha to blender
    //

    wstate->blender_shade_alpha = wstate->shade_color.a + adseed;
    if (wstate->blender_shade_alpha & 0x100)
        wstate->blender_shade_alpha = 0xff;
}

static void
combiner_init_lut(void)
{
    int i;
    for (i = 0; i < 0x200; i++) {
        switch ((i >> 7) & 3) {
            case 0:
            case 1:
                special_9bit_clamptable[i] = i & 0xff;
                break;
            case 2: // saturate
                special_9bit_clamptable[i] = 0xff;
                break;
            case 3: // wrap
                special_9bit_clamptable[i] = 0;
                break;
        }
    }

    // Precomputed values for sign-extending a 9 bit value to 32 bits
    for (i = 0; i < 0x200; i++) {
        special_9bit_exttable[i] = ((i & 0x180) == 0x180) ? (i | ~0x1ff) : (i & 0x1ff);
    }
}

static void
combiner_init(struct rdp_state *wstate)
{
    wstate->combiner_rgbsub_a_r[0] = wstate->combiner_rgbsub_a_r[1] = &one_color;
    wstate->combiner_rgbsub_a_g[0] = wstate->combiner_rgbsub_a_g[1] = &one_color;
    wstate->combiner_rgbsub_a_b[0] = wstate->combiner_rgbsub_a_b[1] = &one_color;
    wstate->combiner_rgbsub_b_r[0] = wstate->combiner_rgbsub_b_r[1] = &one_color;
    wstate->combiner_rgbsub_b_g[0] = wstate->combiner_rgbsub_b_g[1] = &one_color;
    wstate->combiner_rgbsub_b_b[0] = wstate->combiner_rgbsub_b_b[1] = &one_color;
    wstate->combiner_rgbmul_r[0] = wstate->combiner_rgbmul_r[1] = &one_color;
    wstate->combiner_rgbmul_g[0] = wstate->combiner_rgbmul_g[1] = &one_color;
    wstate->combiner_rgbmul_b[0] = wstate->combiner_rgbmul_b[1] = &one_color;
    wstate->combiner_rgbadd_r[0] = wstate->combiner_rgbadd_r[1] = &one_color;
    wstate->combiner_rgbadd_g[0] = wstate->combiner_rgbadd_g[1] = &one_color;
    wstate->combiner_rgbadd_b[0] = wstate->combiner_rgbadd_b[1] = &one_color;

    wstate->combiner_alphasub_a[0] = wstate->combiner_alphasub_a[1] = &one_color;
    wstate->combiner_alphasub_b[0] = wstate->combiner_alphasub_b[1] = &one_color;
    wstate->combiner_alphamul[0] = wstate->combiner_alphamul[1] = &one_color;
    wstate->combiner_alphaadd[0] = wstate->combiner_alphaadd[1] = &one_color;
}

void
rdp_set_prim_color(struct rdp_state *wstate, const uint32_t *args)
{
    wstate->min_level = (args[0] >> 8) & 0x1f;
    wstate->primitive_lod_frac = args[0] & 0xff;
    wstate->prim_color.r = RGBA32_R(args[1]);
    wstate->prim_color.g = RGBA32_G(args[1]);
    wstate->prim_color.b = RGBA32_B(args[1]);
    wstate->prim_color.a = RGBA32_A(args[1]);
}

void
rdp_set_env_color(struct rdp_state *wstate, const uint32_t *args)
{
    wstate->env_color.r = RGBA32_R(args[1]);
    wstate->env_color.g = RGBA32_G(args[1]);
    wstate->env_color.b = RGBA32_B(args[1]);
    wstate->env_color.a = RGBA32_A(args[1]);
}

void
rdp_set_combine(struct rdp_state *wstate, const uint32_t *args)
{
    wstate->combine.sub_a_rgb0 = (args[0] >> 20) & 0xf;
    wstate->combine.mul_rgb0 = (args[0] >> 15) & 0x1f;
    wstate->combine.sub_a_a0 = (args[0] >> 12) & 0x7;
    wstate->combine.mul_a0 = (args[0] >> 9) & 0x7;
    wstate->combine.sub_a_rgb1 = (args[0] >> 5) & 0xf;
    wstate->combine.mul_rgb1 = (args[0] >> 0) & 0x1f;

    wstate->combine.sub_b_rgb0 = (args[1] >> 28) & 0xf;
    wstate->combine.sub_b_rgb1 = (args[1] >> 24) & 0xf;
    wstate->combine.sub_a_a1 = (args[1] >> 21) & 0x7;
    wstate->combine.mul_a1 = (args[1] >> 18) & 0x7;
    wstate->combine.add_rgb0 = (args[1] >> 15) & 0x7;
    wstate->combine.sub_b_a0 = (args[1] >> 12) & 0x7;
    wstate->combine.add_a0 = (args[1] >> 9) & 0x7;
    wstate->combine.add_rgb1 = (args[1] >> 6) & 0x7;
    wstate->combine.sub_b_a1 = (args[1] >> 3) & 0x7;
    wstate->combine.add_a1 = (args[1] >> 0) & 0x7;

    set_suba_rgb_input(wstate, &wstate->combiner_rgbsub_a_r[0], &wstate->combiner_rgbsub_a_g[0],
                       &wstate->combiner_rgbsub_a_b[0], wstate->combine.sub_a_rgb0);
    set_subb_rgb_input(wstate, &wstate->combiner_rgbsub_b_r[0], &wstate->combiner_rgbsub_b_g[0],
                       &wstate->combiner_rgbsub_b_b[0], wstate->combine.sub_b_rgb0);
    set_mul_rgb_input(wstate, &wstate->combiner_rgbmul_r[0], &wstate->combiner_rgbmul_g[0],
                      &wstate->combiner_rgbmul_b[0], wstate->combine.mul_rgb0);
    set_add_rgb_input(wstate, &wstate->combiner_rgbadd_r[0], &wstate->combiner_rgbadd_g[0],
                      &wstate->combiner_rgbadd_b[0], wstate->combine.add_rgb0);
    set_sub_alpha_input(wstate, &wstate->combiner_alphasub_a[0], wstate->combine.sub_a_a0);
    set_sub_alpha_input(wstate, &wstate->combiner_alphasub_b[0], wstate->combine.sub_b_a0);
    set_mul_alpha_input(wstate, &wstate->combiner_alphamul[0], wstate->combine.mul_a0);
    set_sub_alpha_input(wstate, &wstate->combiner_alphaadd[0], wstate->combine.add_a0);

    set_suba_rgb_input(wstate, &wstate->combiner_rgbsub_a_r[1], &wstate->combiner_rgbsub_a_g[1],
                       &wstate->combiner_rgbsub_a_b[1], wstate->combine.sub_a_rgb1);
    set_subb_rgb_input(wstate, &wstate->combiner_rgbsub_b_r[1], &wstate->combiner_rgbsub_b_g[1],
                       &wstate->combiner_rgbsub_b_b[1], wstate->combine.sub_b_rgb1);
    set_mul_rgb_input(wstate, &wstate->combiner_rgbmul_r[1], &wstate->combiner_rgbmul_g[1],
                      &wstate->combiner_rgbmul_b[1], wstate->combine.mul_rgb1);
    set_add_rgb_input(wstate, &wstate->combiner_rgbadd_r[1], &wstate->combiner_rgbadd_g[1],
                      &wstate->combiner_rgbadd_b[1], wstate->combine.add_rgb1);
    set_sub_alpha_input(wstate, &wstate->combiner_alphasub_a[1], wstate->combine.sub_a_a1);
    set_sub_alpha_input(wstate, &wstate->combiner_alphasub_b[1], wstate->combine.sub_b_a1);
    set_mul_alpha_input(wstate, &wstate->combiner_alphamul[1], wstate->combine.mul_a1);
    set_sub_alpha_input(wstate, &wstate->combiner_alphaadd[1], wstate->combine.add_a1);

    wstate->other_modes.f.stalederivs = 1;
}

void
rdp_set_key_gb(struct rdp_state *wstate, const uint32_t *args)
{
    wstate->key_width.g = (args[0] >> 12) & 0xfff;
    wstate->key_width.b = args[0] & 0xfff;
    wstate->key_center.g = (args[1] >> 24) & 0xff;
    wstate->key_scale.g = (args[1] >> 16) & 0xff;
    wstate->key_center.b = (args[1] >> 8) & 0xff;
    wstate->key_scale.b = args[1] & 0xff;
}

void
rdp_set_key_r(struct rdp_state *wstate, const uint32_t *args)
{
    wstate->key_width.r = (args[1] >> 16) & 0xfff;
    wstate->key_center.r = (args[1] >> 8) & 0xff;
    wstate->key_scale.r = args[1] & 0xff;
}

#endif // N64VIDEO_C
