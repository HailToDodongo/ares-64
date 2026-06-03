#ifdef N64VIDEO_C

static uint16_t z_com_table[0x40000];
static uint32_t z_complete_dec_table[0x4000];
static uint16_t deltaz_comparator_lut[0x10000];

static struct {
    uint32_t shift;
    uint32_t add;
} z_dec_table[8] = {
  // clang-format off
    { 6, 0x00000 },
    { 5, 0x20000 },
    { 4, 0x30000 },
    { 3, 0x38000 },
    { 2, 0x3C000 },
    { 1, 0x3E000 },
    { 0, 0x3F000 },
    { 0, 0x3F800 },
  // clang-format on
};

static STRICTINLINE uint32_t
z_decompress(uint32_t zb)
{
    return z_complete_dec_table[(zb >> 2) & 0x3FFF];
}

static INLINE void
z_build_com_table(void)
{
    for (int z = 0; z < 0x40000; z++) {
        uint16_t altmem = 0;
        uint16_t key = (z >> 11) & 0x7F;

        if (key <= 0x3F) {
            altmem = ((z >> 4) & 0x1FFC) | 0x0000;
        } else if (key <= 0x5F) {
            altmem = ((z >> 3) & 0x1FFC) | 0x2000;
        } else if (key <= 0x6F) {
            altmem = ((z >> 2) & 0x1FFC) | 0x4000;
        } else if (key <= 0x77) {
            altmem = ((z >> 1) & 0x1FFC) | 0x6000;
        } else if (key <= 0x7B) {
            altmem = ((z >> 0) & 0x1FFC) | 0x8000;
        } else if (key <= 0x7D) {
            altmem = ((z << 1) & 0x1FFC) | 0xA000;
        } else if (key == 0x7E) {
            altmem = ((z << 2) & 0x1FFC) | 0xC000;
        } else { // 0x7F
            altmem = ((z << 2) & 0x1FFC) | 0xE000;
        }

        z_com_table[z] = altmem;
    }
}

static STRICTINLINE void
z_store(uint32_t zcurpixel, uint32_t z, int dzpixenc)
{
#ifdef N64VIDEO_METRICS
    fb_heatmap_bump(fb_heatmap_writes, zcurpixel);
#endif
    uint16_t zval = z_com_table[z & 0x3FFFF] | (uint16_t)(dzpixenc >> 2);
    uint8_t hval = dzpixenc & 3;

    rdram_write_pair16(zcurpixel, zval, hval, 0);
}

static STRICTINLINE uint32_t
dz_decompress(uint32_t dz_compressed)
{
    return 1 << dz_compressed;
}

static STRICTINLINE uint32_t
dz_compress(uint32_t value)
{
    // Integer log2, valid only for powers of 2
    uint32_t j = 0;
    if (value & 0xFF00)
        j |= 8;
    if (value & 0xF0F0)
        j |= 4;
    if (value & 0xCCCC)
        j |= 2;
    if (value & 0xAAAA)
        j |= 1;
    return j;
}

#define ZMODE_OPA   0
#define ZMODE_INTER 1
#define ZMODE_XLU   2
#define ZMODE_DEC   3

static STRICTINLINE uint32_t
z_compare(struct rdp_state *wstate, uint32_t zcurpixel, uint32_t sz, uint16_t dzpix, int dzpixenc, uint32_t *blend_en,
          uint32_t *prewrap, uint32_t *curpixel_cvg, uint32_t curpixel_memcvg)
{
    bool overflow = (curpixel_memcvg + *curpixel_cvg) & 8;
    *prewrap = overflow;

    if (wstate->other_modes.z_compare_en) {
        uint8_t hval;
        uint16_t zval;
        PAIRREAD16(zval, hval, zcurpixel);
#ifdef N64VIDEO_METRICS
        fb_heatmap_bump(fb_heatmap_reads, zcurpixel);
#endif

        sz &= 0x3ffff; // u15.3
        uint32_t oz = z_decompress(zval);

        int32_t rawdzmem = ((zval & 3) << 2) | hval; // 2 bits rdram, 2 bits hidden rdram
        uint32_t dzmem = dz_decompress(rawdzmem);

        // determine blender shifter signals
        if (wstate->other_modes.f.realblendershiftersneeded) {
            wstate->blshifta = clamp(dzpixenc - rawdzmem, 0, 4);
            wstate->blshiftb = clamp(rawdzmem - dzpixenc, 0, 4);
        }

        if (wstate->other_modes.f.interpixelblendershiftersneeded) {
            // off-by-1 bug, blender shifters use previous pixel dzmem in first cycle of 2-cycle mode
            wstate->pastblshifta = clamp(dzpixenc - wstate->pastrawdzmem, 0, 4);
            wstate->pastblshiftb = clamp(wstate->pastrawdzmem - dzpixenc, 0, 4);
        }

        wstate->pastrawdzmem = rawdzmem;

        int zval_exponent = (zval >> 13) & 0xf;

        bool force_coplanar = false;
        // if small exponent, modify dzmem ?
        if (zval_exponent < 3) {
            if (dzmem == 0x8000) { // maximum dzmem
                force_coplanar = true;
                dzmem = 0xffff;
            } else {
                dzmem = MAX(dzmem << 1, 16 >> zval_exponent);
            }
        }

        bool max = oz == 0x3ffff;
        bool infront = sz < oz;

        // Finds largest power of two <= (dzpix | dzmem)
        uint32_t dznew = (uint32_t)deltaz_comparator_lut[dzpix | dzmem];
        uint32_t dznotshift = dznew;
        dznew <<= 3;

        uint32_t sum = sz + dznew;
        int32_t diff = (int32_t)sz - (int32_t)dznew;

        // coplanar OR sz - dz is not behind oz
        bool nearer = force_coplanar || (diff <= (int32_t)oz);
        // coplanar OR sz + dz is behind oz
        bool farther = force_coplanar || (sum >= oz);

        *blend_en = wstate->other_modes.force_blend || (!overflow && wstate->other_modes.antialias_en && farther);

        switch (wstate->other_modes.z_mode) {
            case_no_default;

            case ZMODE_INTER:
                // pass the sz < oz test
                // coplanar or sz + dz is behind oz
                // (old_cvg + new_cvg) overflows
                if (infront && farther && overflow) {
                    // Modify pixel coverage for possible antialiasing
                    uint32_t dzenc = dz_compress(dznotshift & 0xffff);
                    int cvgcoeff = ((oz >> dzenc) - (sz >> dzenc)) & 0xf;
                    *curpixel_cvg = ((cvgcoeff * (*curpixel_cvg)) >> 3) & 0xf;
                    return true;
                }
                // Fallthrough to opaque z mode otherwise
                FALLTHROUGH;
            case ZMODE_OPA:
                // z-buffer is empty or
                // overflow:
                //   sz < oz
                // else:
                //   sz <= oz within dz threshold
                return max || (overflow ? infront : nearer);

            case ZMODE_XLU:
                // Transparent surface: new < old or old is max depth
                return (infront || max);

            case ZMODE_DEC:
                // z-buffer is NOT empty AND sz falls within the dz threshold of oz in either direction
                return farther && nearer && !max;
        }
    } else {

        if (wstate->other_modes.f.realblendershiftersneeded) {
            wstate->blshifta = 0;
            if (dzpixenc < 11) // Clamp the shift between [0,4]
                wstate->blshiftb = 4;
            else
                wstate->blshiftb = 15 - dzpixenc;
        }

        if (wstate->other_modes.f.interpixelblendershiftersneeded) {
            // Off-by-1 hardware bug, blender shifters use prev pixel dz in first cycle of 2-cycle mode
            wstate->pastblshifta = 0;
            if (dzpixenc < 11)
                wstate->pastblshiftb = 4;
            else
                wstate->pastblshiftb = 15 - dzpixenc;
        }

        wstate->pastrawdzmem = 15;

        *blend_en = wstate->other_modes.force_blend || (!overflow && wstate->other_modes.antialias_en);

        return 1;
    }
}

void
rdp_set_mask_image(struct rdp_state *wstate, const uint32_t *args)
{
    wstate->zb_address = args[1] & 0x0ffffff;
}

static void
z_init_lut(void)
{
    int i;
    z_build_com_table();

    uint32_t exponent;
    uint32_t mantissa;
    for (i = 0; i < 0x4000; i++) {
        exponent = (i >> 11) & 7;
        mantissa = i & 0x7ff;
        z_complete_dec_table[i] = ((mantissa << z_dec_table[exponent].shift) + z_dec_table[exponent].add) & 0x3ffff;
    }

    deltaz_comparator_lut[0] = 0;
    for (i = 1; i < 0x10000; i++) {
        int k;
        for (k = 15; k >= 0; k--) {
            if (i & (1 << k)) {
                deltaz_comparator_lut[i] = 1 << k;
                break;
            }
        }
    }
}

#endif // N64VIDEO_C
