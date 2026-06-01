#ifdef N64VIDEO_C

#define tmem16 ((uint16_t *)wstate->tmem)
#define tc16   ((uint16_t *)wstate->tmem)
#define tlut   ((uint16_t *)(&wstate->tmem[0x800]))

static uint8_t replicated_rgba[32];

#define RGBA16_EXTEND_R(x) (replicated_rgba[((x) >> 11)])
#define RGBA16_EXTEND_G(x) (replicated_rgba[((x) >> 6) & 0x1f])
#define RGBA16_EXTEND_B(x) (replicated_rgba[((x) >> 1) & 0x1f])

static void
sort_tmem_idx(uint32_t *idx, uint32_t idxa, uint32_t idxb, uint32_t idxc, uint32_t idxd, uint32_t bankno)
{
    if ((idxa & 3) == bankno)
        *idx = idxa & 0x3ff;
    else if ((idxb & 3) == bankno)
        *idx = idxb & 0x3ff;
    else if ((idxc & 3) == bankno)
        *idx = idxc & 0x3ff;
    else if ((idxd & 3) == bankno)
        *idx = idxd & 0x3ff;
    else // TODO should be unreachable?
        *idx = 0;
}

static void
sort_tmem_shorts_lowhalf(uint32_t *bindshort, uint32_t short0, uint32_t short1, uint32_t short2, uint32_t short3,
                         uint32_t bankno)
{
    switch (bankno) {
        case 0:
            *bindshort = short0;
            break;
        case 1:
            *bindshort = short1;
            break;
        case 2:
            *bindshort = short2;
            break;
        case 3:
            *bindshort = short3;
            break;
    }
}

static void
compute_color_index(struct rdp_state *wstate, uint32_t *cidx, uint32_t readshort, uint32_t nybbleoffset,
                    uint32_t tilenum)
{
    uint32_t lownib, hinib;
    if (wstate->tile[tilenum].size == PIXEL_SIZE_4BIT) {
        lownib = (nybbleoffset ^ 3) << 2;
        hinib = wstate->tile[tilenum].palette;
    } else {
        lownib = ((nybbleoffset & 2) ^ 2) << 2;
        hinib = lownib ? ((readshort >> 12) & 0xf) : ((readshort >> 4) & 0xf);
    }
    lownib = (readshort >> lownib) & 0xf;
    *cidx = (hinib << 4) | lownib;
}

static INLINE void
fetch_texel(struct rdp_state *wstate, struct color *color, int s, int t, uint32_t tilenum)
{
    // Determine address
    int tsize = wstate->tile[tilenum].size;
    int tformat = wstate->tile[tilenum].format;

    // Compute tmem base address for the line
    uint32_t tbase = wstate->tile[tilenum].line * (t & 0xff) + wstate->tile[tilenum].tmem;

    // Compute tmem address for the exact sample
    uint32_t taddr;
    if (tformat == FORMAT_YUV || tsize == PIXEL_SIZE_8BIT)
        // yuv access is done using both 16-bit (u,v pair) and 8-bit (y) so it's treated like 8-bit here and a 16-bit
        // variant is below
        taddr = (tbase << 3) + s;
    else if (tsize == PIXEL_SIZE_4BIT)
        // lshift by 4 to get 4-bit granularity for s coordinate, then rshift by 1 to resolve the byte address
        taddr = ((tbase << 4) + s) >> 1;
    else /* (PIXEL_SIZE_16BIT || PIXEL_SIZE_32BIT) && !FORMAT_YUV */
        // uses tc16, so << 3 becomes >> 2
        taddr = (tbase << 2) + s;

    // for YUV only, 16-bit samples for (u,v)
    uint32_t taddrlow;
    taddrlow = taddr >> 1;

    // XORs for endianness

    uint32_t taddr_xor_b = ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);
    uint32_t taddr_xor_w = taddr_xor_b >> 1;

    if (tformat == FORMAT_YUV || tsize == PIXEL_SIZE_4BIT || tsize == PIXEL_SIZE_8BIT)
        taddr ^= taddr_xor_b; // 8-bit samples
    else                      /* PIXEL_SIZE_16BIT || PIXEL_SIZE_32BIT */
        taddr ^= taddr_xor_w; // 16-bit samples

    taddrlow ^= taddr_xor_w;

    // TMEM access, up to 2x 16 bits

    uint16_t c1, c2;

    switch (wstate->tile[tilenum].f.notlutswitch) {
        case_no_default;

            /* 4-bit */

        case TEXEL_CI4:
        case TEXEL_RGBA4:
        case TEXEL_I4:
        case TEXEL_IA4:
            c1 = wstate->tmem[taddr & 0xfff];
            c1 = (s & 1) ? (c1 & 0xf) : (c1 >> 4);
            break;

            /* 8-bit */

        case TEXEL_I8:
        case TEXEL_CI8:
        case TEXEL_RGBA8:
        case TEXEL_IA8:
            c1 = wstate->tmem[taddr & 0xfff];
            break;

            /* 16-bit and 32-bit */

        case TEXEL_RGBA32:
            c1 = tc16[(0x000 >> 1) | (taddr & 0x3ff)];
            c2 = tc16[(0x800 >> 1) | (taddr & 0x3ff)];
            break;

        case TEXEL_IA16:
        case TEXEL_RGBA16:
        case TEXEL_CI16:
        case TEXEL_CI32:
        case TEXEL_I16:
        case TEXEL_I32:
        case TEXEL_IA32:
            c1 = c2 = tc16[taddr & 0x7ff];
            break;

            /* YUV formats */

        case TEXEL_YUV4:
        case TEXEL_YUV8:
            c1 = wstate->tmem[taddr & 0x7ff];
            break;
        case TEXEL_YUV16:
            c1 = tc16[taddrlow & 0x3ff];                // u,v
            c2 = wstate->tmem[(taddr & 0x7ff) | 0x800]; // y
            break;
        case TEXEL_YUV32:
            c1 = tc16[taddrlow & 0x3ff];                                                                    // u,v
            c2 = (s & 1) ? wstate->tmem[(taddr & 0x7ff) | 0x800] : tc16[(taddrlow & 0x3ff) | (0x800 >> 1)]; // yy
            break;
    }

    // Formatting

    switch (wstate->tile[tilenum].f.notlutswitch) {
        case_no_default;

            /* 4-bit formats (excluding YUV) */

        case TEXEL_RGBA4:
        case TEXEL_I4:
            color->r = color->g = color->b = color->a = (c1 << 4) | c1;
            break;

        case TEXEL_CI4:
            // Like I4 but rather than replicating pixels it stuffs the palette number into the upper 4 bits
            color->r = color->g = color->b = color->a = (wstate->tile[tilenum].palette << 4) | c1;
            break;

        case TEXEL_IA4:
            {
                uint8_t i = c1 & 0b1110;
                uint8_t a = c1 & 1;
                color->r = color->g = color->b = (i << 4) | (i << 1) | (i >> 2);
                color->a = a * 255;
            }
            break;

            /* 8-bit formats (excluding YUV) */

        case TEXEL_I8:
        case TEXEL_CI8:
        case TEXEL_RGBA8:
            color->r = color->g = color->b = color->a = c1;
            break;

        case TEXEL_IA8:
            {
                uint8_t i = c1 & 0xf0;
                uint8_t a = c1 & 0x0f;
                color->r = color->g = color->b = i | (i >> 4);
                color->a = (a << 4) | a;
            }
            break;

            /* 16 and 32 bit formats (excluding YUV) */

        case TEXEL_RGBA16:
            color->r = RGBA16_EXTEND_R(c1);
            color->g = RGBA16_EXTEND_G(c1);
            color->b = RGBA16_EXTEND_B(c1);
            color->a = (c1 & 1) * 255;
            break;

        case TEXEL_IA16:
            color->r = color->g = color->b = c1 >> 8;
            color->a = c1 & 0xff;
            break;

        case TEXEL_CI16:
        case TEXEL_CI32:
        case TEXEL_I16:
        case TEXEL_I32:
        case TEXEL_IA32:
        case TEXEL_RGBA32:
            // For all but rgba32, c1 = c2 = low tmem sample
            // For rgba32, c1 is low tmem and c2 is high tmem
            color->r = c1 >> 8;
            color->g = c1 & 0xff;
            color->b = c2 >> 8;
            color->a = c2 & 0xff;
            break;

            /* YUV formats */

        case TEXEL_YUV4:
            // expand to 8-bit value (taking upper nibble) then just 8-bit YUV behavior
            c1 &= 0xf0; // u,v
            c1 = c1 | (c1 >> 4);
            FALLTHROUGH;
        case TEXEL_YUV8:
            color->r = color->g = c1 - 0x80; // u,v
            color->b = color->a = c1;        // u,v
            break;

        case TEXEL_YUV16:
        case TEXEL_YUV32:
            {
                int32_t y = c2;        // yy or y
                int32_t u = c1 >> 8;   // u
                int32_t v = c1 & 0xff; // v

                color->r = u - 0x80;
                color->g = v - 0x80;

                if (tsize == PIXEL_SIZE_16BIT || (s & 1)) {
                    // lower 8 bits of yy, for the 16-bit format the upper 8 bits are meaningless anyway
                    color->b = color->a = y;
                } else {
                    color->b = y >> 8; // upper 8 bits of yy
                    // some insane stuff here
                    // _F__ -> ___F     lower nibble of upper 8 bits becomes lower nibble
                    // __F_ -> __F_     upper nibble of lower 8 bits becomes upper nibble
                    color->a = ((y >> 8) & 0xf) | (y & 0xf0);
                }
            }
            break;
    }
}

static INLINE void
fetch_texel_quadro(struct rdp_state *wstate, struct color *color0, struct color *color1, struct color *color2,
                   struct color *color3, int s0, int sdiff, int t0, int tdiff, uint32_t tilenum, int unequaluppers)
{
    uint32_t tbase0 = wstate->tile[tilenum].line * (t0 & 0xff) + wstate->tile[tilenum].tmem;
    int t1 = (t0 & 0xff) + tdiff;
    int s1 = s0 + sdiff;
    uint32_t tbase2 = wstate->tile[tilenum].line * t1 + wstate->tile[tilenum].tmem;

    int tsize = wstate->tile[tilenum].size;
    int tformat = wstate->tile[tilenum].format;

    struct color *colors[] = {
        color0,
        color1,
        color2,
        color3,
    };
    uint32_t taddrs[4];

    if (tformat == FORMAT_YUV && (tsize == PIXEL_SIZE_4BIT || tsize == PIXEL_SIZE_8BIT)) {
        taddrs[0] = (tbase0 << 3) + s0;
        taddrs[1] = (tbase0 << 3) + s1 + sdiff;
        taddrs[2] = (tbase2 << 3) + s0;
        taddrs[3] = (tbase2 << 3) + s1 + sdiff;
    } else if (tformat == FORMAT_YUV || tsize == PIXEL_SIZE_8BIT) {
        taddrs[0] = (tbase0 << 3) + s0;
        taddrs[1] = (tbase0 << 3) + s1;
        taddrs[2] = (tbase2 << 3) + s0;
        taddrs[3] = (tbase2 << 3) + s1;
    } else if (tsize == PIXEL_SIZE_4BIT) {
        taddrs[0] = ((tbase0 << 4) + s0) >> 1;
        taddrs[1] = ((tbase0 << 4) + s1) >> 1;
        taddrs[2] = ((tbase2 << 4) + s0) >> 1;
        taddrs[3] = ((tbase2 << 4) + s1) >> 1;
    } else { /* 16B / 32B */
        taddrs[0] = (tbase0 << 2) + s0;
        taddrs[1] = (tbase0 << 2) + s1;
        taddrs[2] = (tbase2 << 2) + s0;
        taddrs[3] = (tbase2 << 2) + s1;
    }

    uint32_t taddrs_low[4] = {
        (taddrs[0] + 0) >> 1,
        (taddrs[1] + sdiff) >> 1,
        (taddrs[2] + 0) >> 1,
        (taddrs[3] + sdiff) >> 1,
    };

    // XORs for endianness

    uint32_t taddr_xor_b_L = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
    uint32_t taddr_xor_b_H = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
    uint32_t taddr_xor_w_L = taddr_xor_b_L >> 1;
    uint32_t taddr_xor_w_H = taddr_xor_b_H >> 1;

    if (tformat == FORMAT_YUV || tsize == PIXEL_SIZE_4BIT || tsize == PIXEL_SIZE_8BIT) {
        taddrs[0] ^= taddr_xor_b_L;
        taddrs[1] ^= taddr_xor_b_L;
        taddrs[2] ^= taddr_xor_b_H;
        taddrs[3] ^= taddr_xor_b_H;
    } else { /* TEXEL_SIZ_16b || TEXEL_SIZ_32b */
        taddrs[0] ^= taddr_xor_w_L;
        taddrs[1] ^= taddr_xor_w_L;
        taddrs[2] ^= taddr_xor_w_H;
        taddrs[3] ^= taddr_xor_w_H;
    }

    taddrs_low[0] ^= taddr_xor_w_L;
    taddrs_low[1] ^= taddr_xor_w_L;
    taddrs_low[2] ^= taddr_xor_w_H;
    taddrs_low[3] ^= taddr_xor_w_H;

    // TMEM access

    uint16_t c1[4];
    uint16_t c2[4];

    switch (wstate->tile[tilenum].f.notlutswitch) {
        case_no_default;

        case TEXEL_CI4:
        case TEXEL_RGBA4:
        case TEXEL_I4:
        case TEXEL_IA4:
            for (int k = 0; k < 4; k++) {
                uint16_t c = wstate->tmem[taddrs[k] & 0xfff];
                int sel = (k & 1) ? (s1 & 1) : (s0 & 1);
                c1[k] = sel ? (c & 0xf) : (c >> 4);
            }
            break;

        case TEXEL_RGBA8:
        case TEXEL_I8:
        case TEXEL_CI8:
        case TEXEL_IA8:
            for (int k = 0; k < 4; k++) {
                c1[k] = wstate->tmem[taddrs[k] & 0xfff];
            }
            break;

        case TEXEL_RGBA32:
            for (int k = 0; k < 4; k++) {
                c1[k] = tc16[(taddrs[k] & 0x3ff) | (0x000 >> 1)];
                c2[k] = tc16[(taddrs[k] & 0x3ff) | (0x800 >> 1)];
            }
            break;

        case TEXEL_IA16:
        case TEXEL_RGBA16:
        case TEXEL_CI16:
        case TEXEL_CI32:
        case TEXEL_I16:
        case TEXEL_I32:
        case TEXEL_IA32:
            for (int k = 0; k < 4; k++) {
                c1[k] = c2[k] = tc16[taddrs[k] & 0x7ff];
            }
            break;

        case TEXEL_YUV4:
        case TEXEL_YUV8:
            for (int k = 0; k < 4; k++) {
                c1[k] = wstate->tmem[taddrs[k] & 0x7ff];
            }
            break;
        case TEXEL_YUV16:
            for (int k = 0; k < 4; k++) {
                c1[k] = tc16[taddrs_low[k] & 0x3ff];
                c2[k] = wstate->tmem[(taddrs[k] & 0x7ff) | 0x800];
            }
            break;
        case TEXEL_YUV32:
            for (int k = 0; k < 4; k++) {
                c1[k] = tc16[taddrs_low[k] & 0x3ff];

                int ys = (k & 1) ? s0 : s1;
                c2[k] = (ys & 1) ? wstate->tmem[(taddrs[k] & 0x7ff) | 0x800]
                                 : tc16[((taddrs[k] >> 1) & 0x3ff) | (0x800 >> 1)];
            }
            break;
    }

    // Format Conversion

    switch (wstate->tile[tilenum].f.notlutswitch) {
        case_no_default;

        case TEXEL_RGBA4:
        case TEXEL_I4:
            for (int k = 0; k < 4; k++) {
                uint16_t c = c1[k];
                colors[k]->r = colors[k]->g = colors[k]->b = colors[k]->a = (c << 4) | c;
            }
            break;
        case TEXEL_CI4:
            for (int k = 0; k < 4; k++) {
                uint16_t c = c1[k];
                colors[k]->r = colors[k]->g = colors[k]->b = colors[k]->a = (wstate->tile[tilenum].palette << 4) | c;
            }
            break;
        case TEXEL_IA4:
            for (int k = 0; k < 4; k++) {
                uint16_t c = c1[k];
                uint8_t i = c & 0xe;
                uint8_t a = c & 0x1;
                colors[k]->r = colors[k]->g = colors[k]->b = (i << 4) | (i << 1) | (i >> 2);
                colors[k]->a = a * 255;
            }
            break;

        case TEXEL_RGBA8:
        case TEXEL_I8:
        case TEXEL_CI8:
            for (int k = 0; k < 4; k++) {
                colors[k]->r = colors[k]->g = colors[k]->b = colors[k]->a = c1[k];
            }
            break;
        case TEXEL_IA8:
            for (int k = 0; k < 4; k++) {
                uint16_t c = c1[k];
                uint8_t i = c & 0xf0;
                uint8_t a = c & 0x0f;
                colors[k]->r = colors[k]->g = colors[k]->b = i | (i >> 4);
                colors[k]->a = (a << 4) | a;
            }
            break;

        case TEXEL_RGBA16:
            for (int k = 0; k < 4; k++) {
                uint16_t c = c1[k];
                colors[k]->r = RGBA16_EXTEND_R(c);
                colors[k]->g = RGBA16_EXTEND_G(c);
                colors[k]->b = RGBA16_EXTEND_B(c);
                colors[k]->a = (c & 1) * 255;
            }
            break;
        case TEXEL_IA16:
            for (int k = 0; k < 4; k++) {
                uint16_t c = c1[k];
                colors[k]->r = colors[k]->g = colors[k]->b = c >> 8;
                colors[k]->a = c & 0xff;
            }
            break;

        case TEXEL_CI16:
        case TEXEL_CI32:
        case TEXEL_IA32:
        case TEXEL_I16:
        case TEXEL_I32:
            for (int k = 0; k < 4; k++) {
                uint16_t c = c1[k];
                colors[k]->r = c >> 8;
                colors[k]->g = c & 0xff;
                colors[k]->b = c >> 8;
                colors[k]->a = c & 0xff;
            }
            break;

        case TEXEL_RGBA32:
            for (int k = 0; k < 4; k++) {
                uint16_t c1k = c1[k];
                uint16_t c2k = c2[k];
                colors[k]->r = c1k >> 8;
                colors[k]->g = c1k & 0xff;
                colors[k]->b = c2k >> 8;
                colors[k]->a = c2k & 0xff;
            }
            break;

            /* YUV Formats */

        case TEXEL_YUV4:
            for (int k = 0; k < 4; k++) {
                uint16_t c = c1[k];
                c &= 0xf0;
                c = c | (c >> 4);
                colors[k]->r = colors[k]->g = c - 0x80;

                if (unequaluppers) {
                    c = c1[3 - k];
                    c &= 0xf0;
                    c = c | (c >> 4);
                }
                colors[k]->b = colors[k]->a = c;
            }
            break;

        case TEXEL_YUV8:
            for (int k = 0; k < 4; k++) {
                uint16_t c = c1[k];
                colors[k]->r = colors[k]->g = c - 0x80;

                if (unequaluppers)
                    c = c1[3 - k];

                colors[k]->b = colors[k]->a = c;
            }
            break;

        case TEXEL_YUV16:
        case TEXEL_YUV32:
            for (int k = 0; k < 4; k++) {
                uint16_t c1k = c1[k];
                uint16_t c2k = c2[k];

                int32_t y = c2k;
                int32_t u = c1k >> 8;
                int32_t v = c1k & 0xff;
                colors[k]->r = u - 0x80;
                colors[k]->g = v - 0x80;

                int ys = (k & 1) ? s0 : s1;
                if (tformat == TEXEL_YUV16 || ys & 1) {
                    colors[k]->b = colors[k]->a = y;
                } else {
                    colors[k]->b = y >> 8;
                    colors[k]->a = ((y >> 8) & 0xf) | (y & 0xf0);
                }
            }
            break;
    }
}

static INLINE void
tlut_dereference(struct rdp_state *wstate, uint32_t *taddrs, struct color *color0, struct color *color1,
                 struct color *color2, struct color *color3, bool upperrg, bool upperba)
{
    // Dereference tlut
    // Hardware is capable of doing this on the same cycle since the tlut data
    // is in high tmem and the indices are in low tmem, but might delay 1 cycle
    // anyway depending on pipelining and signal propagation speeds

    uint32_t xorupperrg = upperrg ? (WORD_ADDR_XOR ^ 3) : WORD_ADDR_XOR;

    uint16_t c0 = tlut[taddrs[0] ^ xorupperrg];
    uint16_t c2 = tlut[taddrs[2] ^ xorupperrg];
    uint16_t c1 = tlut[taddrs[1] ^ xorupperrg];
    uint16_t c3 = tlut[taddrs[3] ^ xorupperrg];

    uint16_t c0s, c1s, c2s, c3s;
    if (upperrg == upperba) {
        c0s = c0;
        c1s = c1;
        c2s = c2;
        c3s = c3;
    } else {
        c0s = c3;
        c1s = c2;
        c2s = c1;
        c3s = c0;
    }

    if (wstate->other_modes.tlut_type == TLUT_RGBA16) {
        color0->r = RGBA16_EXTEND_R(c0);
        color0->g = RGBA16_EXTEND_G(c0);
        color0->b = RGBA16_EXTEND_B(c0s);
        color0->a = (c0s & 1) ? 0xff : 0;

        color1->r = RGBA16_EXTEND_R(c1);
        color1->g = RGBA16_EXTEND_G(c1);
        color1->b = RGBA16_EXTEND_B(c1s);
        color1->a = (c1s & 1) ? 0xff : 0;

        color2->r = RGBA16_EXTEND_R(c2);
        color2->g = RGBA16_EXTEND_G(c2);
        color2->b = RGBA16_EXTEND_B(c2s);
        color2->a = (c2s & 1) ? 0xff : 0;

        color3->r = RGBA16_EXTEND_R(c3);
        color3->g = RGBA16_EXTEND_G(c3);
        color3->b = RGBA16_EXTEND_B(c3s);
        color3->a = (c3s & 1) ? 0xff : 0;
    } else { // IA16
        color0->r = color0->g = c0 >> 8;
        color0->b = c0s >> 8;
        color0->a = c0s & 0xff;

        color1->r = color1->g = c1 >> 8;
        color1->b = c1s >> 8;
        color1->a = c1s & 0xff;

        color2->r = color2->g = c2 >> 8;
        color2->b = c2s >> 8;
        color2->a = c2s & 0xff;

        color3->r = color3->g = c3 >> 8;
        color3->b = c3s >> 8;
        color3->a = c3s & 0xff;
    }
}

static INLINE void
fetch_texel_entlut_quadro_nearest(struct rdp_state *wstate, struct color *color0, struct color *color1,
                                  struct color *color2, struct color *color3, int s0, int t0, uint32_t tilenum,
                                  int upperrg, int upperba)
{
    int tsize = wstate->tile[tilenum].size;
    int tformat = wstate->tile[tilenum].format;
    uint32_t tpal = wstate->tile[tilenum].palette;

    // Determine address

    uint32_t tbase0 = wstate->tile[tilenum].line * t0 + wstate->tile[tilenum].tmem;

    uint32_t taddr;
    if (tformat == FORMAT_YUV || tsize == PIXEL_SIZE_8BIT)
        taddr = (tbase0 << 3) + s0;
    else if (tsize == PIXEL_SIZE_4BIT)
        taddr = ((tbase0 << 4) + s0) >> 1;
    else // 16-bit or 32-bit (and not YUV)
        taddr = (tbase0 << 2) + s0;

    // XOR for endianness

    uint32_t xort;

    if (tsize >= PIXEL_SIZE_16BIT && tformat != FORMAT_YUV)
        xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR; // 16-bit samples
    else
        xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR; // 8-bit samples

    taddr ^= xort;

    // TMEM access for tlut index

    uint16_t c;

    switch (wstate->tile[tilenum].f.tlutswitch) {
        case_no_default;

        case 0: // CI4
        case 1: // IA4
        case 2: // I4 / RGBA4
            c = wstate->tmem[taddr & 0x7ff];
            c = (s0 & 1) ? (c & 0xf) : (c >> 4); // Alternate which 4 bits to use based on s coord
            taddr = ((tpal << 4) | c) << 2;      // shift left by 2 for quad sampling 4 addresses in a row
            break;

        case 3: // YUV4
            // Like the other 4-bit formats except always use the upper 4 bits of the 8-bit sample
            c = wstate->tmem[taddr & 0x7ff];
            taddr = ((tpal << 4) | (c >> 4)) << 2;
            break;

        case 4:  // CI8
        case 5:  // IA8
        case 6:  // I8 / RGBA8
        case 7:  // YUV8
        case 11: // YUV16
        case 15: // YUV32
            c = wstate->tmem[taddr & 0x7ff];
            taddr = c << 2;
            break;

        case 8:  // CI16
        case 9:  // IA16
        case 10: // I16 / RGBA16
        case 12: // CI32
        case 13: // IA32
        case 14: // I32 / RGBA32
            c = tc16[taddr & 0x3ff];
            taddr = (c >> 6) & ~3; // basically CI8 except using the upper 8 bits of the 16-bit sample
            break;
    }

    uint32_t taddrs[4] = {
        taddr + 0,
        taddr + 1,
        taddr + 2,
        taddr + 3,
    };

    tlut_dereference(wstate, taddrs, color0, color1, color2, color3, upperrg, upperba);
}

static INLINE void
fetch_texel_entlut_quadro(struct rdp_state *wstate, struct color *color0, struct color *color1, struct color *color2,
                          struct color *color3, int s0, int sdiff, int t0, int tdiff, uint32_t tilenum, int upperrg,
                          int upperba)
{
    int tsize = wstate->tile[tilenum].size;
    int tformat = wstate->tile[tilenum].format;
    uint32_t tpal = wstate->tile[tilenum].palette;

    // Addresses

    uint32_t tbase0 = wstate->tile[tilenum].line * (t0 & 0xff) + wstate->tile[tilenum].tmem;
    int t1 = (t0 & 0xff) + tdiff;
    int s1;
    uint32_t tbase2 = wstate->tile[tilenum].line * t1 + wstate->tile[tilenum].tmem;

    uint32_t taddrs[4];

    if (tformat == FORMAT_YUV)
        s1 = s0 + (sdiff << 1);
    else
        s1 = s0 + sdiff;

    if (tformat == FORMAT_YUV || tsize == PIXEL_SIZE_8BIT) {
        taddrs[0] = (tbase0 << 3) + s0;
        taddrs[1] = (tbase0 << 3) + s1;
        taddrs[2] = (tbase2 << 3) + s0;
        taddrs[3] = (tbase2 << 3) + s1;
    } else if (tsize == PIXEL_SIZE_4BIT) {
        taddrs[0] = ((tbase0 << 4) + s0) >> 1;
        taddrs[1] = ((tbase0 << 4) + s1) >> 1;
        taddrs[2] = ((tbase2 << 4) + s0) >> 1;
        taddrs[3] = ((tbase2 << 4) + s1) >> 1;
    } else {
        taddrs[0] = (tbase0 << 2) + s0;
        taddrs[1] = (tbase0 << 2) + s1;
        taddrs[2] = (tbase2 << 2) + s0;
        taddrs[3] = (tbase2 << 2) + s1;
    }

    // XOR for endianness

    uint32_t xortL, xortH;

    if (tsize >= PIXEL_SIZE_16BIT && tformat != FORMAT_YUV) {
        xortL = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
        xortH = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
    } else {
        xortL = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
        xortH = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
    }

    taddrs[0] ^= xortL;
    taddrs[1] ^= xortL;
    taddrs[2] ^= xortH;
    taddrs[3] ^= xortH;

    // TMEM access for tlut indices

    uint16_t c;

    switch (wstate->tile[tilenum].f.tlutswitch) {
        case_no_default;

        case 0: // CI4
        case 1: // IA4
        case 2: // I4 / RGBA4
            for (int k = 0; k < 4; k++) {
                c = wstate->tmem[taddrs[k] & 0x7ff];
                int sel = (k & 1) ? (s1 & 1) : (s0 & 1);
                c = sel ? (c & 0xf) : (c >> 4);
                taddrs[k] = (((tpal << 4) | c) << 2) + k;
            }
            break;

        case 3: // YUV4
            for (int k = 0; k < 4; k++) {
                c = wstate->tmem[taddrs[k] & 0x7ff];
                taddrs[k] = (((tpal << 4) | (c >> 4)) << 2) + k;
            }
            break;

        case 4:  // CI8
        case 5:  // IA8
        case 6:  // I8 / RGBA8
        case 7:  // YUV8
        case 11: // YUV16
        case 15: // YUV32
            for (int k = 0; k < 4; k++) {
                c = wstate->tmem[taddrs[k] & 0x7ff];
                taddrs[k] = (c << 2) + k;
            }
            break;

        case 8:  // CI16
        case 9:  // IA16
        case 10: // I16 / RGBA16
        case 12: // CI32
        case 13: // IA32
        case 14: // I32 / RGBA32
            for (int k = 0; k < 4; k++) {
                c = tc16[taddrs[k] & 0x3ff];
                taddrs[k] = ((c >> 6) & ~3) + k;
            }
            break;
    }

    tlut_dereference(wstate, taddrs, color0, color1, color2, color3, upperrg, upperba);
}

static void
get_tmem_idx(struct rdp_state *wstate, int s, int t, uint32_t tilenum, uint32_t *idx0, uint32_t *idx1, uint32_t *idx2,
             uint32_t *idx3, uint32_t *bit3flipped, uint32_t *hibit)
{
    uint32_t tbase = wstate->tile[tilenum].tmem + ((wstate->tile[tilenum].line * t) & 0x1ff);
    int tsize = wstate->tile[tilenum].size;
    int tformat = wstate->tile[tilenum].format;

    // Shift s coordinate based on formatting
    if (tsize == PIXEL_SIZE_8BIT || tformat == FORMAT_YUV)
        s >>= 1;
    else if (tsize >= PIXEL_SIZE_16BIT)
        s >>= 0;
    else
        s >>= 2;

    s &= 0x7ff;

    // something to do with yuv and rgba32 ?
    *bit3flipped = ((s & 2) != 0) ^ (t & 1);

    // tmem indices
    int tidx_a = ((tbase << 2) + s) & 0x7fd;
    int tidx_b = (tidx_a + 1) & 0x7ff;
    int tidx_c = (tidx_a + 2) & 0x7ff;
    int tidx_d = (tidx_a + 3) & 0x7ff;

    // high tmem?
    *hibit = (tidx_a & 0x400) != 0;

    if (t & 1) { // word swapping?
        tidx_a ^= 2;
        tidx_b ^= 2;
        tidx_c ^= 2;
        tidx_d ^= 2;
    }

    // Sorts the four tmem indices {tidx_a, tidx_b, tidx_c, tidx_d} into {idx0, idx1, idx2, idx3}
    // such that idx0 < idx1 < idx2 < idx3
    sort_tmem_idx(idx0, tidx_a, tidx_b, tidx_c, tidx_d, 0);
    sort_tmem_idx(idx1, tidx_a, tidx_b, tidx_c, tidx_d, 1);
    sort_tmem_idx(idx2, tidx_a, tidx_b, tidx_c, tidx_d, 2);
    sort_tmem_idx(idx3, tidx_a, tidx_b, tidx_c, tidx_d, 3);
}

static void
read_tmem_copy(struct rdp_state *wstate, int s, int s1, int s2, int s3, int t, uint32_t tilenum, uint32_t *sortshort,
               int *hibits, int *lowbits)
{
    uint32_t tbase = wstate->tile[tilenum].tmem + ((wstate->tile[tilenum].line * t) & 0x1ff);
    uint32_t tsize = wstate->tile[tilenum].size;
    uint32_t tformat = wstate->tile[tilenum].format;

    uint32_t shbytes0, shbytes1, shbytes2, shbytes3;
    if (tsize == PIXEL_SIZE_8BIT || tformat == FORMAT_YUV) {
        shbytes0 = s << 1;
        shbytes1 = s1 << 1;
        shbytes2 = s2 << 1;
        shbytes3 = s3 << 1;
    } else if (tsize >= PIXEL_SIZE_16BIT) { // While this is >=, note that PIXEL_SIZE_32BIT is invalid for copy mode
        shbytes0 = s << 2;
        shbytes1 = s1 << 2;
        shbytes2 = s2 << 2;
        shbytes3 = s3 << 2;
    } else {
        shbytes0 = s;
        shbytes1 = s1;
        shbytes2 = s2;
        shbytes3 = s3;
    }

    shbytes0 &= 0x1fff;
    shbytes1 &= 0x1fff;
    shbytes2 &= 0x1fff;
    shbytes3 &= 0x1fff;

    int tidx_a, tidx_blow, tidx_bhi, tidx_c, tidx_dlow, tidx_dhi;

    tbase <<= 4;
    tidx_a = (tbase + shbytes0) & 0x1fff;
    tidx_bhi = (tbase + shbytes1) & 0x1fff;
    tidx_c = (tbase + shbytes2) & 0x1fff;
    tidx_dhi = (tbase + shbytes3) & 0x1fff;

    if (tformat == FORMAT_YUV) {
        int32_t delta = shbytes1 - shbytes0;
        tidx_blow = (tidx_a + (delta << 1)) & 0x1fff;
        tidx_dlow = (tidx_blow + shbytes3 - shbytes0) & 0x1fff;
    } else {
        tidx_blow = tidx_bhi;
        tidx_dlow = tidx_dhi;
    }

    if (t & 1) {
        tidx_a ^= 8;
        tidx_blow ^= 8;
        tidx_bhi ^= 8;
        tidx_c ^= 8;
        tidx_dlow ^= 8;
        tidx_dhi ^= 8;
    }

    hibits[0] = (tidx_a & 0x1000) != 0;
    hibits[1] = (tidx_blow & 0x1000) != 0;
    hibits[2] = (tidx_bhi & 0x1000) != 0;
    hibits[3] = (tidx_c & 0x1000) != 0;
    hibits[4] = (tidx_dlow & 0x1000) != 0;
    hibits[5] = (tidx_dhi & 0x1000) != 0;
    lowbits[0] = tidx_a & 0xf;
    lowbits[1] = tidx_blow & 0xf;
    lowbits[2] = tidx_bhi & 0xf;
    lowbits[3] = tidx_c & 0xf;
    lowbits[4] = tidx_dlow & 0xf;
    lowbits[5] = tidx_dhi & 0xf;

    uint32_t short0, short1, short2, short3;

    tidx_a >>= 2;
    tidx_blow >>= 2;
    tidx_bhi >>= 2;
    tidx_c >>= 2;
    tidx_dlow >>= 2;
    tidx_dhi >>= 2;

    uint32_t sortidx[8];
    sort_tmem_idx(&sortidx[0], tidx_a, tidx_blow, tidx_c, tidx_dlow, 0);
    sort_tmem_idx(&sortidx[1], tidx_a, tidx_blow, tidx_c, tidx_dlow, 1);
    sort_tmem_idx(&sortidx[2], tidx_a, tidx_blow, tidx_c, tidx_dlow, 2);
    sort_tmem_idx(&sortidx[3], tidx_a, tidx_blow, tidx_c, tidx_dlow, 3);

    // Get low TMEM sample
    short0 = tmem16[sortidx[0] ^ WORD_ADDR_XOR];
    short1 = tmem16[sortidx[1] ^ WORD_ADDR_XOR];
    short2 = tmem16[sortidx[2] ^ WORD_ADDR_XOR];
    short3 = tmem16[sortidx[3] ^ WORD_ADDR_XOR];

    sort_tmem_shorts_lowhalf(&sortshort[0], short0, short1, short2, short3, lowbits[0] >> 2);
    sort_tmem_shorts_lowhalf(&sortshort[1], short0, short1, short2, short3, lowbits[1] >> 2);
    sort_tmem_shorts_lowhalf(&sortshort[2], short0, short1, short2, short3, lowbits[3] >> 2);
    sort_tmem_shorts_lowhalf(&sortshort[3], short0, short1, short2, short3, lowbits[4] >> 2);

    if (wstate->other_modes.en_tlut) {

        compute_color_index(wstate, &short0, sortshort[0], lowbits[0] & 3, tilenum);
        compute_color_index(wstate, &short1, sortshort[1], lowbits[1] & 3, tilenum);
        compute_color_index(wstate, &short2, sortshort[2], lowbits[3] & 3, tilenum);
        compute_color_index(wstate, &short3, sortshort[3], lowbits[4] & 3, tilenum);

        sortidx[4] = (short0 << 2) | 0;
        sortidx[5] = (short1 << 2) | 1;
        sortidx[6] = (short2 << 2) | 2;
        sortidx[7] = (short3 << 2) | 3;
    } else {
        sort_tmem_idx(&sortidx[4], tidx_a, tidx_bhi, tidx_c, tidx_dhi, 0);
        sort_tmem_idx(&sortidx[5], tidx_a, tidx_bhi, tidx_c, tidx_dhi, 1);
        sort_tmem_idx(&sortidx[6], tidx_a, tidx_bhi, tidx_c, tidx_dhi, 2);
        sort_tmem_idx(&sortidx[7], tidx_a, tidx_bhi, tidx_c, tidx_dhi, 3);
    }

    // Get high TMEM sample
    short0 = tmem16[(sortidx[4] | 0x400) ^ WORD_ADDR_XOR];
    short1 = tmem16[(sortidx[5] | 0x400) ^ WORD_ADDR_XOR];
    short2 = tmem16[(sortidx[6] | 0x400) ^ WORD_ADDR_XOR];
    short3 = tmem16[(sortidx[7] | 0x400) ^ WORD_ADDR_XOR];

    if (wstate->other_modes.en_tlut) {
        sort_tmem_shorts_lowhalf(&sortshort[4], short0, short1, short2, short3, 0);
        sort_tmem_shorts_lowhalf(&sortshort[5], short0, short1, short2, short3, 1);
        sort_tmem_shorts_lowhalf(&sortshort[6], short0, short1, short2, short3, 2);
        sort_tmem_shorts_lowhalf(&sortshort[7], short0, short1, short2, short3, 3);
    } else {
        sort_tmem_shorts_lowhalf(&sortshort[4], short0, short1, short2, short3, lowbits[0] >> 2);
        sort_tmem_shorts_lowhalf(&sortshort[5], short0, short1, short2, short3, lowbits[2] >> 2);
        sort_tmem_shorts_lowhalf(&sortshort[6], short0, short1, short2, short3, lowbits[3] >> 2);
        sort_tmem_shorts_lowhalf(&sortshort[7], short0, short1, short2, short3, lowbits[5] >> 2);
    }
}

static void
tmem_init_lut(void)
{
    // For expanding RGBA16 to RGBA32
    for (int i = 0; i < 32; i++)
        replicated_rgba[i] = (i << 3) | (i >> 2);
}

// fetch_texel wrapper that resolves TLUT for CI textures (fetch_texel alone
// uses notlutswitch and returns raw palette indices). Uses the same address
// math and colour tables as tlut_dereference.
void
fetch_texel_decode(struct rdp_state *wstate, struct color *color, int s, int t, uint32_t tilenum)
{
    fetch_texel(wstate, color, s, t, tilenum);
    if (wstate->tile[tilenum].format != FORMAT_CI || !wstate->other_modes.en_tlut)
        return;

    // Build the TLUT entry address the same way tlutswitch does.
    int entry = (wstate->tile[tilenum].size == PIXEL_SIZE_4BIT)
        ? ((wstate->tile[tilenum].palette << 4) | (color->r & 0xf))
        : (color->r & 0xff);
    uint32_t taddr = (uint32_t)(entry << 2); // matches (entry << 2) + 0 in tlutswitch

    // Read the TLUT entry the same way tlut_dereference does.
    uint16_t v = tlut[taddr ^ WORD_ADDR_XOR];

    if (wstate->other_modes.tlut_type == 0) { // RGBA16
        color->r = replicated_rgba[(v >> 11) & 0x1f];
        color->g = replicated_rgba[(v >>  6) & 0x1f];
        color->b = replicated_rgba[(v >>  1) & 0x1f];
        color->a = (v & 1) ? 255 : 0;
    } else { // IA16
        color->r = color->g = color->b = v >> 8;
        color->a = v & 0xff;
    }
}

#endif // N64VIDEO_C
