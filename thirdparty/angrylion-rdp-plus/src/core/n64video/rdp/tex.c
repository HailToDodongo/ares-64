#ifdef N64VIDEO_C

static STRICTINLINE void
tcmask_single(int32_t *C, int mask, int mirror, int maskclamped)
{
    if (mask == 0)
        return;

    int32_t maskbits = maskbits_table[mask]; // maskbits_table[i] = ((uint16_t)(0xffff) >> (16 - i)) & 0x3ff
    int32_t C1 = *C;

    if (mirror) {
        int32_t wrap = (C1 >> maskclamped) & 1;
        C1 ^= -wrap;
    }
    *C = C1 & maskbits;
}

static STRICTINLINE void
tcmask(struct tile *tile, int32_t *S, int32_t *T)
{
    tcmask_single(S, tile->mask_s, tile->ms, tile->f.masksclamped);
    tcmask_single(T, tile->mask_t, tile->mt, tile->f.masktclamped);
}

static STRICTINLINE void
tcmask_coupled_single(int32_t *C, int32_t *Cdiff, int mask, int mirror, int maskclamped, uint32_t unkmask)
{
    if (mask == 0) {
        *Cdiff = 1;
        return;
    }

    int32_t maskbits = maskbits_table[mask]; // maskbits_table[i] = ((uint16_t)(0xffff) >> (16 - i)) & 0x3ff
    int32_t C1 = *C;

    if (mirror) {
        int32_t wrap = C1 >> maskclamped & 1;
        C1 ^= -wrap;
        C1 &= maskbits;

        if (((C1 - wrap) & maskbits) == maskbits)
            *Cdiff = 0;
        else
            *Cdiff = 1 - (wrap << 1);
    } else {
        C1 &= maskbits;
        if (C1 == maskbits)
            *Cdiff = -(C1 & unkmask); // TODO what is this masking about and why is it different for S and T (is it
                                      // meaningfully different?)
        else
            *Cdiff = 1;
    }
    *C = C1;
}

static STRICTINLINE void
tcmask_coupled(struct tile *tile, int32_t *S, int32_t *sdiff, int32_t *T, int32_t *tdiff)
{
    tcmask_coupled_single(S, sdiff, tile->mask_s, tile->ms, tile->f.masksclamped, ~0);
    tcmask_coupled_single(T, tdiff, tile->mask_t, tile->mt, tile->f.masktclamped, 0xFF);
}

static INLINE void
calculate_clamp_diffs(struct tile *t)
{
    // lrs - uls
    t->f.clampdiffs = ((t->sh >> 2) - (t->sl >> 2)) & 0x3ff;
    // lrt - ult
    t->f.clampdifft = ((t->th >> 2) - (t->tl >> 2)) & 0x3ff;
}

static INLINE void
calculate_tile_derivs(struct tile *t)
{
    t->f.clampens = t->cs || !t->mask_s;
    t->f.clampent = t->ct || !t->mask_t;

    t->f.masksclamped = t->mask_s <= 10 ? t->mask_s : 10;
    t->f.masktclamped = t->mask_t <= 10 ? t->mask_t : 10;

    int format = t->format;
    if (format > 4)
        format = 4;

    t->f.notlutswitch = (format << 2) | t->size;
    t->f.tlutswitch = (t->size << 2) | ((format + 2) & 3);
}

static STRICTINLINE void
get_texel1_1cycle(struct rdp_state *wstate, int32_t *s1, int32_t *t1, int32_t s, int32_t t, int32_t w, int32_t dsinc,
                  int32_t dtinc, int32_t dwinc, int32_t scanline, struct spansigs *sigs)
{
    int32_t nextscan = scanline + 1;

    int32_t nexts, nextt, nextw;
    if (!sigs->endspan || !sigs->longspan || !wstate->span[nextscan].validline) {
        // sample from current span if any of:
        //  not at the end of the span
        //  not a "long span" (8 pixels or more)
        //  next scan is not valid
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
        nextw = (w + dwinc) >> 16;
    } else {
        // sample from start of next span
        nextt = wstate->span[nextscan].t >> 16;
        nexts = wstate->span[nextscan].s >> 16;
        nextw = wstate->span[nextscan].w >> 16;
    }
    wstate->tcdiv_ptr(nexts, nextt, nextw, s1, t1);
}

static STRICTINLINE void
bilerp_calc(struct color *TEX, struct color *t0, struct color *t1, struct color *t2, struct color *t3, int32_t sfracrg,
            int32_t tfrac, int centerrg, int upperrg, int32_t sfracba, int centerba, int upperba)
{
    int32_t invtf = 0x20 - tfrac;

    if (!centerrg) {
        if (upperrg) {
            int32_t invsfrg = 0x20 - sfracrg;

            //   R = R_3  + ((32 - S) * ( R_2 -  R_3) + (32 - T) * ( R_1 -  R_3) + 16) / 32
            TEX->r = t3->r + ((invsfrg * (t2->r - t3->r) + invtf * (t1->r - t3->r) + 0x10) >> 5);
            TEX->g = t3->g + ((invsfrg * (t2->g - t3->g) + invtf * (t1->g - t3->g) + 0x10) >> 5);
        } else {
            TEX->r = t0->r + ((sfracrg * (t1->r - t0->r) + tfrac * (t2->r - t0->r) + 0x10) >> 5);
            TEX->g = t0->g + ((sfracrg * (t1->g - t0->g) + tfrac * (t2->g - t0->g) + 0x10) >> 5);
        }
    } else {
        // Do not be deceived, this is literally just
        // TEX->r = (t0->r + t1->r + t2->r + t3->r + 2) / 4;
        int32_t invt3r = ~t3->r;
        int32_t invt3g = ~t3->g;

        TEX->r = t3->r + ((((t1->r + t2->r) << 6) - (t3->r << 7) + ((invt3r + t0->r) << 6) + 0xc0) >> 8);
        TEX->g = t3->g + ((((t1->g + t2->g) << 6) - (t3->g << 7) + ((invt3g + t0->g) << 6) + 0xc0) >> 8);
    }

    if (!centerba) {
        if (upperba) {
            int32_t invsf = 0x20 - sfracba;

            //   B = R_3  + ((32 - S) * (R_2 - R_3) + (32 - T) * (R_1 - R_3) + 16) / 32
            TEX->b = t3->b + ((invsf * (t2->b - t3->b) + invtf * (t1->b - t3->b) + 0x10) >> 5);
            TEX->a = t3->a + ((invsf * (t2->a - t3->a) + invtf * (t1->a - t3->a) + 0x10) >> 5);
        } else {
            TEX->b = t0->b + ((sfracba * (t1->b - t0->b) + tfrac * (t2->b - t0->b) + 0x10) >> 5);
            TEX->a = t0->a + ((sfracba * (t1->a - t0->a) + tfrac * (t2->a - t0->a) + 0x10) >> 5);
        }
    } else {
        int32_t invt3b = ~t3->b;
        int32_t invt3a = ~t3->a;

        TEX->b = t3->b + ((((t1->b + t2->b) << 6) - (t3->b << 7) + ((invt3b + t0->b) << 6) + 0xc0) >> 8);
        TEX->a = t3->a + ((((t1->a + t2->a) << 6) - (t3->a << 7) + ((invt3a + t0->a) << 6) + 0xc0) >> 8);
    }
}

static STRICTINLINE void
bilerp_conv(struct color *TEX, struct color *prev, struct color *t0, struct color *t1, struct color *t2,
            struct color *t3, int centerrg, int centerba, int upperrg, int upperba)
{
    int32_t prevr = SIGN(prev->r, 9);
    int32_t prevg = SIGN(prev->g, 9);
    int32_t prevb = SIGN(prev->b, 9);

    if (!centerrg) {
        if (upperrg) {
            // MADD Q0.8 with Q0.8
            TEX->r = prevb + ((prevr * (t2->r - t3->r) + prevg * (t1->r - t3->r) + 0x80) >> 8);
            TEX->g = prevb + ((prevr * (t2->g - t3->g) + prevg * (t1->g - t3->g) + 0x80) >> 8);
        } else {
            TEX->r = prevb + ((prevr * (t1->r - t0->r) + prevg * (t2->r - t0->r) + 0x80) >> 8);
            TEX->g = prevb + ((prevr * (t1->g - t0->g) + prevg * (t2->g - t0->g) + 0x80) >> 8);
        }
    } else {
        int32_t invt3r = ~t3->r;
        int32_t invt3g = ~t3->g;

        TEX->r = prevb + ((prevr * (t2->r - t3->r) + prevg * (t1->r - t3->r) + ((invt3r + t0->r) << 6) + 0xc0) >> 8);
        TEX->g = prevb + ((prevr * (t2->g - t3->g) + prevg * (t1->g - t3->g) + ((invt3g + t0->g) << 6) + 0xc0) >> 8);
    }

    if (!centerba) {
        if (upperba) {
            TEX->b = prevb + ((prevr * (t2->b - t3->b) + prevg * (t1->b - t3->b) + 0x80) >> 8);
            TEX->a = prevb + ((prevr * (t2->a - t3->a) + prevg * (t1->a - t3->a) + 0x80) >> 8);
        } else {
            TEX->b = prevb + ((prevr * (t1->b - t0->b) + prevg * (t2->b - t0->b) + 0x80) >> 8);
            TEX->a = prevb + ((prevr * (t1->a - t0->a) + prevg * (t2->a - t0->a) + 0x80) >> 8);
        }
    } else {
        int32_t invt3b = ~t3->b;
        int32_t invt3a = ~t3->a;

        TEX->b = prevb + ((prevr * (t2->b - t3->b) + prevg * (t1->b - t3->b) + ((invt3b + t0->b) << 6) + 0xc0) >> 8);
        TEX->a = prevb + ((prevr * (t2->a - t3->a) + prevg * (t1->a - t3->a) + ((invt3a + t0->a) << 6) + 0xc0) >> 8);
    }
}

static STRICTINLINE void
texture_pipeline_cycle(struct rdp_state *wstate, struct color *TEX, struct color *prev, int32_t SSS, int32_t SST,
                       uint32_t tilenum, uint32_t cycle)
{
    int32_t maxs, maxt;
    int32_t sfrac, tfrac;
    int32_t sfracrg, sfracba;

    bool bilerp = cycle ? wstate->other_modes.bi_lerp1 : wstate->other_modes.bi_lerp0;
    bool convert = wstate->other_modes.convert_one && cycle;

    struct color t0, t1, t2, t3;

    int sss1 = SSS;
    int sst1 = SST;

    // Shift texture coordinates
    tcshift_cycle(&wstate->tile[tilenum], &sss1, &sst1, &maxs, &maxt);

    // Convert to relative coordinates
    sss1 = TRELATIVE(sss1, wstate->tile[tilenum].sl);
    sst1 = TRELATIVE(sst1, wstate->tile[tilenum].tl);

    // Extract fraction bits
    sfrac = sss1 & 0x1f;
    tfrac = sst1 & 0x1f;
    // Shift to integer parts only
    sss1 >>= 5;
    sst1 >>= 5;

#define SAMPLE_POINT 0
#define SAMPLE_2x2   1

    int sdiff, tdiff;
    if (wstate->other_modes.sample_type != SAMPLE_POINT || wstate->other_modes.en_tlut) {
        // Clamp texture coordinates
        tcclamp_cycle(&wstate->tile[tilenum], &sss1, &sst1, &sfrac, &tfrac, maxs, maxt);

        // Mask texture coordinate (int parts)
        tcmask_coupled(&wstate->tile[tilenum], &sss1, &sdiff, &sst1, &tdiff);

        if (wstate->tile[tilenum].format == FORMAT_YUV)
            sfracrg = (sfrac >> 1) | ((sss1 & 1) << 4);
        else
            sfracrg = sfrac;

        sfracba = sfrac;

        // Select upper or lower triangle in
        // (0,0) o-----------o (1,0)
        //       | lower    /|
        //       |        /  |
        //       |      /    |
        //       |    /      |
        //       |  /        |
        //       |/    upper |
        // (0,1) o-----------o (1,1)
#if 0
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
        0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
#endif
        int upperrg = (sfracrg + tfrac) & 0x20;
        int upperba = (sfracba + tfrac) & 0x20;

        if (bilerp) {
            // use this cycle to do filtering

            if (wstate->other_modes.sample_type == SAMPLE_POINT)
                fetch_texel_entlut_quadro_nearest(wstate, &t0, &t1, &t2, &t3, sss1, sst1, tilenum, upperba, upperrg);
            else if (wstate->other_modes.en_tlut)
                fetch_texel_entlut_quadro(wstate, &t0, &t1, &t2, &t3, sss1, sdiff, sst1, tdiff, tilenum, upperba,
                                          upperrg);
            else
                fetch_texel_quadro(wstate, &t0, &t1, &t2, &t3, sss1, sdiff, sst1, tdiff, tilenum, upperba - upperrg);

            // check center if mid_texel
            bool centerrg = wstate->other_modes.mid_texel && (sfracrg == 0x10 && tfrac == 0x10); // 0x10 = 0.5 (q10.5)
            bool centerba = wstate->other_modes.mid_texel && (sfracba == 0x10 && tfrac == 0x10);

            if (!convert) // bilerp the newly sampled texel and its position
                bilerp_calc(TEX, &t0, &t1, &t2, &t3, sfracrg, tfrac, centerrg, upperrg, sfracba, centerba, upperba);
            else // bilerp the newly sampled texel and the previously sampled texel (2-cycle mode only)
                bilerp_conv(TEX, prev, &t0, &t1, &t2, &t3, centerrg, centerba, upperrg, upperba);
        } else {
            // use this cycle for YUV -> RGB

            if (convert) {
                // second cycle, bring in previously sampled texel
                t0.r = t3.r = SIGN(prev->r, 9);
                t0.g = t3.g = SIGN(prev->g, 9);
                t0.b = t3.b = SIGN(prev->b, 9);
                t0.a = t3.a = prev->a;
            } else {
                // sample new texel
                if (!wstate->other_modes.sample_type)
                    fetch_texel_entlut_quadro_nearest(wstate, &t0, &t1, &t2, &t3, sss1, sst1, tilenum, upperba,
                                                      upperrg);
                else if (wstate->other_modes.en_tlut)
                    fetch_texel_entlut_quadro(wstate, &t0, &t1, &t2, &t3, sss1, sdiff, sst1, tdiff, tilenum, upperba,
                                              upperrg);
                else
                    fetch_texel_quadro(wstate, &t0, &t1, &t2, &t3, sss1, sdiff, sst1, tdiff, tilenum,
                                       upperba - upperrg);
            }

            // do YUV -> RGB
            struct color *tX_rg = (upperrg) ? &t3 : &t0;
            struct color *tX_ba = (upperba) ? &t3 : &t0;

            // Here r = U , g = V , b = YY
            TEX->r = tX_ba->b + ((wstate->k0_tf * tX_rg->g + 0x80) >> 8);
            TEX->g = tX_ba->b + ((wstate->k1_tf * tX_rg->r + wstate->k2_tf * tX_rg->g + 0x80) >> 8);
            TEX->b = tX_ba->b + ((wstate->k3_tf * tX_rg->r + 0x80) >> 8);
            TEX->a = tX_ba->b;
        }

        TEX->r &= 0x1ff;
        TEX->g &= 0x1ff;
        TEX->b &= 0x1ff;
        TEX->a &= 0x1ff;
    } else {
        tcclamp_cycle_light(&wstate->tile[tilenum], &sss1, &sst1, maxs, maxt);

        tcmask(&wstate->tile[tilenum], &sss1, &sst1);

        if (bilerp) {
            if (convert) {
                TEX->r = TEX->g = TEX->b = TEX->a = prev->b;
            } else {
                fetch_texel(wstate, &t0, sss1, sst1, tilenum);
                TEX->r = t0.r & 0x1ff;
                TEX->g = t0.g & 0x1ff;
                TEX->b = t0.b;
                TEX->a = t0.a;
            }
        } else {
            if (convert) {
                t0.r = SIGN(prev->r, 9);
                t0.g = SIGN(prev->g, 9);
                t0.b = SIGN(prev->b, 9);
                t0.a = prev->a;
            } else {
                fetch_texel(wstate, &t0, sss1, sst1, tilenum);
            }

            TEX->r = t0.b + ((wstate->k0_tf * t0.g + 0x80) >> 8);
            TEX->g = t0.b + ((wstate->k1_tf * t0.r + wstate->k2_tf * t0.g + 0x80) >> 8);
            TEX->b = t0.b + ((wstate->k3_tf * t0.r + 0x80) >> 8);
            TEX->a = t0.b;

            TEX->r &= 0x1ff;
            TEX->g &= 0x1ff;
            TEX->b &= 0x1ff;
            TEX->a &= 0x1ff;
        }
    }
}

static void
loading_pipeline(struct rdp_state *wstate, int ystart, int yend, int tilenum, int coord_quad, int ltlut)
{
    if (yend > ystart && ltlut) {
        // TODO verify that this actually crashes on real hardware, there are conflicting accounts
        rdp_pipeline_crashed = 1;
        return;
    }
    if ((wstate->ti_address & 0b111) && !(wstate->ti_address & 0b111000)) {
        // Weird texture alignments crash, 8-byte alignment always works while lower alignment works if you get lucky.
        // This is one such case that always crashes.
        rdp_pipeline_crashed = 1;
        return;
    }
    if (wstate->ti_size == PIXEL_SIZE_4BIT) {
        // TODO also conflicting accounts on whether this actually crashes, does it work and if so how?
        rdp_pipeline_crashed = true;
        return;
    }

#define TMEM_FMT_YUV    0
#define TMEM_FMT_RGBA32 1
#define TMEM_FMT_NORMAL 2

    // YUV and RGBA32 have special TMEM formatting, so that all the required data can be accessed in a single cycle
    int tmem_formatting;
    if (wstate->tile[tilenum].format == FORMAT_YUV)
        tmem_formatting = TMEM_FMT_YUV;
    else if (wstate->tile[tilenum].format == FORMAT_RGBA && wstate->tile[tilenum].size == PIXEL_SIZE_32BIT)
        tmem_formatting = TMEM_FMT_RGBA32;
    else
        tmem_formatting = TMEM_FMT_NORMAL;

    int tiadvance = 8;                                   // ltlut ? 8/4 : 8
    int spanadvance = 8 / ((4 << wstate->ti_size) >> 3); // 8 / siz_bytes(siz)

    // If quadricating texels for tlut loading, divide by 4
    bool quadricate = wstate->ti_size == PIXEL_SIZE_16BIT && ltlut;
    tiadvance >>= (quadricate << 1);
    spanadvance >>= (quadricate << 1);

    int dsinc = wstate->spans_dsdx;
    int dtinc = wstate->spans_dtdx;

    // Note ycur is integer part of y
    for (int ycur = ystart; ycur <= yend; ycur++) {
        int xstart = wstate->span[ycur].lx;
        int xend = wstate->span[ycur].unscrx;
        int s = wstate->span[ycur].s;
        int t = wstate->span[ycur].t;

        int ti_index = wstate->ti_width * ycur + xend;
        int tiptr = wstate->ti_address + PIXELS_TO_BYTES(ti_index, wstate->ti_size);

        int length = (xstart - xend + 1) & 0xfff;

        for (int x = 0; x < length; x += spanadvance) {
            int sss = s >> 16 & 0xffff;
            int sst = t >> 16 & 0xffff;

            tc_pipeline_load(&wstate->tile[tilenum], &sss, &sst, coord_quad);

            uint32_t bit3fl, hibit;
            uint32_t tmemidx0, tmemidx1, tmemidx2, tmemidx3;
            get_tmem_idx(wstate, sss, sst, tilenum, &tmemidx0, &tmemidx1, &tmemidx2, &tmemidx3, &bit3fl, &hibit);

            // Read 128 bits off the (back-aligned) texture pointer, this is guaranteed to be enough data regardless
            // of alignment and we select the relevant 64 bits below
            uint32_t readval0, readval1, readval2, readval3;
            uint32_t readidx32 = (tiptr >> 2) & ~1;
            RREADIDX32(readval0, readidx32 + 0);
            RREADIDX32(readval1, readidx32 + 1);
            RREADIDX32(readval2, readidx32 + 2);
            RREADIDX32(readval3, readidx32 + 3);

            // NOTE there's some unemulated behavior that occurs on hardware for tile loads where the tile width
            // (lrs - uls + 1) and the DRAM width (ti_width) are different. Hardware will not always load past the
            // tile width, instead it will load up to the next 64-bit DRAM boundary and then fill the remainder of
            // the TMEM line with either 0s or apparent garbage. The origin of the garbage bytes is unclear.

            // Loading textures pass through the span buffers in 64-bit chunks. The garbage bytes are present in the
            // spans indicating they come from the memory controller? Or maybe its used as storage for some state
            // information.. very weird.

// For word-aligned transfers
#define PACK_QWORD_WW(a, b) (((uint64_t)(a) << 32) | ((uint64_t)(b) << 0))

// For subword-aligned transfers
#define PACK_QWORD_WWW(a, b, c, n) \
    (((uint64_t)(a) << (32 + (n))) | ((uint64_t)(b) << (n)) | ((uint64_t)(c) >> (32 - (n))))

// for TLUT quadrication
#define PACK_QWORD_HHHH(a, b, c, d) \
    (((uint64_t)(a) << 48) | ((uint64_t)(b) << 32) | ((uint64_t)(c) << 16) | ((uint64_t)(d) << 0))

            // Shuffle the four 32-bit words into a 64-bit word to facilitate unaligned transfers
            uint64_t loadqword;
            uint16_t tempshort;

            switch (tiptr & 7) {
                case 0:
                    if (!ltlut) {
                        loadqword = PACK_QWORD_WW(readval0, readval1);
                    } else {
                        // quadricate tlut
                        tempshort = readval0 >> 16;
                        loadqword = PACK_QWORD_HHHH(tempshort, tempshort, tempshort, tempshort);
                    }
                    break;

                case 1:
                    loadqword = PACK_QWORD_WWW(readval0, readval1, readval2, 8);
                    break;

                case 2:
                    if (!ltlut) {
                        loadqword = PACK_QWORD_WWW(readval0, readval1, readval2, 16);
                    } else {
                        // quadricate tlut
                        tempshort = readval0 & 0xffff;
                        loadqword = PACK_QWORD_HHHH(tempshort, tempshort, tempshort, tempshort);
                    }
                    break;

                case 3:
                    loadqword = PACK_QWORD_WWW(readval0, readval1, readval2, 24);
                    break;

                case 4: // 4-byte aligned transfer (but not 8-byte aligned)
                    if (!ltlut) {
                        loadqword = PACK_QWORD_WW(readval1, readval2);
                    } else {
                        // quadricate tlut
                        tempshort = readval1 >> 16;
                        loadqword = PACK_QWORD_HHHH(tempshort, tempshort, tempshort, tempshort);
                    }
                    break;

                case 5:
                    loadqword = PACK_QWORD_WWW(readval1, readval2, readval3, 8);
                    break;

                case 6:
                    if (!ltlut) {
                        loadqword = PACK_QWORD_WWW(readval1, readval2, readval3, 16);
                    } else {
                        // quadricate tlut
                        tempshort = readval1 & 0xffff;
                        loadqword = PACK_QWORD_HHHH(tempshort, tempshort, tempshort, tempshort);
                    }
                    break;

                case 7:
                    loadqword = PACK_QWORD_WWW(readval1, readval2, readval3, 24);
                    break;
            }

                // clang-format off
            #define tmem_write16(adr, val) { tmem16[(adr) ^ WORD_ADDR_XOR] = (uint16_t)(val); }(void)0
            // clang-format on

            switch (tmem_formatting) {
                case TMEM_FMT_YUV: // u and v are in low tmem while yy are in high tmem
                    readval0 = (uint32_t)((((loadqword >> 56) & 0xff) << 24) | (((loadqword >> 40) & 0xff) << 16) |
                                          (((loadqword >> 24) & 0xff) << 8) | (((loadqword >> 8) & 0xff) << 0));
                    readval1 = (uint32_t)((((loadqword >> 48) & 0xff) << 24) | (((loadqword >> 32) & 0xff) << 16) |
                                          (((loadqword >> 16) & 0xff) << 8) | (((loadqword >> 0) & 0xff) << 0));

                    if (bit3fl) { // dxt word swapping(?)
                        tmem_write16(tmemidx2, readval0 >> 16);
                        tmem_write16(tmemidx3, readval0 >> 0);
                        tmem_write16(tmemidx2 | 0x400, readval1 >> 16);
                        tmem_write16(tmemidx3 | 0x400, readval1 >> 0);
                    } else {
                        tmem_write16(tmemidx0, readval0 >> 16);
                        tmem_write16(tmemidx1, readval0 >> 0);
                        tmem_write16(tmemidx0 | 0x400, readval1 >> 16);
                        tmem_write16(tmemidx1 | 0x400, readval1 >> 0);
                    }
                    break;

                case TMEM_FMT_RGBA32: // r and g are in low tmem while b and a are in high tmem
                    readval0 = (uint32_t)(((loadqword >> 48) << 16) | ((loadqword >> 16) & 0xffff));
                    readval1 = (uint32_t)((((loadqword >> 32) & 0xffff) << 16) | (loadqword & 0xffff));

                    if (bit3fl) {
                        tmem_write16(tmemidx2, loadqword >> 48);
                        tmem_write16(tmemidx3, loadqword >> 16);
                        tmem_write16(tmemidx2 | 0x400, loadqword >> 32);
                        tmem_write16(tmemidx3 | 0x400, loadqword >> 0);
                    } else {
                        tmem_write16(tmemidx0, loadqword >> 48);
                        tmem_write16(tmemidx1, loadqword >> 16);
                        tmem_write16(tmemidx0 | 0x400, loadqword >> 32);
                        tmem_write16(tmemidx1 | 0x400, loadqword >> 0);
                    }
                    break;

                case TMEM_FMT_NORMAL:
                    if (sst & 1) { // dxt word swapping(?)
                        tmem_write16(tmemidx0 | (hibit << 10), loadqword >> 16);
                        tmem_write16(tmemidx1 | (hibit << 10), loadqword >> 0);
                        tmem_write16(tmemidx2 | (hibit << 10), loadqword >> 48);
                        tmem_write16(tmemidx3 | (hibit << 10), loadqword >> 32);
                    } else {
                        tmem_write16(tmemidx0 | (hibit << 10), loadqword >> 48);
                        tmem_write16(tmemidx1 | (hibit << 10), loadqword >> 32);
                        tmem_write16(tmemidx2 | (hibit << 10), loadqword >> 16);
                        tmem_write16(tmemidx3 | (hibit << 10), loadqword >> 0);
                    }
                    break;
            }

            s = (s + dsinc) & ~0x1f;
            t = (t + dtinc) & ~0x1f;
            tiptr += tiadvance;
        }
    }
}

static void
edgewalker_for_loads(struct rdp_state *wstate, int32_t *lewdata)
{
    int cmd_id = CMD_ID(lewdata);
    int ltlut = (cmd_id == CMD_ID_LOAD_TLUT);
    int coord_quad = ltlut || (cmd_id == CMD_ID_LOAD_BLOCK);
    int tilenum = (lewdata[0] >> 16) & 7;
    wstate->max_level = 0;

    int32_t yl = SIGN(lewdata[0], 14);
    int32_t ym = SIGN(lewdata[1] >> 16, 14);
    int32_t yh = SIGN(lewdata[1], 14);

    int32_t xl = SIGN(lewdata[2], 28);
    int32_t xh = SIGN(lewdata[3], 28);
    int32_t xm = SIGN(lewdata[4], 28);

    int s = lewdata[5] & 0xffff0000;
    int t = (lewdata[5] & 0xffff) << 16;
    int dsdx = (lewdata[7] & 0xffff0000) | ((lewdata[6] >> 16) & 0xffff);
    int dtdx = ((lewdata[7] << 16) & 0xffff0000) | (lewdata[6] & 0xffff);
    int dtde = (lewdata[9] & 0xffff) << 16;

    wstate->spans_dsdx = dsdx & ~0x1f;
    wstate->spans_dtdx = dtdx & ~0x1f;
    wstate->spans_dwdx = 0;

    if (xh > xm) // uls > lrs
        return;  // always loads nothing (this includes loadblock max 2048 texels since >2048 texels is a negative
                 // amount)

    int xright = xh & ~0x1;
    int xleft = xm & ~0x1;
    int xend = xright >> 16;

    int ystart = yh & ~3;
    int yend = yl | 3;

    // These are guaranteed to be initialized before use
    int32_t maxxmx;
    int32_t minxhx;

    if (yend < ystart)
        return; // always loads nothing

    for (int ycur = ystart; ycur <= yend; ycur++) {
        if (ycur == ym)
            xleft = xl & ~1;

        int ycur_int = ycur >> 2;
        int ycur_frac = ycur & 3;

        if (!(ycur & ~0xfff)) { // TODO what is this condition?

            if (ycur_frac == 0) {
                // First subpixel line
                maxxmx = 0;
                minxhx = 0xfff;
            }

            int32_t xrsc = (xright >> 13) & 0x7ffe;
            int32_t xlsc = (xleft >> 13) & 0x7ffe;

            if (!(ycur < yh || ycur >= yl)) {
                // If line is valid, track min/max width across subpixel lines
                maxxmx = (((xlsc >> 3) & 0xfff) > maxxmx) ? (xlsc >> 3) & 0xfff : maxxmx;
                minxhx = (((xrsc >> 3) & 0xfff) < minxhx) ? (xrsc >> 3) & 0xfff : minxhx;
            }

            if (ycur_frac == 0) {
                // First subpixel line
                wstate->span[ycur_int].unscrx = xend;
                wstate->span[ycur_int].s = s & ~0x3ff;
                wstate->span[ycur_int].t = t & ~0x3ff;
            }

            if (ycur_frac == 3) {
                // Last subpixel line
                wstate->span[ycur_int].lx = maxxmx;
                wstate->span[ycur_int].rx = minxhx;
            }
        }

        if (ycur_frac == 3) {
            // Step t at last subpixel line
            t += dtde;
        }
    }

    loading_pipeline(wstate, yh >> 2, yl >> 2, tilenum, coord_quad, ltlut);
}

void
rdp_set_tile_size(struct rdp_state *wstate, const uint32_t *args)
{
    int tilenum = (args[1] >> 24) & 0x7;
    wstate->tile[tilenum].sl = (args[0] >> 12) & 0xfff;
    wstate->tile[tilenum].tl = (args[0] >> 0) & 0xfff;
    wstate->tile[tilenum].sh = (args[1] >> 12) & 0xfff;
    wstate->tile[tilenum].th = (args[1] >> 0) & 0xfff;

    calculate_clamp_diffs(&wstate->tile[tilenum]);
}

void
rdp_load_block(struct rdp_state *wstate, const uint32_t *args)
{
    int tilenum = (args[1] >> 24) & 0x7;
    uint16_t sl, sh, tl, dxt;

    wstate->tile[tilenum].sl = sl = ((args[0] >> 12) & 0xfff); // uls
    wstate->tile[tilenum].tl = tl = ((args[0] >> 0) & 0xfff);  // ult
    wstate->tile[tilenum].sh = sh = ((args[1] >> 12) & 0xfff); // lrs
    wstate->tile[tilenum].th = dxt = ((args[1] >> 0) & 0xfff); // dxt

    calculate_clamp_diffs(&wstate->tile[tilenum]);

    int tlclamped = tl & 0x3ff;

    int32_t lewdata[10];

    lewdata[0] = (args[0] & 0xff000000) | (0x10 << 19) | (tilenum << 16) | ((tlclamped << 2) | 3);
    lewdata[1] = (((tlclamped << 2) | 3) << 16) | (tlclamped << 2);
    lewdata[2] = sh << 16;
    lewdata[3] = sl << 16;
    lewdata[4] = sh << 16;
    lewdata[5] = ((sl << 3) << 16) | (tl << 3);
    lewdata[6] = (dxt & 0xff) << 8;
    lewdata[7] = ((0x80 >> wstate->ti_size) << 16) | (dxt >> 8);
    lewdata[8] = 0x20;
    lewdata[9] = 0x20;

    edgewalker_for_loads(wstate, lewdata);
}

static void
tile_tlut_common_cs_decoder(struct rdp_state *wstate, const uint32_t *args)
{
    int tilenum = (args[1] >> 24) & 0x7;
    uint16_t sl, tl, sh, th;

    wstate->tile[tilenum].sl = sl = ((args[0] >> 12) & 0xfff);
    wstate->tile[tilenum].tl = tl = ((args[0] >> 0) & 0xfff);
    wstate->tile[tilenum].sh = sh = ((args[1] >> 12) & 0xfff);
    wstate->tile[tilenum].th = th = ((args[1] >> 0) & 0xfff);

    calculate_clamp_diffs(&wstate->tile[tilenum]);

    int32_t lewdata[10];

    lewdata[0] = (args[0] & 0xff000000) | (0x10 << 19) | (tilenum << 16) | (th | 3);
    lewdata[1] = ((th | 3) << 16) | (tl);
    lewdata[2] = ((sh >> 2) << 16) | ((sh & 3) << 14);
    lewdata[3] = ((sl >> 2) << 16) | ((sl & 3) << 14);
    lewdata[4] = ((sh >> 2) << 16) | ((sh & 3) << 14);
    lewdata[5] = ((sl << 3) << 16) | (tl << 3);
    lewdata[6] = 0;
    lewdata[7] = (0x200 >> wstate->ti_size) << 16;
    lewdata[8] = 0x20;
    lewdata[9] = 0x20;

    edgewalker_for_loads(wstate, lewdata);
}

void
rdp_load_tlut(struct rdp_state *wstate, const uint32_t *args)
{
    tile_tlut_common_cs_decoder(wstate, args);
}

void
rdp_load_tile(struct rdp_state *wstate, const uint32_t *args)
{
    tile_tlut_common_cs_decoder(wstate, args);
}

void
rdp_set_tile(struct rdp_state *wstate, const uint32_t *args)
{
    int tilenum = (args[1] >> 24) & 0x7;

    wstate->tile[tilenum].format = (args[0] >> 21) & 0x7;
    wstate->tile[tilenum].size = (args[0] >> 19) & 0x3;
    wstate->tile[tilenum].line = (args[0] >> 9) & 0x1ff;
    wstate->tile[tilenum].tmem = (args[0] >> 0) & 0x1ff;
    wstate->tile[tilenum].palette = (args[1] >> 20) & 0xf;
    wstate->tile[tilenum].ct = (args[1] >> 19) & 0x1;
    wstate->tile[tilenum].mt = (args[1] >> 18) & 0x1;
    wstate->tile[tilenum].mask_t = (args[1] >> 14) & 0xf;
    wstate->tile[tilenum].shift_t = (args[1] >> 10) & 0xf;
    wstate->tile[tilenum].cs = (args[1] >> 9) & 0x1;
    wstate->tile[tilenum].ms = (args[1] >> 8) & 0x1;
    wstate->tile[tilenum].mask_s = (args[1] >> 4) & 0xf;
    wstate->tile[tilenum].shift_s = (args[1] >> 0) & 0xf;

    calculate_tile_derivs(&wstate->tile[tilenum]);
}

void
rdp_set_texture_image(struct rdp_state *wstate, const uint32_t *args)
{
    wstate->ti_format = (args[0] >> 21) & 0x7;
    wstate->ti_size = (args[0] >> 19) & 0x3;
    wstate->ti_width = (args[0] & 0x3ff) + 1;
    wstate->ti_address = args[1] & 0x0ffffff;
}

void
rdp_set_convert(struct rdp_state *wstate, const uint32_t *args)
{
    int32_t k0 = (args[0] >> 13) & 0x1ff;
    int32_t k1 = (args[0] >> 4) & 0x1ff;
    int32_t k2 = ((args[0] & 0xf) << 5) | ((args[1] >> 27) & 0x1f);
    int32_t k3 = (args[1] >> 18) & 0x1ff;
    wstate->k0_tf = (SIGN(k0, 9) << 1) + 1;
    wstate->k1_tf = (SIGN(k1, 9) << 1) + 1;
    wstate->k2_tf = (SIGN(k2, 9) << 1) + 1;
    wstate->k3_tf = (SIGN(k3, 9) << 1) + 1;
    wstate->k4 = (args[1] >> 9) & 0x1ff;
    wstate->k5 = args[1] & 0x1ff;
}

static void
tex_init_lut(void)
{
    tmem_init_lut();
    tcoord_init_lut();
}

static void
tex_init(struct rdp_state *wstate)
{
    int i;
    tcoord_init(wstate);

    for (i = 0; i < 8; i++) {
        calculate_tile_derivs(&wstate->tile[i]);
        calculate_clamp_diffs(&wstate->tile[i]);
    }
}

#endif // N64VIDEO_C
