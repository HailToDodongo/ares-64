#ifdef N64VIDEO_C

static uint8_t gamma_table[0x100];
static uint8_t gamma_dither_table[0x4000];

static uint8_t
vi_integer_sqrt(uint32_t a)
{
    unsigned long op = a, res = 0, one = 1 << 30;

    while (one > op)
        one >>= 2;

    while (one != 0) {
        if (op >= res + one) {
            op -= res + one;
            res += one << 1;
        }
        res >>= 1;
        one >>= 2;
    }
    return res;
}

static STRICTINLINE void
gamma_filters(struct n64video_pixel *pixel, bool gamma_enable, bool gamma_dither_enable, uint32_t *rstate)
{
    unsigned cdith;
    unsigned dr, dg, db;

    switch ((gamma_enable << 1) | gamma_dither_enable) {
        case 0: // no gamma, no dithering
            break;

        case 1: // no gamma, dithering enabled (contrary to the programming manual, this actually does something)
            cdith = irand(rstate);
            // pixel += (Vec3(RAND) >> [0, 1, 2]) & 1       (with clamp)
            if (pixel->r < 255)
                pixel->r += (cdith >> 0) & 1;
            if (pixel->g < 255)
                pixel->g += (cdith >> 1) & 1;
            if (pixel->b < 255)
                pixel->b += (cdith >> 2) & 1;
            break;

        case 2: // gamma enabled, no dithering (square root)
            pixel->r = gamma_table[pixel->r];
            pixel->g = gamma_table[pixel->g];
            pixel->b = gamma_table[pixel->b];
            break;

        case 3:                    // gamma and dithering enabled (first dither then square root)
            cdith = irand(rstate); // only 15 bits of randomness? 3 lsbits of db are correlated with 3 lsbits of dr
            dr = (cdith >> 0 & 0x3f);
            dg = (cdith >> 6 & 0x3f);
            db = (cdith >> 9 & 0x38) | (cdith & 7);
            pixel->r = gamma_dither_table[((pixel->r) << 6) | dr];
            pixel->g = gamma_dither_table[((pixel->g) << 6) | dg];
            pixel->b = gamma_dither_table[((pixel->b) << 6) | db];
            break;
    }
}

static void
vi_gamma_init(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        gamma_table[i] = vi_integer_sqrt(i << 6);
        gamma_table[i] <<= 1;
    }

    for (i = 0; i < 0x4000; i++) {
        gamma_dither_table[i] = vi_integer_sqrt(i);
        gamma_dither_table[i] <<= 1;
    }
}

#endif // N64VIDEO_C
