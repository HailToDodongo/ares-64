#ifdef N64VIDEO_C

static STRICTINLINE void
rgb_dither(int rgb_dither_sel, int *r, int *g, int *b, int dith)
{
    int32_t newr = *r, newg = *g, newb = *b;
    newr = (newr > 247) ? 255 : ((newr & 0xf8) + 8);
    newg = (newg > 247) ? 255 : ((newg & 0xf8) + 8);
    newb = (newb > 247) ? 255 : ((newb & 0xf8) + 8);

    int32_t rcomp, gcomp, bcomp;
    if (rgb_dither_sel != 2) {
        // Not noise, each channel gets the same dither value
        rcomp = gcomp = bcomp = dith;
    } else {
        // G_CD_NOISE, each channel gets a different dither value
        rcomp = (dith >> 0) & 7;
        gcomp = (dith >> 3) & 7;
        bcomp = (dith >> 6) & 7;
    }

    int32_t sr, sg, sb;
    sr = (rcomp - (*r & 7)) >> 31;
    sg = (gcomp - (*g & 7)) >> 31;
    sb = (bcomp - (*b & 7)) >> 31;

    *r += ((newr - *r) & sr);
    *g += ((newg - *g) & sg);
    *b += ((newb - *b) & sb);
}

// Same as the above function but for the g channel only
static STRICTINLINE void
rgb_dither_gval(int rgb_dither_sel, int *g, int dith)
{
    int32_t newg = *g;
    newg = (newg > 247) ? 255 : ((newg & 0xf8) + 8);

    int32_t gcomp;
    if (rgb_dither_sel != 2)
        gcomp = dith;
    else
        gcomp = (dith >> 3) & 7;

    int32_t sg = (gcomp - (*g & 7)) >> 31;

    *g += ((newg - *g) & sg);
}

// clang-format off
static const uint8_t bayer_matrix[16] = {
    0, 4, 1, 5,
    4, 0, 5, 1,
    3, 7, 2, 6,
    7, 3, 6, 2,
};
// clang-format on

// clang-format off
static const uint8_t magic_matrix[16] = {
    0, 6, 1, 7,
    4, 2, 5, 3,
    3, 5, 2, 4,
    7, 1, 6, 0,
};
// clang-format on

#define CD_MAGICSQ 0
#define CD_BAYER   1
#define CD_NOISE   2
#define CD_DISABLE 3

#define AD_PATTERN  0
#define AD_NPATTERN 1
#define AD_NOISE    2
#define AD_DISABLE  3

static STRICTINLINE void
get_dither_noise(struct rdp_state *wstate, int x, int y, int *cdith, int *adith)
{
    // Cycle noise if it's used (renderer optimization, hw always cycles noise even when it isn't used)
    if (wstate->other_modes.f.getditherlevel == DITHER_LEVEL_NOISE)
        wstate->noise = ((irand(&wstate->rseed) & 7) << 6) | 0x20;

    // Adjustment for when scissor interlacing is enabled
    y >>= wstate->scfield;

    int dithindex = ((y & 3) << 2) | (x & 3);
    const uint8_t *pattern_mtx = (wstate->other_modes.rgb_dither_sel & 1) ? bayer_matrix : magic_matrix;

    switch (wstate->other_modes.rgb_dither_sel) {
        case_no_default;

        case CD_MAGICSQ:
        case CD_BAYER:
            *cdith = pattern_mtx[dithindex];
            break;
        case CD_NOISE:
            *cdith = irand(&wstate->rseed);
            break;
        case CD_DISABLE:
            *cdith = 7;
            break;
    }

    switch (wstate->other_modes.alpha_dither_sel) {
        case_no_default;

        case AD_PATTERN:
            *adith = pattern_mtx[dithindex];
            break;
        case AD_NPATTERN:
            *adith = (~pattern_mtx[dithindex]) & 7;
            break;
        case AD_NOISE:
            *adith = (wstate->noise >> 6) & 7;
            break;
        case AD_DISABLE:
            *adith = 0;
            break;
    }
}

#endif // N64VIDEO_C
