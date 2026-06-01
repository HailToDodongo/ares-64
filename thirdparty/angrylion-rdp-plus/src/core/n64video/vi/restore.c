#ifdef N64VIDEO_C

// https://patents.google.com/patent/US5699079A/en

static int vi_restore_table[(1 << 5) * (1 << 5)];

static STRICTINLINE void
restore_filter16(int *r, int *g, int *b, uint32_t fboffset, uint32_t num, uint32_t hres, uint32_t fetchbugstate)
{
    // position of center pixel
    uint32_t idx = (fboffset >> 1) + num;

    // pixel immediately left
    // . . .
    // @ x .
    // . . .
    uint32_t toleftpix = idx - 1;

    // upper-left neighbor
    // @ . .
    // . x .
    // . . .
    uint32_t leftuppix = idx - hres - 1;

    uint32_t leftdownpix;
    if (fetchbugstate != 1) {
        // not fetch bug: sample next line
        // lower-left neighbor
        // . . .
        // . x .
        // @ . .
        leftdownpix = idx + hres - 1;
    } else {
        // fetch bug: sample current line again
        // . . .
        // @ x .
        // . . .
        leftdownpix = idx - 1;
    }

    // 8 pixels surrounding this pixel
    const uint32_t dirs[] = {
        // clang-format off
        leftuppix,   leftuppix   + 1, leftuppix   + 2,
        toleftpix,                    toleftpix   + 2,
        leftdownpix, leftdownpix + 1, leftdownpix + 2,
        // clang-format on
    };

    // Read value for the center pixel
    int rend = *r;
    int gend = *g;
    int bend = *b;
    const int *redptr = &vi_restore_table[(rend << 2) & 0x3e0];
    const int *greenptr = &vi_restore_table[(gend << 2) & 0x3e0];
    const int *blueptr = &vi_restore_table[(bend << 2) & 0x3e0];

    // Correct it according to whether the surrounding pixels are greater, smaller or equal in each channel

    // Note there was a renderer optimization here, if all the pixels are in bounds of RDRAM use
    // the "fast" rdram read function that doesn't do the range check for all pixels. On hardware
    // it should request the RDRAM read anyway and the RDRAM will return 0 if out of range.
    if (rdram_valid_idx16(leftdownpix + 2) && rdram_valid_idx16(leftuppix)) {
        for (int i = 0; i < 8; i++) {
            uint16_t pix = rdram_read_idx16_fast(dirs[i]);
            uint32_t tempr = (pix >> 11) & 0x1f;
            uint32_t tempg = (pix >> 6) & 0x1f;
            uint32_t tempb = (pix >> 1) & 0x1f;
            rend += redptr[tempr];
            gend += greenptr[tempg];
            bend += blueptr[tempb];
        }
    } else {
        for (int i = 0; i < 8; i++) {
            uint16_t pix = rdram_read_idx16(dirs[i]);
            uint32_t tempr = (pix >> 11) & 0x1f;
            uint32_t tempg = (pix >> 6) & 0x1f;
            uint32_t tempb = (pix >> 1) & 0x1f;
            rend += redptr[tempr];
            gend += greenptr[tempg];
            bend += blueptr[tempb];
        }
    }

    *r = rend;
    *g = gend;
    *b = bend;
}

static STRICTINLINE void
restore_filter32(int *r, int *g, int *b, uint32_t fboffset, uint32_t num, uint32_t hres, uint32_t fetchbugstate)
{
    // position of center pixel
    uint32_t idx = (fboffset >> 2) + num;

    // pixel immediately left
    // . . .
    // @ x .
    // . . .
    uint32_t toleftpix = idx - 1;

    // upper-left neighbor
    // @ . .
    // . x .
    // . . .
    uint32_t leftuppix = idx - hres - 1;

    uint32_t leftdownpix;
    if (fetchbugstate != 1) {
        // not fetch bug: sample next line
        // lower-left neighbor
        // . . .
        // . x .
        // @ . .
        leftdownpix = idx + hres - 1;
    } else {
        // fetch bug: sample current line again
        // . . .
        // @ x .
        // . . .
        leftdownpix = idx - 1;
    }

    // 8 pixels surrounding this pixel
    const uint32_t dirs[] = {
        // clang-format off
        leftuppix,   leftuppix   + 1, leftuppix   + 2,
        toleftpix,                    toleftpix   + 2,
        leftdownpix, leftdownpix + 1, leftdownpix + 2,
        // clang-format on
    };

    // Read value for the center pixel
    int rend = *r;
    int gend = *g;
    int bend = *b;
    const int *redptr = &vi_restore_table[(rend << 2) & 0x3e0];
    const int *greenptr = &vi_restore_table[(gend << 2) & 0x3e0];
    const int *blueptr = &vi_restore_table[(bend << 2) & 0x3e0];

    // Correct it according to whether the surrounding pixels are greater, smaller or equal in each channel
    if (rdram_valid_idx32(leftdownpix + 2) && rdram_valid_idx32(leftuppix)) {
        for (int i = 0; i < 8; i++) {
            uint32_t pix = rdram_read_idx32_fast(dirs[i]);
            uint32_t tempr = (pix >> 27) & 0x1f;
            uint32_t tempg = (pix >> 19) & 0x1f;
            uint32_t tempb = (pix >> 11) & 0x1f;
            rend += redptr[tempr];
            gend += greenptr[tempg];
            bend += blueptr[tempb];
        }
    } else {
        for (int i = 0; i < 8; i++) {
            uint32_t pix = rdram_read_idx32(dirs[i]);
            uint32_t tempr = (pix >> 27) & 0x1f;
            uint32_t tempg = (pix >> 19) & 0x1f;
            uint32_t tempb = (pix >> 11) & 0x1f;
            rend += redptr[tempr];
            gend += greenptr[tempg];
            bend += blueptr[tempb];
        }
    }

    *r = rend;
    *g = gend;
    *b = bend;
}

static void
vi_restore_init(void)
{
    // Lookup table that encodes the results of comparing two 5-bit numbers
    for (int i = 0; i < (1 << 5) * (1 << 5); i++) {
        if (((i >> 5) & 0x1f) < (i & 0x1f))
            vi_restore_table[i] = 1;
        else if (((i >> 5) & 0x1f) > (i & 0x1f))
            vi_restore_table[i] = -1;
        else
            vi_restore_table[i] = 0;
    }
}

#endif // N64VIDEO_C
