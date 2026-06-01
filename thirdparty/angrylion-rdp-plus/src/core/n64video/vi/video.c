#ifdef N64VIDEO_C

static STRICTINLINE void
video_max_optimized(uint32_t *pixels, uint32_t *penumin, uint32_t *penumax, int numofels)
{
    uint32_t curpenmin = pixels[0];
    uint32_t curpenmax = pixels[0];

    int posmax = 0, posmin = 0;
    for (int i = 1; i < numofels; i++) {
        if (pixels[i] > pixels[posmax]) {
            curpenmax = pixels[posmax];
            posmax = i;
        } else if (pixels[i] < pixels[posmin]) {
            curpenmin = pixels[posmin];
            posmin = i;
        }
    }

    uint32_t max = pixels[posmax];
    uint32_t min = pixels[posmin];

    if (curpenmax != max) {
        for (int i = posmax + 1; i < numofels; i++) {
            if (pixels[i] > curpenmax)
                curpenmax = pixels[i];
        }
    }

    if (curpenmin != min) {
        for (int i = posmin + 1; i < numofels; i++) {
            if (pixels[i] < curpenmin)
                curpenmin = pixels[i];
        }
    }

    *penumax = curpenmax;
    *penumin = curpenmin;
}

// https://patents.google.com/patent/EP0767444B1/en
static STRICTINLINE void
video_filter16(int *endr, int *endg, int *endb, uint32_t fboffset, uint32_t num, uint32_t hres, uint32_t centercvg,
               uint32_t fetchbugstate)
{
    // Index of current pixel in the framebuffer
    uint32_t idx = (fboffset >> 1) + num;

    // Pixel to the left, with 1 space
    // . . . . .
    // @ . x . .
    // . . . . .
    uint32_t toleft = idx - 2;

    // Pixel to the right, with 1 space
    // . . . . .
    // . . x . @
    // . . . . .
    uint32_t toright = idx + 2;

    // Upper-left sample
    // . @ . . .
    // . . x . .
    // . . . . .
    uint32_t leftup = idx - hres - 1;

    // Upper-right sample
    // . . . @ .
    // . . x . .
    // . . . . .
    uint32_t rightup = idx - hres + 1;

    uint32_t leftdown, rightdown;

    if (fetchbugstate != 1) {
        // No fetch bug: Sample next line
        // . @ . . .
        // . . x . .
        // . . . . .
        leftdown = idx + hres - 1;
        // . @ . . .
        // . . x . .
        // . . . . .
        rightdown = idx + hres + 1;
    } else {
        // Fetch bug: use middle line samples again (but why would it not use the immediate neighbors?)
        // . . . . .
        // @ . x . .
        // . . . . .
        leftdown = toleft;
        // . . . . .
        // . . x . @
        // . . . . .
        rightdown = toright;
    }

    // Sample points in the 5x3 neighborhood:
    //    _ _ _ _ _
    //  |   o   o   |
    //  | o   X   o |
    //  |   o   o   |
    //    ‾ ‾ ‾ ‾ ‾
    const uint32_t dirs[] = {
        // clang-format off
            leftup,     rightup,
        toleft,             toright,
            leftdown,   rightdown,
        // clang-format on
    };

    // Read center pixel value (Foreground color)
    uint32_t r = *endr;
    uint32_t g = *endg;
    uint32_t b = *endb;

    // Initialize the Background color array first item to the Foreground color
    uint32_t backr[7], backg[7], backb[7];
    backr[0] = r;
    backg[0] = g;
    backb[0] = b;

    // Collect samples, update the background color array when they have full coverage, counting how many
    uint32_t numoffull = 1;
    for (int i = 0; i < 6; i++) {
        uint16_t pix;
        uint8_t hidval;
        rdram_read_pair16(&pix, &hidval, dirs[i]);
        if (hidval == 3 && (pix & 1)) {
            backr[numoffull] = RGBA16_R(pix);
            backg[numoffull] = RGBA16_G(pix);
            backb[numoffull] = RGBA16_B(pix);
            numoffull++;
        }
    }

    // Compute penultimate minimum and maximum
    uint32_t penumaxr, penumaxg, penumaxb;
    uint32_t penuminr, penuming, penuminb;
    video_max_optimized(backr, &penuminr, &penumaxr, numoffull);
    video_max_optimized(backg, &penuming, &penumaxg, numoffull);
    video_max_optimized(backb, &penuminb, &penumaxb, numoffull);

    // The background color is the average of the penultimate minimum and maximum, subtract the Foreground
    uint32_t colr = penuminr + penumaxr - (r << 1);
    uint32_t colg = penuming + penumaxg - (g << 1);
    uint32_t colb = penuminb + penumaxb - (b << 1);

    // LERP ForeGround and BackGround based on coverage
    uint32_t coeff = 7 - centercvg;
    colr = (((colr * coeff) + 4) >> 3) + r;
    colg = (((colg * coeff) + 4) >> 3) + g;
    colb = (((colb * coeff) + 4) >> 3) + b;

    // Output anti-aliased pixel
    *endr = colr & 0xff;
    *endg = colg & 0xff;
    *endb = colb & 0xff;
}

static STRICTINLINE void
video_filter32(int *endr, int *endg, int *endb, uint32_t fboffset, uint32_t num, uint32_t hres, uint32_t centercvg,
               uint32_t fetchbugstate)
{
    // Index of current pixel in the framebuffer
    uint32_t idx = (fboffset >> 2) + num;

    // Pixel to the left, with 1 space
    // . . . . .
    // @ . x . .
    // . . . . .
    uint32_t toleft = idx - 2;

    // Pixel to the right, with 1 space
    // . . . . .
    // . . x . @
    // . . . . .
    uint32_t toright = idx + 2;

    // Upper-left sample
    // . @ . . .
    // . . x . .
    // . . . . .
    uint32_t leftup = idx - hres - 1;

    // Upper-right sample
    // . . . @ .
    // . . x . .
    // . . . . .
    uint32_t rightup = idx - hres + 1;

    uint32_t leftdown, rightdown;
    if (fetchbugstate != 1) {
        // No fetch bug: Sample next line
        // . @ . . .
        // . . x . .
        // . . . . .
        leftdown = idx + hres - 1;
        // . @ . . .
        // . . x . .
        // . . . . .
        rightdown = idx + hres + 1;
    } else {
        // Fetch bug: use middle line samples again (but why would it not use the immediate neighbors?)
        // . . . . .
        // @ . x . .
        // . . . . .
        leftdown = toleft;
        // . . . . .
        // . . x . @
        // . . . . .
        rightdown = toright;
    }

    // Sample points in the 5x3 neighborhood:
    //    _ _ _ _ _
    //  |   o   o   |
    //  | o   X   o |
    //  |   o   o   |
    //    ‾ ‾ ‾ ‾ ‾
    const uint32_t dirs[] = {
        // clang-format off
            leftup,     rightup,
        toleft,             toright,
            leftdown,   rightdown
        // clang-format on
    };

    // Read center pixel value (Foreground color)
    uint32_t r = *endr;
    uint32_t g = *endg;
    uint32_t b = *endb;

    // Initialize the Background color array first item to the Foreground color
    uint32_t backr[7], backg[7], backb[7];
    backr[0] = r;
    backg[0] = g;
    backb[0] = b;

    // Collect samples, update the background color array when they have full coverage, counting how many
    uint32_t numoffull = 1;
    for (int i = 0; i < 6; i++) {
        uint32_t pix = rdram_read_idx32(dirs[i]);
        uint32_t pixcvg = (pix >> 5) & 7;
        if (pixcvg == 7) { // check for full coverage
            backr[numoffull] = (pix >> 24) & 0xff;
            backg[numoffull] = (pix >> 16) & 0xff;
            backb[numoffull] = (pix >> 8) & 0xff;
            numoffull++;
        }
    }

    // Compute penultimate minimum and maximum
    uint32_t penumaxr, penumaxg, penumaxb;
    uint32_t penuminr, penuming, penuminb;
    video_max_optimized(backr, &penuminr, &penumaxr, numoffull);
    video_max_optimized(backg, &penuming, &penumaxg, numoffull);
    video_max_optimized(backb, &penuminb, &penumaxb, numoffull);

    // The background color is the average of the penultimate minimum and maximum, subtract the Foreground
    uint32_t colr = penuminr + penumaxr - (r << 1);
    uint32_t colg = penuming + penumaxg - (g << 1);
    uint32_t colb = penuminb + penumaxb - (b << 1);

    // LERP ForeGround and BackGround based on coverage
    uint32_t coeff = 7 - centercvg;
    colr = (((colr * coeff) + 4) >> 3) + r;
    colg = (((colg * coeff) + 4) >> 3) + g;
    colb = (((colb * coeff) + 4) >> 3) + b;

    // Output anti-aliased pixel
    *endr = colr & 0xff;
    *endg = colg & 0xff;
    *endb = colb & 0xff;
}

#endif // N64VIDEO_C
