#ifdef N64VIDEO_C

static struct {
    uint8_t cvg;
    uint8_t cvbit;
    uint8_t xoff;
    uint8_t yoff;
} cvarray[0x100];

static STRICTINLINE uint32_t
rightcvghex(uint32_t x, uint32_t fmask)
{
    // Mod 8, divided by 2 (rounded to nearest)
    uint32_t covered = ((x & 7) + 1) >> 1;
    return ((0xF0 >> covered) & fmask);
}

static STRICTINLINE uint32_t
leftcvghex(uint32_t x, uint32_t fmask)
{
    // Mod 8, divided by 2 (rounded to nearest)
    uint32_t covered = ((x & 7) + 1) >> 1;
    return ((0x0F >> covered) & fmask);
}

// TODO there's not a lot of differences between the flip and noflip cases.
//   purgestart and purgeend are swapped
//        flip: (start, end) = (rx, lx)
//      noflip: (start, end) = (lx, rx)
//   minorx and majorx are swapped
//        flip: (min, max) = (minorx, majorx)
//      noflip: (min, max) = (majorx, minorx)

static STRICTINLINE void
compute_cvg_flip(struct rdp_state *wstate, int32_t scanline)
{
    int32_t purgestart = wstate->span[scanline].rx;
    int32_t purgeend = wstate->span[scanline].lx;

    int length = purgeend - purgestart;
    if (length < 0)
        return; // TODO under what conditions does this occur?

    // Start with full coverage
    memset(&wstate->cvgbuf[purgestart], 0xff, length + 1);

    for (int y_subpx = 0; y_subpx < 4; y_subpx++) {
        int fmask = 0b1010 >> (y_subpx & 1);   // { 0b1010, 0b0101, 0b1010, 0b0101 }
        int maskshift = (y_subpx - 2) & 4;     // {      4,      4,      0,      0 }
        int fmaskshifted = fmask << maskshift; // {   0xA0,   0x50,   0x0A,   0x05 }
        // ~fmaskshifted                          {   0x5F,   0xAF,   0xF5,   0xFA }

        if (!wstate->span[scanline].invalyscan[y_subpx]) {
            // Not invalid
            int32_t minorcur = wstate->span[scanline].minorx[y_subpx];
            int32_t majorcur = wstate->span[scanline].majorx[y_subpx];
            int32_t minorcurint = minorcur >> 3;
            int32_t majorcurint = majorcur >> 3;

            // Mask coverage in the range [purgestart, majorx_int]
            for (int i = purgestart; i <= majorcurint; i++)
                wstate->cvgbuf[i] &= ~fmaskshifted;
            // Mask coverage in the range [minorx, purgeend]
            for (int i = minorcurint; i <= purgeend; i++)
                wstate->cvgbuf[i] &= ~fmaskshifted;

            if (minorcurint > majorcurint) {
                wstate->cvgbuf[minorcurint] |= rightcvghex(minorcur, fmask) << maskshift;
                wstate->cvgbuf[majorcurint] |= leftcvghex(majorcur, fmask) << maskshift;
            } else if (minorcurint == majorcurint) {
                // Take the union of the coverage
                int32_t samecvg = rightcvghex(minorcur, fmask) & leftcvghex(majorcur, fmask);
                wstate->cvgbuf[majorcurint] |= samecvg << maskshift;
            } else {
                // This had better be unreachable, since minorx should be >= majorx
                UNREACHABLE();
            }
        } else {
            // Invalid, mask coverage in the range [purgestart, purgeend]
            for (int i = purgestart; i <= purgeend; i++)
                wstate->cvgbuf[i] &= ~fmaskshifted;
        }
    }
}

static STRICTINLINE void
compute_cvg_noflip(struct rdp_state *wstate, int32_t scanline)
{
    int32_t purgestart = wstate->span[scanline].lx;
    int32_t purgeend = wstate->span[scanline].rx;

    int length = purgeend - purgestart;
    if (length < 0)
        return; // TODO under what conditions does this occur?

    // Start with full coverage
    memset(&wstate->cvgbuf[purgestart], 0xff, length + 1);

    for (int y_subpx = 0; y_subpx < 4; y_subpx++) {
        int fmask = 0b1010 >> (y_subpx & 1);   // { 0b1010, 0b0101, 0b1010, 0b0101 }
        int maskshift = (y_subpx - 2) & 4;     // {      4,      4,      0,      0 }
        int fmaskshifted = fmask << maskshift; // {   0xA0,   0x50,   0x0A,   0x05 }
        //     ~fmaskshifted                      {   0x5F,   0xAF,   0xF5,   0xFA }

        if (!wstate->span[scanline].invalyscan[y_subpx]) {
            // Not invalid
            int32_t minorcur = wstate->span[scanline].minorx[y_subpx];
            int32_t majorcur = wstate->span[scanline].majorx[y_subpx];
            int32_t minorcurint = minorcur >> 3;
            int32_t majorcurint = majorcur >> 3;

            // Mask coverage in the range [purgestart, majorx_int]
            for (int k = purgestart; k <= minorcurint; k++)
                wstate->cvgbuf[k] &= ~fmaskshifted;
            // Mask coverage in the range [minorx, purgeend]
            for (int k = majorcurint; k <= purgeend; k++)
                wstate->cvgbuf[k] &= ~fmaskshifted;

            if (majorcurint > minorcurint) {
                wstate->cvgbuf[minorcurint] |= leftcvghex(minorcur, fmask) << maskshift;
                wstate->cvgbuf[majorcurint] |= rightcvghex(majorcur, fmask) << maskshift;
            } else if (minorcurint == majorcurint) {
                // Take the union of the coverage
                int32_t samecvg = leftcvghex(minorcur, fmask) & rightcvghex(majorcur, fmask);
                wstate->cvgbuf[majorcurint] |= samecvg << maskshift;
            } else {
                // This had better be unreachable, since minorx should be >= majorx
                UNREACHABLE();
            }
        } else {
            // Invalid, mask coverage in the range [purgestart, purgeend]
            for (int k = purgestart; k <= purgeend; k++)
                wstate->cvgbuf[k] &= ~fmaskshifted;
        }
    }
}

#define CVG_CLAMP 0
#define CVG_WRAP  1
#define CVG_ZAP   2
#define CVG_SAVE  3

static STRICTINLINE int
finalize_spanalpha(int cvg_dest, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg)
{
    int finalcvg;

    switch (cvg_dest) {
        case_no_default;

        case CVG_CLAMP:
            // If blending is enabled, add memory cvg first
            if (blend_en)
                finalcvg = curpixel_cvg + curpixel_memcvg;
            else
                finalcvg = curpixel_cvg - 1;

            // Perform clamp
            if (finalcvg & 8)
                finalcvg = 7;
            else
                finalcvg &= 7;
            break;

        case CVG_WRAP:
            // Wrap, keep only low order bits
            finalcvg = (curpixel_cvg + curpixel_memcvg) & 7;
            break;

        case CVG_ZAP:
            // Force full coverage
            finalcvg = 7;
            break;

        case CVG_SAVE:
            // Leave coverage unchanged
            finalcvg = curpixel_memcvg;
            break;
    }

    return finalcvg;
}

static STRICTINLINE void
lookup_cvmask_derivatives(uint8_t mask, uint8_t *offx, uint8_t *offy, uint32_t *curpixel_cvg, uint32_t *curpixel_cvbit)
{
    *curpixel_cvg = cvarray[mask].cvg;
    *curpixel_cvbit = cvarray[mask].cvbit;
    *offx = cvarray[mask].xoff;
    *offy = cvarray[mask].yoff;
}

static void
coverage_init_lut(void)
{
    static const uint8_t xarray[16] = { 0, 3, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
    static const uint8_t yarray[16] = { 0, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0 };

    uint16_t mask = 0, maskx = 0, masky = 0;
    uint8_t offx = 0, offy = 0;

    for (unsigned i = 0; i < 256; i++) {
        uint8_t c = i;
        mask = (c & 0b00000101) | ((c & 0b01011010) << 4) | ((c & 0b10100000) << 8);

        // Fast lookup of msbit for non-AA pixel rejection
        cvarray[i].cvbit = (c >> 7) & 1;

        // Population count gives the coverage value
        // (note that c has the same amount of bits set as mask does)
        cvarray[i].cvg = 0;
        for (unsigned j = 0; j < 8; j++)
            cvarray[i].cvg += ((c >> j) & 1);

        // Prepare subpixel offsets for attribute correction

        masky = 0;
        for (unsigned j = 0; j < 4; j++)
            masky |= ((mask & (0xF000 >> (j << 2))) > 0) << j;

        offy = yarray[masky];
        maskx = (mask & (0xF000 >> (offy << 2))) >> ((offy ^ 3) << 2);
        offx = xarray[maskx];

        cvarray[i].xoff = offx; // Result in the range [0,3]
        cvarray[i].yoff = offy; // Result in the range [0,3]
    }
}

#endif // N64VIDEO_C
