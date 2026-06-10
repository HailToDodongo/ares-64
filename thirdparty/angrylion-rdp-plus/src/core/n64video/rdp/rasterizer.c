#ifdef N64VIDEO_C

static STRICTINLINE uint16_t
normalize_dzpix(uint16_t sum)
{
    if (sum & 0xc000)
        return 0x8000;

    if (!(sum & 0xffff))
        return 1;

    if (sum == 1)
        return 3;

    // Integer log2, finding the highest-position set bit
    for (int count = 0x2000; count > 0; count >>= 1) {
        if (sum & count)
            return count << 1;
    }
    return 0;
}

static void
replicate_for_copy(struct rdp_state *wstate, uint32_t *outbyte, uint32_t inshort, uint32_t nybbleoffset,
                   uint32_t tilenum, uint32_t tformat, uint32_t tsize)
{
    uint32_t lownib, hinib;

    switch (tsize) {
        case PIXEL_SIZE_4BIT:
            lownib = (nybbleoffset ^ 3) << 2;
            lownib = hinib = (inshort >> lownib) & 0xf;

            switch (tformat) {
                case FORMAT_CI:
                    *outbyte = (wstate->tile[tilenum].palette << 4) | lownib;
                    break;

                case FORMAT_IA:
                    lownib = (lownib << 4) | lownib;
                    *outbyte = (lownib & 0xe0) | ((lownib & 0xe0) >> 3) | ((lownib & 0xc0) >> 6);
                    break;

                default:
                    *outbyte = (lownib << 4) | lownib;
                    break;
            }
            break;

        case PIXEL_SIZE_8BIT:
            hinib = ((nybbleoffset ^ 3) | 1) << 2;

            switch (tformat) {
                case FORMAT_IA:
                    lownib = (inshort >> hinib) & 0xf;
                    *outbyte = (lownib << 4) | lownib;
                    break;

                default:
                    lownib = (inshort >> (hinib & ~4)) & 0xf;
                    hinib = (inshort >> hinib) & 0xf;
                    *outbyte = (hinib << 4) | lownib;
                    break;
            }
            break;

        default:
            *outbyte = (inshort >> 8) & 0xff;
            break;
    }
}

static void
fetch_qword_copy(struct rdp_state *wstate, uint32_t *hidword, uint32_t *lowdword, int32_t ssss, int32_t ssst,
                 uint32_t tilenum)
{
    uint32_t tformat, tsize;
    if (wstate->other_modes.en_tlut) {
        tformat = wstate->other_modes.tlut_type ? FORMAT_IA : FORMAT_RGBA;
        tsize = PIXEL_SIZE_16BIT;
    } else {
        tformat = wstate->tile[tilenum].format;
        tsize = wstate->tile[tilenum].size;
    }

    int32_t sss = ssss, sst = ssst;

    // Shift / convert relative / mask / mirror
    int32_t sss1, sss2, sss3;
    tc_pipeline_copy(&wstate->tile[tilenum], &sss, &sss1, &sss2, &sss3, &sst);

    uint32_t sortshort[8];
    int hibits[6];
    int lowbits[6];
    read_tmem_copy(wstate, sss, sss1, sss2, sss3, sst, tilenum, sortshort, hibits, lowbits);

    uint32_t shorta, shortb, shortc, shortd;
    if (wstate->other_modes.en_tlut) {
        shorta = sortshort[4];
        shortb = sortshort[5];
        shortc = sortshort[6];
        shortd = sortshort[7];
    } else if (tformat == FORMAT_YUV || (tformat == FORMAT_RGBA && tsize == PIXEL_SIZE_32BIT)) {
        shorta = sortshort[0];
        shortb = sortshort[1];
        shortc = sortshort[2];
        shortd = sortshort[3];
    } else {
        shorta = hibits[0] ? sortshort[4] : sortshort[0];
        shortb = hibits[1] ? sortshort[5] : sortshort[1];
        shortc = hibits[3] ? sortshort[6] : sortshort[2];
        shortd = hibits[4] ? sortshort[7] : sortshort[3];
    }

    *lowdword = (shortc << 16) | shortd;

    if (tsize == PIXEL_SIZE_16BIT) {
        *hidword = (shorta << 16) | shortb;
    } else {
        replicate_for_copy(wstate, &shorta, shorta, lowbits[0] & 3, tilenum, tformat, tsize);
        replicate_for_copy(wstate, &shortb, shortb, lowbits[1] & 3, tilenum, tformat, tsize);
        replicate_for_copy(wstate, &shortc, shortc, lowbits[3] & 3, tilenum, tformat, tsize);
        replicate_for_copy(wstate, &shortd, shortd, lowbits[4] & 3, tilenum, tformat, tsize);
        *hidword = (shorta << 24) | (shortb << 16) | (shortc << 8) | shortd;
    }
}

/**
 * offx: 0.2
 * offy: 0.2
 * r: 9.2
 * g: 9.2
 * b: 9.2
 * a: 9.2
 */
static STRICTINLINE void
rgba_correct(struct rdp_state *wstate, int offx, int offy, int r, int g, int b, int a, uint32_t cvg)
{
    if (cvg == 8) {
        r >>= 2;
        g >>= 2;
        b >>= 2;
        a >>= 2;
    } else {
        // 0.2 * s10.2 = s10.4
        int summand_r, summand_b, summand_g, summand_a;
        summand_r = offx * wstate->spans_cdrdx + offy * wstate->spans_drdy;
        summand_g = offx * wstate->spans_cdgdx + offy * wstate->spans_dgdy;
        summand_b = offx * wstate->spans_cdbdx + offy * wstate->spans_dbdy;
        summand_a = offx * wstate->spans_cdadx + offy * wstate->spans_dady;

        // colors coming in are 9.2 formatted, shift left by 2 to sum with 10.4 format then extract int part
        r = ((r << 2) + summand_r) >> 4;
        g = ((g << 2) + summand_g) >> 4;
        b = ((b << 2) + summand_b) >> 4;
        a = ((a << 2) + summand_a) >> 4;
    }
    // result colors are 1.8 formatted
    // clamp to 0.8, possibly wrap to 0
    wstate->shade_color.r = special_9bit_clamptable[r & 0x1ff];
    wstate->shade_color.g = special_9bit_clamptable[g & 0x1ff];
    wstate->shade_color.b = special_9bit_clamptable[b & 0x1ff];
    wstate->shade_color.a = special_9bit_clamptable[a & 0x1ff];
}

static STRICTINLINE void
z_correct(struct rdp_state *wstate, int offx /* u0.2 */, int offy /* u0.2 */, int *z /* s15.6 */,
          uint32_t cvg /* [0, 8] */)
{
    int sz = *z; // input z is s15.6 (truncated from s15.16)

    if (cvg == 8) {
        sz = sz >> 3;
    } else {
        int summand_z = offx * wstate->spans_cdzdx + offy * wstate->spans_dzdy;
        // s15.6 << 2 = s15.8
        // s15.8 >> 5 = s15.3
        sz = ((sz << 2) + summand_z) >> 5;
    }

    // Clamp s15.3 to u15.3 with a combiner-style clamp
    switch ((sz & 0x60000) >> 17) {
        case_no_default;

        case 0:
        case 1:
            *z = sz & 0x3ffff;
            break;
        case 2: // Clamp to max
            *z = 0x3ffff;
            break;
        case 3: // Clamp to 0
            *z = 0;
            break;
    }
}

static void
rejected_hbwrite_1cycle(struct rdp_state *wstate, int cdith, uint32_t blend_en, uint32_t prewrap, uint32_t curpixel,
                        uint32_t curpixel_cvg, uint32_t curpixel_memcvg, int flip, int *delayedhbwidx)
{
    int g, dontblend;
    int gval = 0;
    uint32_t fb = 0;
    int32_t hval = 0;
    int fbsel = wstate->fb_size;

    if (wstate->fb_size == PIXEL_SIZE_8BIT) {
        fb = wstate->fb_address + curpixel;
        if (!(fb & 1))
            fbsel--;
    }

    if (fbsel & 1) {
        if (!wstate->other_modes.color_on_cvg || prewrap) {
            dontblend = (wstate->other_modes.f.partialreject_1cycle && wstate->pixel_color.a >= 0xff);
            if (!blend_en || dontblend)
                g = *wstate->blender1a_g[0];
            else {
                wstate->inv_pixel_color.a = (~(*wstate->blender1b_a[0])) & 0xff;

                g = blender_equation_cycle_gval(wstate, 0);
            }
        } else
            g = *wstate->blender2a_g[0];

        if (wstate->other_modes.rgb_dither_sel != 3)
            rgb_dither_gval(wstate->other_modes.rgb_dither_sel, &g, cdith);

        gval = (g & 1) ? 3 : 0;
    }

    switch (fbsel) {
        case PIXEL_SIZE_4BIT:
            break;
        case PIXEL_SIZE_8BIT:
            if (flip && *delayedhbwidx >= 0) {
                if ((uint32_t)*delayedhbwidx < fb) {
                    if (rdram_valid_idx8((uint32_t)*delayedhbwidx)) {
                        int oldhbidx = *delayedhbwidx >> 1;
                        rdram_hidden[oldhbidx] &= ~2;
                        rdram_hidden[oldhbidx] |= rdram_hidden_old[oldhbidx & 7] & 2;
                    }
                } else if (rdram_valid_idx8(fb)) {
                    rdram_hidden[fb >> 1] &= ~2;
                    rdram_hidden[fb >> 1] |= gval & 2;
                }

                *delayedhbwidx = -1;
            }

            rdram_hidden_old[(fb >> 1) & 7] = gval & 0xff;
            break;
        case PIXEL_SIZE_16BIT:
            fb = (wstate->fb_address >> 1) + curpixel;
            if (wstate->fb_format == FORMAT_RGBA)
                hval = finalize_spanalpha(wstate->other_modes.cvg_dest, blend_en, curpixel_cvg, curpixel_memcvg) & 3;
            rdram_hidden_old[fb & 7] = hval & 0xff;
            break;
        case PIXEL_SIZE_32BIT:
            fb = (wstate->fb_address >> 2) + curpixel;
            rdram_hidden_old[(fb << 1) & 7] = gval & 0xff;
            rdram_hidden_old[((fb << 1) + 1) & 7] = 0;
            break;
    }
}

static void
rejected_hbwrite_2cycle(struct rdp_state *wstate, int cdith, uint32_t blend_en, uint32_t prewrap, uint32_t curpixel,
                        uint32_t curpixel_cvg, uint32_t curpixel_memcvg, int flip, int *delayedhbwidx)
{
    int g, dontblend;
    int gval = 0;
    uint32_t fb = 0;
    int32_t hval = 0;
    int fbsel = wstate->fb_size;

    if (wstate->fb_size == PIXEL_SIZE_8BIT) {
        fb = wstate->fb_address + curpixel;
        if (!(fb & 1))
            fbsel--;
    }

    if (fbsel & 1) {
        if (!wstate->other_modes.color_on_cvg || prewrap) {
            dontblend = (wstate->other_modes.f.partialreject_2cycle && wstate->pixel_color.a >= 0xff);
            if (!blend_en || dontblend)
                g = *wstate->blender1a_g[1];
            else {
                wstate->inv_pixel_color.a = (~(*wstate->blender1b_a[1])) & 0xff;

                g = blender_equation_cycle_gval(wstate, 1);
            }
        } else
            g = *wstate->blender2a_g[1];

        if (wstate->other_modes.rgb_dither_sel != 3)
            rgb_dither_gval(wstate->other_modes.rgb_dither_sel, &g, cdith);

        gval = (g & 1) ? 3 : 0;
    }

    switch (fbsel) {
        case PIXEL_SIZE_4BIT:
            break;
        case PIXEL_SIZE_8BIT:
            if (flip && *delayedhbwidx >= 0) {
                if ((uint32_t)*delayedhbwidx < fb) {
                    if (rdram_valid_idx8((uint32_t)*delayedhbwidx)) {
                        int oldhbidx = *delayedhbwidx >> 1;
                        rdram_hidden[oldhbidx] &= ~2;
                        rdram_hidden[oldhbidx] |= rdram_hidden_old[oldhbidx & 7] & 2;
                    }
                } else if (rdram_valid_idx8(fb)) {
                    rdram_hidden[fb >> 1] &= ~2;
                    rdram_hidden[fb >> 1] |= gval & 2;
                }

                *delayedhbwidx = -1;
            }

            rdram_hidden_old[(fb >> 1) & 7] = gval & 0xff;
            break;
        case PIXEL_SIZE_16BIT:
            fb = (wstate->fb_address >> 1) + curpixel;
            if (wstate->fb_format == FORMAT_RGBA)
                hval = finalize_spanalpha(wstate->other_modes.cvg_dest, blend_en, curpixel_cvg, curpixel_memcvg) & 3;
            rdram_hidden_old[fb & 7] = hval & 0xff;
            break;
        case PIXEL_SIZE_32BIT:
            fb = (wstate->fb_address >> 2) + curpixel;
            rdram_hidden_old[(fb << 1) & 7] = gval & 0xff;
            rdram_hidden_old[((fb << 1) + 1) & 7] = 0;
            break;
    }
}

static void
render_spans_1cycle_complete(struct rdp_state *wstate, int start, int end, int tilenum, int flip)
{
    int drinc, dginc, dbinc, dainc, dzinc, dsinc, dtinc, dwinc;
    int xinc;

    if (flip) {
        drinc = wstate->spans_drdx;
        dginc = wstate->spans_dgdx;
        dbinc = wstate->spans_dbdx;
        dainc = wstate->spans_dadx;
        dzinc = wstate->spans_dzdx;
        dsinc = wstate->spans_dsdx;
        dtinc = wstate->spans_dtdx;
        dwinc = wstate->spans_dwdx;
        xinc = 1;
    } else {
        drinc = -wstate->spans_drdx;
        dginc = -wstate->spans_dgdx;
        dbinc = -wstate->spans_dbdx;
        dainc = -wstate->spans_dadx;
        dzinc = -wstate->spans_dzdx;
        dsinc = -wstate->spans_dsdx;
        dtinc = -wstate->spans_dtdx;
        dwinc = -wstate->spans_dwdx;
        xinc = -1;
    }

    // Select source of dz

    uint16_t dzpix;
    if (wstate->other_modes.z_source_sel == Z_SRC_PIXEL)
        dzpix = wstate->spans_dzpix;
    else {
        dzpix = wstate->primitive_delta_z;
        dzinc = wstate->spans_cdzdx = wstate->spans_dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int prim_tile = tilenum;
    int tile1 = tilenum;
    int newtile = tilenum;

    struct spansigs sigs;

    int cdith = 7, adith = 0;
    int delayedhbwidx = -1;
    int zb = wstate->zb_address >> 1;

    int32_t prelodfrac = 0; // Note this is not used until it is first set

    for (int ycur = start; ycur <= end; ycur++) {
        if (!wstate->span[ycur].validline)
            continue;

        int xstart = wstate->span[ycur].lx;
        int xend = wstate->span[ycur].unscrx;
        int xendsc = wstate->span[ycur].rx;
        int r = wstate->span[ycur].r;
        int g = wstate->span[ycur].g;
        int b = wstate->span[ycur].b;
        int a = wstate->span[ycur].a;
        // z is s15.16, prim z has fractional part all 0
        int z = (wstate->other_modes.z_source_sel == Z_SRC_PIXEL) ? wstate->span[ycur].z : wstate->primitive_z;
        int s = wstate->span[ycur].s;
        int t = wstate->span[ycur].t;
        int w = wstate->span[ycur].w;

        /* xstart, xend, xendsc, r, g, b, a, z, s, t, w */

        int x = xendsc;
        int curpixel = wstate->fb_width * ycur + x;
        int zbcur = zb + curpixel;

        int length, scdiff;
        if (!flip) {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(wstate, ycur);
        } else {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(wstate, ycur);
        }

        /* xstart, xend, xendsc, r, g, b, a, z, s, t, w, cvg */

        if (scdiff) {
            scdiff &= 0xfff;
            r += drinc * scdiff;
            g += dginc * scdiff;
            b += dbinc * scdiff;
            a += dainc * scdiff;
            z += dzinc * scdiff;
            s += dsinc * scdiff;
            t += dtinc * scdiff;
            w += dwinc * scdiff;
        }

        // !flip => xend - xstart
        //  flip => xstart - xend
        int lodlength = length + scdiff;

        // "long spans" are those larger than 8 pixels
        sigs.longspan = lodlength > 7;
        sigs.midspan = lodlength == 7;
        sigs.onelessthanmid = lodlength == 6;

        for (int j = 0; j <= length; j++) {
            int sr = r >> 14; // 9.2 format
            int sg = g >> 14;
            int sb = b >> 14;
            int sa = a >> 14;
            int ss = s >> 16;
            int st = t >> 16;
            int sw = w >> 16;
            int sz = (z >> 10) & 0x3fffff; // 22 bits = 21 bits + 1 sign bit = s15.6

            sigs.endspan = j == length;
            sigs.preendspan = j == (length - 1);

            // coverage derived quantities
            uint8_t offx, offy; // both 0.2 formatted
            uint32_t curpixel_cvg, curpixel_cvbit;
            lookup_cvmask_derivatives(wstate->cvgbuf[x], &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

            // texture perspective correction for TEXEL1
            int news, newt;
            get_texel1_1cycle(wstate, &news, &newt, s, t, w, dsinc, dtinc, dwinc, ycur, &sigs);

            if (j != 0) {
                // shuffle TEXEL1 into TEXEL0
                wstate->texel0_color = wstate->texel1_color;
                // lod frac was calculated for next pixel already, shuffle it in
                wstate->lod_frac = prelodfrac;
            } else {
                // texture pipeline first cycling, HW picks up from last primitive
                int sss, sst;
                // texture perspective correction for TEXEL0
                wstate->tcdiv_ptr(ss, st, sw, &sss, &sst);
                // LOD for TEXEL0, updates wstate->lod_frac
                tclod_1cycle_current(wstate, &sss, &sst, news, newt, s, t, w, dsinc, dtinc, dwinc, ycur, prim_tile,
                                     &tile1, &sigs);
                // texture sampling + filtering for TEXEL0
                texture_pipeline_cycle(wstate, &wstate->texel0_color, &wstate->texel0_color, sss, sst, tile1, 0);
            }

            sigs.nextspan = sigs.endspan;
            sigs.endspan = sigs.preendspan;
            sigs.preendspan = j == (length - 2);

            // next pixel s/t/w
            s += dsinc;
            t += dtinc;
            w += dwinc;

            // LOD for TEXEL1
            tclod_1cycle_next(wstate, &news, &newt, s, t, w, dsinc, dtinc, dwinc, ycur, prim_tile, &newtile, &sigs,
                              &prelodfrac);
            // texture sampling + filtering for TEXEL1
            texture_pipeline_cycle(wstate, &wstate->texel1_color, &wstate->texel1_color, news, newt, newtile, 0);
            // the fact that TEXEL1 is next pixel TEXEL0 in 1-cycle mode proves pipelining, the next pixel has passed
            // through the texture pipeline before the current pixel reaches CC

            // subpixel correction
            rgba_correct(wstate, offx, offy, sr, sg, sb, sa, curpixel_cvg);
            z_correct(wstate, offx, offy, &sz, curpixel_cvg);
            // sz is now u15.3

            // dither noise
            if (wstate->other_modes.f.getditherlevel != DITHER_LEVEL_UNUSED)
                get_dither_noise(wstate, x, ycur, &cdith, &adith);

            // combiner
            combiner_finalstage(wstate, adith, &curpixel_cvg);

            // image read
            uint32_t curpixel_memcvg;
            wstate->fbread1_ptr(wstate, curpixel, &curpixel_memcvg);

            // depth compare
            uint32_t blend_en;
            uint32_t prewrap;
            int wen =
                z_compare(wstate, zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg);

            // alpha compare
            if (wen)
                wen = alpha_compare(wstate, wstate->pixel_color.a);

            // coverage rejection
            // z_compare modifies curpixel_cvg in interpenetrating mode but does not adjust curpixel_cvbit ?? CHECK this
            if (wen)
                wen = wstate->other_modes.antialias_en ? curpixel_cvg : curpixel_cvbit;

            // blender + image write
            if (wen) {
                uint32_t fir, fig, fib;
                blender_finalstage(wstate, &fir, &fig, &fib, cdith, blend_en, prewrap,
                                   wstate->other_modes.f.partialreject_1cycle, 0);
                wstate->fbwrite_ptr(wstate, curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg, flip,
                                    &delayedhbwidx);
                if (wstate->other_modes.z_update_en)
                    z_store(zbcur, sz, dzpixenc);
            } else if (ycur >= wstate->last_overwriting_scanline) {
                // at or below the last valid scanline that uses more than 1 span allocation
                // accumulate rejected hidden bits, 8-bit fbs use this for hidden bit writes
                // since they don't have meaningful hidden bits
                rejected_hbwrite_1cycle(wstate, cdith, blend_en, prewrap, curpixel, curpixel_cvg, curpixel_memcvg, flip,
                                        &delayedhbwidx);
            }

            // advance to next span
            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            z += dzinc;
            x += xinc;
            curpixel += xinc;
            zbcur += xinc;
        }
    }

    if (delayedhbwidx >= 0 && flip && wstate->fb_size == PIXEL_SIZE_8BIT)
        rdram_complete_delayed_hbwrites(delayedhbwidx);
}

#if 0
static void
render_spans_1cycle_notexel1(struct rdp_state *wstate, int start, int end, int tilenum, int flip)
{
    int zb = wstate->zb_address >> 1;
    int zbcur;
    uint8_t offx = 0;
    uint8_t offy = 0;
    struct spansigs sigs;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;

    int prim_tile = tilenum;
    int tile1 = tilenum;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc, dsinc, dtinc, dwinc;
    int xinc;
    if (flip) {
        drinc = wstate->spans_dr;
        dginc = wstate->spans_dg;
        dbinc = wstate->spans_db;
        dainc = wstate->spans_da;
        dzinc = wstate->spans_dz;
        dsinc = wstate->spans_ds;
        dtinc = wstate->spans_dt;
        dwinc = wstate->spans_dw;
        xinc = 1;
    } else {
        drinc = -wstate->spans_dr;
        dginc = -wstate->spans_dg;
        dbinc = -wstate->spans_db;
        dainc = -wstate->spans_da;
        dzinc = -wstate->spans_dz;
        dsinc = -wstate->spans_ds;
        dtinc = -wstate->spans_dt;
        dwinc = -wstate->spans_dw;
        xinc = -1;
    }

    int dzpix;
    if (!wstate->other_modes.z_source_sel)
        dzpix = wstate->spans_dzpix;
    else {
        dzpix = wstate->primitive_delta_z;
        dzinc = wstate->spans_cdz = wstate->spans_dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z, s, t, w;
    int sr, sg, sb, sa, sz, ss, st, sw;
    int xstart, xend, xendsc;
    int sss = 0, sst = 0;
    int curpixel = 0;
    int x, length, scdiff, lodlength;
    uint32_t fir = 0, fig = 0, fib = 0;
    int delayedhbwidx = -1;
    int wen;

    for (i = start; i <= end; i++) {
        if (wstate->span[i].validline) {

            xstart = wstate->span[i].lx;
            xend = wstate->span[i].unscrx;
            xendsc = wstate->span[i].rx;
            r = wstate->span[i].r;
            g = wstate->span[i].g;
            b = wstate->span[i].b;
            a = wstate->span[i].a;
            z = wstate->other_modes.z_source_sel ? wstate->primitive_z : wstate->span[i].z;
            s = wstate->span[i].s;
            t = wstate->span[i].t;
            w = wstate->span[i].w;

            x = xendsc;
            curpixel = wstate->fb_width * i + x;
            zbcur = zb + curpixel;

            if (!flip) {
                length = xendsc - xstart;
                scdiff = xend - xendsc;
                compute_cvg_noflip(wstate, i);
            } else {
                length = xstart - xendsc;
                scdiff = xendsc - xend;
                compute_cvg_flip(wstate, i);
            }

            if (scdiff) {
                scdiff &= 0xfff;
                r += (drinc * scdiff);
                g += (dginc * scdiff);
                b += (dbinc * scdiff);
                a += (dainc * scdiff);
                z += (dzinc * scdiff);
                s += (dsinc * scdiff);
                t += (dtinc * scdiff);
                w += (dwinc * scdiff);
            }

            lodlength = length + scdiff;

            sigs.longspan = (lodlength > 7);
            sigs.midspan = (lodlength == 7);

            for (j = 0; j <= length; j++) {
                sr = r >> 14;
                sg = g >> 14;
                sb = b >> 14;
                sa = a >> 14;
                ss = s >> 16;
                st = t >> 16;
                sw = w >> 16;
                sz = (z >> 10) & 0x3fffff;

                sigs.endspan = (j == length);
                sigs.preendspan = (j == (length - 1));

                lookup_cvmask_derivatives(wstate->cvgbuf[x], &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

                wstate->tcdiv_ptr(ss, st, sw, &sss, &sst);

                tclod_1cycle_current_simple(wstate, &sss, &sst, s, t, w, dsinc, dtinc, dwinc, i, prim_tile, &tile1,
                                            &sigs);

                texture_pipeline_cycle(wstate, &wstate->texel0_color, &wstate->texel0_color, sss, sst, tile1, 0);

                rgba_correct(wstate, offx, offy, sr, sg, sb, sa, curpixel_cvg);
                z_correct(wstate, offx, offy, &sz, curpixel_cvg);

                if (wstate->other_modes.f.getditherlevel != DITHER_LEVEL_UNUSED)
                    get_dither_noise(wstate, x, i, &cdith, &adith);

                combiner_finalstage(wstate, adith, &curpixel_cvg);

                wstate->fbread1_ptr(wstate, curpixel, &curpixel_memcvg);

                wen = z_compare(wstate, zbcur, sz, (uint16_t)dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg,
                                curpixel_memcvg);

                if (wen)
                    wen = alpha_compare(wstate, wstate->pixel_color.a);

                if (wen)
                    wen = wstate->other_modes.antialias_en ? curpixel_cvg : curpixel_cvbit;

                if (wen) {
                    blender_finalstage(wstate, &fir, &fig, &fib, cdith, blend_en, prewrap,
                                       wstate->other_modes.f.partialreject_1cycle, 0);
                    wstate->fbwrite_ptr(wstate, curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg, flip,
                                        &delayedhbwidx);
                    if (wstate->other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                } else if (i >= wstate->last_overwriting_scanline)
                    rejected_hbwrite_1cycle(wstate, cdith, blend_en, prewrap, curpixel, curpixel_cvg, curpixel_memcvg,
                                            flip, &delayedhbwidx);

                s += dsinc;
                t += dtinc;
                w += dwinc;
                r += drinc;
                g += dginc;
                b += dbinc;
                a += dainc;
                z += dzinc;

                x += xinc;
                curpixel += xinc;
                zbcur += xinc;
            }
        }
    }

    if (delayedhbwidx >= 0 && flip && wstate->fb_size == PIXEL_SIZE_8BIT)
        rdram_complete_delayed_hbwrites(delayedhbwidx);
}
#endif

#if 0
static void
render_spans_1cycle_notex(struct rdp_state *wstate, int start, int end, int tilenum, int flip)
{
    UNUSED(tilenum);

    int zb = wstate->zb_address >> 1;
    int zbcur;
    uint8_t offx = 0;
    uint8_t offy = 0;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc;
    int xinc;

    if (flip) {
        drinc = wstate->spans_dr;
        dginc = wstate->spans_dg;
        dbinc = wstate->spans_db;
        dainc = wstate->spans_da;
        dzinc = wstate->spans_dz;
        xinc = 1;
    } else {
        drinc = -wstate->spans_dr;
        dginc = -wstate->spans_dg;
        dbinc = -wstate->spans_db;
        dainc = -wstate->spans_da;
        dzinc = -wstate->spans_dz;
        xinc = -1;
    }

    int dzpix;
    if (!wstate->other_modes.z_source_sel)
        dzpix = wstate->spans_dzpix;
    else {
        dzpix = wstate->primitive_delta_z;
        dzinc = wstate->spans_cdz = wstate->spans_dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z;
    int sr, sg, sb, sa, sz;
    int xstart, xend, xendsc;
    int curpixel = 0;
    int x, length, scdiff;
    uint32_t fir = 0, fig = 0, fib = 0;
    int delayedhbwidx = -1;
    int wen;

    for (i = start; i <= end; i++) {
        if (wstate->span[i].validline) {

            xstart = wstate->span[i].lx;
            xend = wstate->span[i].unscrx;
            xendsc = wstate->span[i].rx;
            r = wstate->span[i].r;
            g = wstate->span[i].g;
            b = wstate->span[i].b;
            a = wstate->span[i].a;
            z = wstate->other_modes.z_source_sel ? wstate->primitive_z : wstate->span[i].z;

            x = xendsc;
            curpixel = wstate->fb_width * i + x;
            zbcur = zb + curpixel;

            if (!flip) {
                length = xendsc - xstart;
                scdiff = xend - xendsc;
                compute_cvg_noflip(wstate, i);
            } else {
                length = xstart - xendsc;
                scdiff = xendsc - xend;
                compute_cvg_flip(wstate, i);
            }

            if (scdiff) {
                scdiff &= 0xfff;
                r += (drinc * scdiff);
                g += (dginc * scdiff);
                b += (dbinc * scdiff);
                a += (dainc * scdiff);
                z += (dzinc * scdiff);
            }

            for (j = 0; j <= length; j++) {
                sr = r >> 14;
                sg = g >> 14;
                sb = b >> 14;
                sa = a >> 14;
                sz = (z >> 10) & 0x3fffff;

                lookup_cvmask_derivatives(wstate->cvgbuf[x], &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

                rgba_correct(wstate, offx, offy, sr, sg, sb, sa, curpixel_cvg);
                z_correct(wstate, offx, offy, &sz, curpixel_cvg);

                if (wstate->other_modes.f.getditherlevel != DITHER_LEVEL_UNUSED)
                    get_dither_noise(wstate, x, i, &cdith, &adith);

                combiner_finalstage(wstate, adith, &curpixel_cvg);

                wstate->fbread1_ptr(wstate, curpixel, &curpixel_memcvg);

                wen = z_compare(wstate, zbcur, sz, (uint16_t)dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg,
                                curpixel_memcvg);

                if (wen)
                    wen = alpha_compare(wstate, wstate->pixel_color.a);

                if (wen)
                    wen = wstate->other_modes.antialias_en ? curpixel_cvg : curpixel_cvbit;

                if (wen) {
                    blender_finalstage(wstate, &fir, &fig, &fib, cdith, blend_en, prewrap,
                                       wstate->other_modes.f.partialreject_1cycle, 0);
                    wstate->fbwrite_ptr(wstate, curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg, flip,
                                        &delayedhbwidx);
                    if (wstate->other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                } else if (i >= wstate->last_overwriting_scanline)
                    rejected_hbwrite_1cycle(wstate, cdith, blend_en, prewrap, curpixel, curpixel_cvg, curpixel_memcvg,
                                            flip, &delayedhbwidx);

                r += drinc;
                g += dginc;
                b += dbinc;
                a += dainc;
                z += dzinc;

                x += xinc;
                curpixel += xinc;
                zbcur += xinc;
            }
        }
    }

    if (delayedhbwidx >= 0 && flip && wstate->fb_size == PIXEL_SIZE_8BIT)
        rdram_complete_delayed_hbwrites(delayedhbwidx);
}
#endif

static void
render_spans_2cycle_complete(struct rdp_state *wstate, int start, int end, int tilenum, int flip)
{
    int drinc, dginc, dbinc, dainc, dzinc, dsinc, dtinc, dwinc;
    int xinc;

    if (flip) {
        drinc = wstate->spans_drdx;
        dginc = wstate->spans_dgdx;
        dbinc = wstate->spans_dbdx;
        dainc = wstate->spans_dadx;
        dzinc = wstate->spans_dzdx;
        dsinc = wstate->spans_dsdx;
        dtinc = wstate->spans_dtdx;
        dwinc = wstate->spans_dwdx;
        xinc = 1;
    } else {
        drinc = -wstate->spans_drdx;
        dginc = -wstate->spans_dgdx;
        dbinc = -wstate->spans_dbdx;
        dainc = -wstate->spans_dadx;
        dzinc = -wstate->spans_dzdx;
        dsinc = -wstate->spans_dsdx;
        dtinc = -wstate->spans_dtdx;
        dwinc = -wstate->spans_dwdx;
        xinc = -1;
    }

    // Select source of dz

    uint16_t dzpix;
    if (wstate->other_modes.z_source_sel == Z_SRC_PIXEL) {
        dzpix = wstate->spans_dzpix;
    } else {
        dzpix = wstate->primitive_delta_z;
        dzinc = wstate->spans_cdzdx = wstate->spans_dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int zb = wstate->zb_address >> 1;
    int cdith = 7, adith = 0;

    int prim_tile = tilenum;
    int tile1 = tilenum;
    int tile2 = (tilenum + 1) & 7;
    int tile3 = tilenum;

    int delayedhbwidx = -1;

    for (int ycur = start; ycur <= end; ycur++) {
        if (!wstate->span[ycur].validline)
            continue;

        int xstart = wstate->span[ycur].lx;
        int xend = wstate->span[ycur].unscrx;
        int xendsc = wstate->span[ycur].rx;
        int r = wstate->span[ycur].r;
        int g = wstate->span[ycur].g;
        int b = wstate->span[ycur].b;
        int a = wstate->span[ycur].a;
        // prim z is 15.16 so span z is too
        int z = (wstate->other_modes.z_source_sel == Z_SRC_PIXEL) ? wstate->span[ycur].z : wstate->primitive_z;
        int s = wstate->span[ycur].s;
        int t = wstate->span[ycur].t;
        int w = wstate->span[ycur].w;

        int x = xendsc;
        uint32_t curpixel = wstate->fb_width * ycur + x;
        int zbcur = zb + curpixel;

        int length, scdiff;
        if (!flip) {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(wstate, ycur);
        } else {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(wstate, ycur);
        }

        if (scdiff) {
            scdiff &= 0xfff;
            r += drinc * scdiff;
            g += dginc * scdiff;
            b += dbinc * scdiff;
            a += dainc * scdiff;
            z += dzinc * scdiff;
            s += dsinc * scdiff;
            t += dtinc * scdiff;
            w += dwinc * scdiff;
        }

        int lodlength = length + scdiff;

        int sr = r >> 14;
        int sg = g >> 14;
        int sb = b >> 14;
        int sa = a >> 14;
        int ss = s >> 16;
        int st = t >> 16;
        int sw = w >> 16;

        int sss, sst;
        wstate->tcdiv_ptr(ss, st, sw, &sss, &sst);

        tclod_2cycle(wstate, &sss, &sst, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1, &tile2, &wstate->lod_frac);

        texture_pipeline_cycle(wstate, &wstate->texel0_color, &wstate->texel0_color, sss, sst, tile1, 0);
        texture_pipeline_cycle(wstate, &wstate->texel1_color, &wstate->texel0_color, sss, sst, tile2, 1);

        // Coverage derived quantities
        uint8_t offx;
        uint8_t offy;
        uint32_t curpixel_cvg, curpixel_cvbit;
        lookup_cvmask_derivatives(wstate->cvgbuf[x], &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

        // RGBA subpixel correction and shade alpha calculation
        rgba_correct(wstate, offx, offy, sr, sg, sb, sa, curpixel_cvg);

        // Dither noise
        if (wstate->other_modes.f.getditherlevel != DITHER_LEVEL_UNUSED)
            get_dither_noise(wstate, x, ycur, &cdith, &adith);

        // First CC cycle
        // UB: Combined color in first cycle reads prev pixel combined color
        // NB: angrylion says on https://sourceforge.net/p/angrylions-stuff/tickets/10/ that the combiner will emit a
        // new "combined color" output on every RDP cycle, even on stalled cycles? This is true, verified on hw
        uint32_t acalpha;
        combiner_2cycle_cycle0(wstate, adith, curpixel_cvg, &acalpha);

        for (int j = 0; j <= length; j++) {
            // dsinc is s15.16 so s must be too?
            s += dsinc;
            t += dtinc;
            w += dwinc;
            // integer part of s?
            ss = s >> 16;
            st = t >> 16;
            sw = w >> 16;
            int sz = (z >> 10) & 0x3fffff; // z is s15.16 so sz is s15.6

            // Texture perspective correction (next pixel)
            // (sss, sst) are 17-bit
            wstate->tcdiv_ptr(ss, st, sw, &sss, &sst);

            // Texture pipelining for next pixel
            int32_t prelodfrac;
            struct color nexttexel1_color;
            if (j < length || !wstate->span[ycur + 1].validline || lodlength < 3) {
                // Same span or next span is invalid
                tclod_2cycle(wstate, &sss, &sst, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1, &tile2, &prelodfrac);

                texture_pipeline_cycle(wstate, &wstate->nexttexel_color, &wstate->nexttexel_color, sss, sst, tile1, 0);
                texture_pipeline_cycle(wstate, &nexttexel1_color, &wstate->nexttexel_color, sss, sst, tile2, 1);
            } else {
                // Next span
                int sss2, sst2;

                ss = wstate->span[ycur + 1].s >> 16;
                st = wstate->span[ycur + 1].t >> 16;
                sw = wstate->span[ycur + 1].w >> 16;
                wstate->tcdiv_ptr(ss, st, sw, &sss2, &sst2);

                tclod_2cycle_next(wstate, &sss, &sst, &sss2, &sst2, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1,
                                  &tile3, &prelodfrac, ycur);

                texture_pipeline_cycle(wstate, &wstate->nexttexel_color, &wstate->nexttexel_color, sss, sst, tile1, 0);
                texture_pipeline_cycle(wstate, &nexttexel1_color, &wstate->nexttexel_color, sss2, sst2, tile3, 0);
            }

            // Depth subpixel correction
            z_correct(wstate, offx, offy, &sz, curpixel_cvg);

            // Advance texture pipeline
            wstate->texel0_color = wstate->texel1_color;
            wstate->texel1_color = wstate->nexttexel_color;

            // Second CC cycle
            //! HW BUG: Texture pipeline has already been cycled so TEXEL0 -> TEXEL1 and TEXEL1 -> TEXEL0 of next pixel
            // TODO what about LOD frac? LOD frac in second cycle might also be next pixel? Use of prelodfrac above
            // suggests maybe not?
            combiner_finalstage(wstate, adith, &curpixel_cvg);

            // Image read, blender inputs are not yet updated but zbuffer inputs are? TODO try check hw somehow
            uint32_t curpixel_memcvg;
            wstate->fbread2_ptr(wstate, curpixel, &curpixel_memcvg);

            // Depth compare
            uint32_t blend_en;
            uint32_t prewrap;
            int wen =
                z_compare(wstate, zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg);

            // Coverage rejection
            if (wen)
                //! weird behavior here: curpixel_cvg can be updated by cvg_x_alpha but curpixel_cvbit
                //! is NOT updated by it (verified on hardware)
                wen = wstate->other_modes.antialias_en ? curpixel_cvg : curpixel_cvbit;

            // First blender cycle
            //! HW BUG: memory color and memory coverage are sourced from previous pixel
            if (wen)
                blender_2cycle_cycle0(wstate);

            // Update specifically the green component in some edge case?
            if (!wen && ycur >= wstate->last_overwriting_scanline)
                blender_2cycle_cycle0_gval(wstate, curpixel);

            // Update blender inputs for memory color (and should also do memory cvg here)
            wstate->memory_color = wstate->pre_memory_color;

            // Advance attributes to next pixel
            x += xinc;
            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            sr = r >> 14;
            sg = g >> 14;
            sb = b >> 14;
            sa = a >> 14;

            // Coverage mask derived quantities
            uint32_t nextpixel_cvg;
            lookup_cvmask_derivatives(j < length ? wstate->cvgbuf[x] : 0, &offx, &offy, &nextpixel_cvg,
                                      &curpixel_cvbit);

            // RGBA correction and shade alpha for next pixel
            rgba_correct(wstate, offx, offy, sr, sg, sb, sa, nextpixel_cvg);

            // Cycle in lod fraction
            wstate->lod_frac = prelodfrac;
            // Cycle TEXEL0 and TEXEL1
            wstate->texel0_color = wstate->nexttexel_color;
            wstate->texel1_color = nexttexel1_color;

            // CC first cycle for next pixel
            combiner_2cycle_cycle0(wstate, adith, nextpixel_cvg, &acalpha);

            // Alpha compare
            //! HW BUG: Uses result of next pixel CC first cycle
            if (wen)
                wen = alpha_compare(wstate, acalpha);

            // Blender second cycle of current pixel, image write
            //! HW BUG: shade alpha was already cycled in for the next pixel, shade alpha in second blender cycle reads
            //! next pixel
            if (wen) {
                uint32_t fir, fig, fib;
                blender_finalstage(wstate, &fir, &fig, &fib, cdith, blend_en, prewrap,
                                   wstate->other_modes.f.partialreject_2cycle, 1);
                wstate->fbwrite_ptr(wstate, curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg, flip,
                                    &delayedhbwidx);
                if (wstate->other_modes.z_update_en)
                    z_store(zbcur, sz, dzpixenc);
            } else if (ycur >= wstate->last_overwriting_scanline) {
                // Do weird hidden bit writes
                rejected_hbwrite_2cycle(wstate, cdith, blend_en, prewrap, curpixel, curpixel_cvg, curpixel_memcvg, flip,
                                        &delayedhbwidx);
            }

            // Cycle dither noise
            if (wstate->other_modes.f.getditherlevel != DITHER_LEVEL_UNUSED)
                get_dither_noise(wstate, x, ycur, &cdith, &adith);

            // Next pixel becomes current pixel
            curpixel_cvg = nextpixel_cvg;

            // Advance pos and z
            curpixel += xinc;
            z += dzinc;
            zbcur += xinc;
        }
    }

    if (delayedhbwidx >= 0 && flip && wstate->fb_size == PIXEL_SIZE_8BIT)
        rdram_complete_delayed_hbwrites(delayedhbwidx);
}

#if 0
static void
render_spans_2cycle_notexelnext(struct rdp_state *wstate, int start, int end, int tilenum, int flip)
{
    int zb = wstate->zb_address >> 1;
    int zbcur;
    uint8_t offx = 0;
    uint8_t offy = 0;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg = 0, curpixel_cvbit = 0, curpixel_memcvg = 0;
    uint32_t nextpixel_cvg;
    uint32_t acalpha;

    int tile2 = (tilenum + 1) & 7;
    int tile1 = tilenum;
    int prim_tile = tilenum;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc, dsinc, dtinc, dwinc;
    int xinc;
    if (flip) {
        drinc = wstate->spans_dr;
        dginc = wstate->spans_dg;
        dbinc = wstate->spans_db;
        dainc = wstate->spans_da;
        dzinc = wstate->spans_dz;
        dsinc = wstate->spans_ds;
        dtinc = wstate->spans_dt;
        dwinc = wstate->spans_dw;
        xinc = 1;
    } else {
        drinc = -wstate->spans_dr;
        dginc = -wstate->spans_dg;
        dbinc = -wstate->spans_db;
        dainc = -wstate->spans_da;
        dzinc = -wstate->spans_dz;
        dsinc = -wstate->spans_ds;
        dtinc = -wstate->spans_dt;
        dwinc = -wstate->spans_dw;
        xinc = -1;
    }

    int dzpix;
    if (!wstate->other_modes.z_source_sel)
        dzpix = wstate->spans_dzpix;
    else {
        dzpix = wstate->primitive_delta_z;
        dzinc = wstate->spans_cdz = wstate->spans_dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;

    int r, g, b, a, z, s, t, w;
    int sr, sg, sb, sa, sz, ss, st, sw;
    int xstart, xend, xendsc;
    int sss = 0, sst = 0;
    int curpixel = 0;
    int wen;

    int x, length, scdiff;
    uint32_t fir, fig, fib;
    int delayedhbwidx = -1;

    for (i = start; i <= end; i++) {
        if (wstate->span[i].validline) {

            xstart = wstate->span[i].lx;
            xend = wstate->span[i].unscrx;
            xendsc = wstate->span[i].rx;
            r = wstate->span[i].r;
            g = wstate->span[i].g;
            b = wstate->span[i].b;
            a = wstate->span[i].a;
            z = wstate->other_modes.z_source_sel ? wstate->primitive_z : wstate->span[i].z;
            s = wstate->span[i].s;
            t = wstate->span[i].t;
            w = wstate->span[i].w;

            x = xendsc;
            curpixel = wstate->fb_width * i + x;
            zbcur = zb + curpixel;

            if (!flip) {
                length = xendsc - xstart;
                scdiff = xend - xendsc;
                compute_cvg_noflip(wstate, i);
            } else {
                length = xstart - xendsc;
                scdiff = xendsc - xend;
                compute_cvg_flip(wstate, i);
            }

            if (scdiff) {
                scdiff &= 0xfff;
                r += (drinc * scdiff);
                g += (dginc * scdiff);
                b += (dbinc * scdiff);
                a += (dainc * scdiff);
                z += (dzinc * scdiff);
                s += (dsinc * scdiff);
                t += (dtinc * scdiff);
                w += (dwinc * scdiff);
            }

            for (j = 0; j <= length; j++) {
                sz = (z >> 10) & 0x3fffff;

                if (!j) {
                    sr = r >> 14;
                    sg = g >> 14;
                    sb = b >> 14;
                    sa = a >> 14;
                    ss = s >> 16;
                    st = t >> 16;
                    sw = w >> 16;

                    wstate->tcdiv_ptr(ss, st, sw, &sss, &sst);

                    tclod_2cycle(wstate, &sss, &sst, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1, &tile2,
                                 &wstate->lod_frac);

                    texture_pipeline_cycle(wstate, &wstate->texel0_color, &wstate->texel0_color, sss, sst, tile1, 0);
                    texture_pipeline_cycle(wstate, &wstate->texel1_color, &wstate->texel0_color, sss, sst, tile2, 1);

                    lookup_cvmask_derivatives(wstate->cvgbuf[x], &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

                    rgba_correct(wstate, offx, offy, sr, sg, sb, sa, curpixel_cvg);

                    if (wstate->other_modes.f.getditherlevel != DITHER_LEVEL_UNUSED)
                        get_dither_noise(wstate, x, i, &cdith, &adith);

                    combiner_2cycle_cycle0(wstate, adith, curpixel_cvg, &acalpha);
                }

                z_correct(wstate, offx, offy, &sz, curpixel_cvg);

                // Advance texture pipeline
                wstate->texel0_color = wstate->texel1_color;
                wstate->texel1_color = wstate->nexttexel_color;
                combiner_finalstage(wstate, adith, &curpixel_cvg);

                wstate->fbread2_ptr(wstate, curpixel, &curpixel_memcvg);

                wen = z_compare(wstate, zbcur, sz, (uint16_t)dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg,
                                curpixel_memcvg);

                if (wen)
                    wen = wstate->other_modes.antialias_en ? curpixel_cvg : curpixel_cvbit;

                if (wen)
                    blender_2cycle_cycle0(wstate);

                if (!wen && i >= wstate->last_overwriting_scanline)
                    blender_2cycle_cycle0_gval(wstate, curpixel);

                wstate->memory_color = wstate->pre_memory_color;

                x += xinc;

                r += drinc;
                g += dginc;
                b += dbinc;
                a += dainc;
                s += dsinc;
                t += dtinc;
                w += dwinc;

                sr = r >> 14;
                sg = g >> 14;
                sb = b >> 14;
                sa = a >> 14;
                ss = s >> 16;
                st = t >> 16;
                sw = w >> 16;

                lookup_cvmask_derivatives(j < length ? wstate->cvgbuf[x] : 0, &offx, &offy, &nextpixel_cvg,
                                          &curpixel_cvbit);

                rgba_correct(wstate, offx, offy, sr, sg, sb, sa, nextpixel_cvg);

                wstate->tcdiv_ptr(ss, st, sw, &sss, &sst);

                tclod_2cycle(wstate, &sss, &sst, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1, &tile2,
                             &wstate->lod_frac);

                texture_pipeline_cycle(wstate, &wstate->texel0_color, &wstate->texel0_color, sss, sst, tile1, 0);
                texture_pipeline_cycle(wstate, &wstate->texel1_color, &wstate->texel0_color, sss, sst, tile2, 1);

                combiner_2cycle_cycle0(wstate, adith, nextpixel_cvg, &acalpha);

                if (wen)
                    wen = alpha_compare(wstate, acalpha);

                if (wen) {
                    blender_finalstage(wstate, &fir, &fig, &fib, cdith, blend_en, prewrap,
                                       wstate->other_modes.f.partialreject_2cycle, 1);
                    wstate->fbwrite_ptr(wstate, curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg, flip,
                                        &delayedhbwidx);
                    if (wstate->other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                } else if (i >= wstate->last_overwriting_scanline)
                    rejected_hbwrite_2cycle(wstate, cdith, blend_en, prewrap, curpixel, curpixel_cvg, curpixel_memcvg,
                                            flip, &delayedhbwidx);

                if (wstate->other_modes.f.getditherlevel != DITHER_LEVEL_UNUSED)
                    get_dither_noise(wstate, x, i, &cdith, &adith);

                curpixel_cvg = nextpixel_cvg;

                z += dzinc;

                curpixel += xinc;
                zbcur += xinc;
            }
        }
    }

    if (delayedhbwidx >= 0 && flip && wstate->fb_size == PIXEL_SIZE_8BIT)
        rdram_complete_delayed_hbwrites(delayedhbwidx);
}
#endif

#if 0
static void
render_spans_2cycle_notexel1(struct rdp_state *wstate, int start, int end, int tilenum, int flip)
{
    int zb = wstate->zb_address >> 1;
    int zbcur;
    uint8_t offx = 0;
    uint8_t offy = 0;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg = 0, curpixel_cvbit = 0, curpixel_memcvg = 0;
    uint32_t nextpixel_cvg;
    uint32_t acalpha;

    int tile1 = tilenum;
    int prim_tile = tilenum;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc, dsinc, dtinc, dwinc;
    int xinc;
    if (flip) {
        drinc = wstate->spans_dr;
        dginc = wstate->spans_dg;
        dbinc = wstate->spans_db;
        dainc = wstate->spans_da;
        dzinc = wstate->spans_dz;
        dsinc = wstate->spans_ds;
        dtinc = wstate->spans_dt;
        dwinc = wstate->spans_dw;
        xinc = 1;
    } else {
        drinc = -wstate->spans_dr;
        dginc = -wstate->spans_dg;
        dbinc = -wstate->spans_db;
        dainc = -wstate->spans_da;
        dzinc = -wstate->spans_dz;
        dsinc = -wstate->spans_ds;
        dtinc = -wstate->spans_dt;
        dwinc = -wstate->spans_dw;
        xinc = -1;
    }

    int dzpix;
    if (!wstate->other_modes.z_source_sel)
        dzpix = wstate->spans_dzpix;
    else {
        dzpix = wstate->primitive_delta_z;
        dzinc = wstate->spans_cdz = wstate->spans_dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;

    int r, g, b, a, z, s, t, w;
    int sr, sg, sb, sa, sz, ss, st, sw;
    int xstart, xend, xendsc;
    int sss = 0, sst = 0;
    int curpixel = 0;
    int wen;

    int x, length, scdiff;
    uint32_t fir, fig, fib;
    int delayedhbwidx = -1;

    for (i = start; i <= end; i++) {
        if (wstate->span[i].validline) {

            xstart = wstate->span[i].lx;
            xend = wstate->span[i].unscrx;
            xendsc = wstate->span[i].rx;
            r = wstate->span[i].r;
            g = wstate->span[i].g;
            b = wstate->span[i].b;
            a = wstate->span[i].a;
            z = wstate->other_modes.z_source_sel ? wstate->primitive_z : wstate->span[i].z;
            s = wstate->span[i].s;
            t = wstate->span[i].t;
            w = wstate->span[i].w;

            x = xendsc;
            curpixel = wstate->fb_width * i + x;
            zbcur = zb + curpixel;

            if (!flip) {
                length = xendsc - xstart;
                scdiff = xend - xendsc;
                compute_cvg_noflip(wstate, i);
            } else {
                length = xstart - xendsc;
                scdiff = xendsc - xend;
                compute_cvg_flip(wstate, i);
            }

            if (scdiff) {
                scdiff &= 0xfff;
                r += (drinc * scdiff);
                g += (dginc * scdiff);
                b += (dbinc * scdiff);
                a += (dainc * scdiff);
                z += (dzinc * scdiff);
                s += (dsinc * scdiff);
                t += (dtinc * scdiff);
                w += (dwinc * scdiff);
            }

            for (j = 0; j <= length; j++) {
                sz = (z >> 10) & 0x3fffff;

                if (!j) {
                    sr = r >> 14;
                    sg = g >> 14;
                    sb = b >> 14;
                    sa = a >> 14;
                    ss = s >> 16;
                    st = t >> 16;
                    sw = w >> 16;

                    wstate->tcdiv_ptr(ss, st, sw, &sss, &sst);

                    tclod_2cycle_notexel1(wstate, &sss, &sst, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1);

                    texture_pipeline_cycle(wstate, &wstate->texel0_color, &wstate->texel0_color, sss, sst, tile1, 0);

                    lookup_cvmask_derivatives(wstate->cvgbuf[x], &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

                    rgba_correct(wstate, offx, offy, sr, sg, sb, sa, curpixel_cvg);

                    if (wstate->other_modes.f.getditherlevel != DITHER_LEVEL_UNUSED)
                        get_dither_noise(wstate, x, i, &cdith, &adith);

                    combiner_2cycle_cycle0(wstate, adith, curpixel_cvg, &acalpha);
                }

                z_correct(wstate, offx, offy, &sz, curpixel_cvg);

                // Advance texture pipeline
                wstate->texel0_color = wstate->texel1_color;
                wstate->texel1_color = wstate->nexttexel_color;
                combiner_finalstage(wstate, adith, &curpixel_cvg);

                wstate->fbread2_ptr(wstate, curpixel, &curpixel_memcvg);

                wen = z_compare(wstate, zbcur, sz, (uint16_t)dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg,
                                curpixel_memcvg);

                if (wen)
                    wen = wstate->other_modes.antialias_en ? curpixel_cvg : curpixel_cvbit;

                if (wen)
                    blender_2cycle_cycle0(wstate);

                if (!wen && i >= wstate->last_overwriting_scanline)
                    blender_2cycle_cycle0_gval(wstate, curpixel);

                wstate->memory_color = wstate->pre_memory_color;

                x += xinc;

                r += drinc;
                g += dginc;
                b += dbinc;
                a += dainc;
                s += dsinc;
                t += dtinc;
                w += dwinc;

                sr = r >> 14;
                sg = g >> 14;
                sb = b >> 14;
                sa = a >> 14;
                ss = s >> 16;
                st = t >> 16;
                sw = w >> 16;

                lookup_cvmask_derivatives(j < length ? wstate->cvgbuf[x] : 0, &offx, &offy, &nextpixel_cvg,
                                          &curpixel_cvbit);

                rgba_correct(wstate, offx, offy, sr, sg, sb, sa, nextpixel_cvg);

                wstate->tcdiv_ptr(ss, st, sw, &sss, &sst);

                tclod_2cycle_notexel1(wstate, &sss, &sst, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1);

                texture_pipeline_cycle(wstate, &wstate->texel0_color, &wstate->texel0_color, sss, sst, tile1, 0);

                combiner_2cycle_cycle0(wstate, adith, nextpixel_cvg, &acalpha);

                if (wen)
                    wen = alpha_compare(wstate, acalpha);

                if (wen) {
                    blender_finalstage(wstate, &fir, &fig, &fib, cdith, blend_en, prewrap,
                                       wstate->other_modes.f.partialreject_2cycle, 1);
                    wstate->fbwrite_ptr(wstate, curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg, flip,
                                        &delayedhbwidx);
                    if (wstate->other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                } else if (i >= wstate->last_overwriting_scanline)
                    rejected_hbwrite_2cycle(wstate, cdith, blend_en, prewrap, curpixel, curpixel_cvg, curpixel_memcvg,
                                            flip, &delayedhbwidx);

                if (wstate->other_modes.f.getditherlevel != DITHER_LEVEL_UNUSED)
                    get_dither_noise(wstate, x, i, &cdith, &adith);

                curpixel_cvg = nextpixel_cvg;

                z += dzinc;

                curpixel += xinc;
                zbcur += xinc;
            }
        }
    }

    if (delayedhbwidx >= 0 && flip && wstate->fb_size == PIXEL_SIZE_8BIT)
        rdram_complete_delayed_hbwrites(delayedhbwidx);
}
#endif

#if 0
static void
render_spans_2cycle_notex(struct rdp_state *wstate, int start, int end, int tilenum, int flip)
{
    UNUSED(tilenum);

    int zb = wstate->zb_address >> 1;
    int zbcur;
    uint8_t offx = 0;
    uint8_t offy = 0;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg = 0, curpixel_cvbit = 0, curpixel_memcvg = 0;
    uint32_t nextpixel_cvg;
    uint32_t acalpha;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc;
    int xinc;
    if (flip) {
        drinc = wstate->spans_dr;
        dginc = wstate->spans_dg;
        dbinc = wstate->spans_db;
        dainc = wstate->spans_da;
        dzinc = wstate->spans_dz;
        xinc = 1;
    } else {
        drinc = -wstate->spans_dr;
        dginc = -wstate->spans_dg;
        dbinc = -wstate->spans_db;
        dainc = -wstate->spans_da;
        dzinc = -wstate->spans_dz;
        xinc = -1;
    }

    int dzpix;
    if (!wstate->other_modes.z_source_sel)
        dzpix = wstate->spans_dzpix;
    else {
        dzpix = wstate->primitive_delta_z;
        dzinc = wstate->spans_cdz = wstate->spans_dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;

    int r, g, b, a, z;
    int sr, sg, sb, sa, sz;
    int xstart, xend, xendsc;
    int curpixel = 0;
    int wen;

    int x, length, scdiff;
    uint32_t fir, fig, fib;
    int delayedhbwidx = -1;

    for (i = start; i <= end; i++) {
        if (wstate->span[i].validline) {

            xstart = wstate->span[i].lx;
            xend = wstate->span[i].unscrx;
            xendsc = wstate->span[i].rx;
            r = wstate->span[i].r;
            g = wstate->span[i].g;
            b = wstate->span[i].b;
            a = wstate->span[i].a;
            z = wstate->other_modes.z_source_sel ? wstate->primitive_z : wstate->span[i].z;

            x = xendsc;
            curpixel = wstate->fb_width * i + x;
            zbcur = zb + curpixel;

            if (!flip) {
                length = xendsc - xstart;
                scdiff = xend - xendsc;
                compute_cvg_noflip(wstate, i);
            } else {
                length = xstart - xendsc;
                scdiff = xendsc - xend;
                compute_cvg_flip(wstate, i);
            }

            if (scdiff) {
                scdiff &= 0xfff;
                r += (drinc * scdiff);
                g += (dginc * scdiff);
                b += (dbinc * scdiff);
                a += (dainc * scdiff);
                z += (dzinc * scdiff);
            }

            for (j = 0; j <= length; j++) {
                sz = (z >> 10) & 0x3fffff;

                if (!j) {
                    sr = r >> 14;
                    sg = g >> 14;
                    sb = b >> 14;
                    sa = a >> 14;

                    lookup_cvmask_derivatives(wstate->cvgbuf[x], &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

                    rgba_correct(wstate, offx, offy, sr, sg, sb, sa, curpixel_cvg);

                    if (wstate->other_modes.f.getditherlevel != DITHER_LEVEL_UNUSED)
                        get_dither_noise(wstate, x, i, &cdith, &adith);

                    combiner_2cycle_cycle0(wstate, adith, curpixel_cvg, &acalpha);
                }

                z_correct(wstate, offx, offy, &sz, curpixel_cvg);

                // Advance texture pipeline
                wstate->texel0_color = wstate->texel1_color;
                wstate->texel1_color = wstate->nexttexel_color;
                combiner_finalstage(wstate, adith, &curpixel_cvg);

                wstate->fbread2_ptr(wstate, curpixel, &curpixel_memcvg);

                wen = z_compare(wstate, zbcur, sz, (uint16_t)dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg,
                                curpixel_memcvg);

                if (wen)
                    wen = wstate->other_modes.antialias_en ? curpixel_cvg : curpixel_cvbit;

                if (wen)
                    blender_2cycle_cycle0(wstate);

                if (!wen && i >= wstate->last_overwriting_scanline)
                    blender_2cycle_cycle0_gval(wstate, curpixel);

                wstate->memory_color = wstate->pre_memory_color;

                x += xinc;

                r += drinc;
                g += dginc;
                b += dbinc;
                a += dainc;

                sr = r >> 14;
                sg = g >> 14;
                sb = b >> 14;
                sa = a >> 14;

                lookup_cvmask_derivatives(j < length ? wstate->cvgbuf[x] : 0, &offx, &offy, &nextpixel_cvg,
                                          &curpixel_cvbit);

                rgba_correct(wstate, offx, offy, sr, sg, sb, sa, nextpixel_cvg);

                combiner_2cycle_cycle0(wstate, adith, nextpixel_cvg, &acalpha);

                if (wen)
                    wen = alpha_compare(wstate, acalpha);

                if (wen) {
                    blender_finalstage(wstate, &fir, &fig, &fib, cdith, blend_en, prewrap,
                                       wstate->other_modes.f.partialreject_2cycle, 1);
                    wstate->fbwrite_ptr(wstate, curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg, flip,
                                        &delayedhbwidx);
                    if (wstate->other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                } else if (i >= wstate->last_overwriting_scanline)
                    rejected_hbwrite_2cycle(wstate, cdith, blend_en, prewrap, curpixel, curpixel_cvg, curpixel_memcvg,
                                            flip, &delayedhbwidx);

                if (wstate->other_modes.f.getditherlevel != DITHER_LEVEL_UNUSED)
                    get_dither_noise(wstate, x, i, &cdith, &adith);

                curpixel_cvg = nextpixel_cvg;

                z += dzinc;

                curpixel += xinc;
                zbcur += xinc;
            }
        }
    }

    if (delayedhbwidx >= 0 && flip && wstate->fb_size == PIXEL_SIZE_8BIT)
        rdram_complete_delayed_hbwrites(delayedhbwidx);
}
#endif

static void
render_spans_fill(struct rdp_state *wstate, int ystart, int yend, int flip)
{
    // FILL mode is unavailable for 4-bit cfb
    if (wstate->fb_size == PIXEL_SIZE_4BIT) {
        rdp_pipeline_crashed = 1;
        return;
    }

    // Checks for crashing modes
    int fastkillbits = wstate->other_modes.image_read_en || wstate->other_modes.z_compare_en;
    int slowkillbits = wstate->other_modes.z_update_en && !wstate->other_modes.z_source_sel && !fastkillbits;

    int xinc = flip ? 1 : -1;
    int delayedhbwidx = -1;

    for (int ycur = ystart; ycur <= yend; ycur++) {
        if (!wstate->span[ycur].validline)
            continue;

        int xstart = wstate->span[ycur].lx;
        int xendsc = wstate->span[ycur].rx;
        int length = flip ? (xstart - xendsc) : (xendsc - xstart);

        // im_rd or z_cmp is enabled and we have pixels to draw, crash
        // These need to happen before color writes since reads of color or z happen before writes
        if (fastkillbits && length >= 0) {
            if (!onetimewarnings.fillmbitcrashes)
                msg_warning("render_spans_fill: image_read_en %x z_update_en %x z_compare_en %x. RDP crashed",
                            wstate->other_modes.image_read_en, wstate->other_modes.z_update_en,
                            wstate->other_modes.z_compare_en);
            onetimewarnings.fillmbitcrashes = true;
            rdp_pipeline_crashed = 1;
            return;
        }

        // Fill the span with the fill color
        int curpixel = wstate->fb_width * ycur + xendsc;
        for (int i = 0; i <= length; i++) {
            wstate->fbfill_ptr(wstate, curpixel, flip, &delayedhbwidx);
            curpixel += xinc;
        }

        // per-pixel z_upd is enabled and !(IM_RD || Z_CMP)    (note that prim z depth writes also crash the rdp..)
        // This happens after the color writes since z writes happen after color writes
        if (slowkillbits && length >= 0) {
            if (!onetimewarnings.fillmbitcrashes)
                msg_warning("render_spans_fill: image_read_en %x z_update_en %x z_compare_en %x z_source_sel %x. "
                            "RDP crashed",
                            wstate->other_modes.image_read_en, wstate->other_modes.z_update_en,
                            wstate->other_modes.z_compare_en, wstate->other_modes.z_source_sel);
            onetimewarnings.fillmbitcrashes = 1;
            rdp_pipeline_crashed = 1;
            return;
        }
    }

    if (delayedhbwidx >= 0 && flip && wstate->fb_size == PIXEL_SIZE_8BIT)
        rdram_complete_delayed_hbwrites(delayedhbwidx);
}

static void
render_spans_copy(struct rdp_state *wstate, int start, int end, int tilenum, int flip)
{
    // Copy mode is unavailable for 32-bit cfb,
    // TMEM is not equipped to output the required amount of data fast enough
    if (wstate->fb_size == PIXEL_SIZE_32BIT) {
        rdp_pipeline_crashed = 1;
        return;
    }

    int dsinc, dtinc, dwinc;
    int xinc;
    if (flip) {
        dsinc = wstate->spans_dsdx;
        dtinc = wstate->spans_dtdx;
        dwinc = wstate->spans_dwdx;
        xinc = 1;
    } else {
        dsinc = -wstate->spans_dsdx;
        dtinc = -wstate->spans_dtdx;
        dwinc = -wstate->spans_dwdx;
        xinc = -1;
    }

    int tile1 = tilenum;
    int prim_tile = tilenum;

    int fbadvance = ((wstate->fb_size == PIXEL_SIZE_4BIT) ? 8 : 16) >> wstate->fb_size;
    int bytesperpixel = (wstate->fb_size == PIXEL_SIZE_4BIT) ? 1 : (1 << (wstate->fb_size - 1));
    int fbptr_advance = flip ? 8 : -8;

    int delayedhbwidx = -1;

    for (int i = start; i <= end; i++) {
        if (!wstate->span[i].validline)
            continue;

        int s = wstate->span[i].s;
        int t = wstate->span[i].t;
        int w = wstate->span[i].w;

        int xstart = wstate->span[i].lx;
        int xendsc = wstate->span[i].rx;
        int length = flip ? (xstart - xendsc) : (xendsc - xstart);

#define PIXELS_TO_BYTES_SPECIAL4(pix, siz) ((siz) ? PIXELS_TO_BYTES(pix, siz) : (pix))

        int fb_index = wstate->fb_width * i + xendsc;
        int fb_end_index = wstate->fb_width * i + xstart;
        uint32_t fbptr = wstate->fb_address + PIXELS_TO_BYTES_SPECIAL4(fb_index, wstate->fb_size);
        uint32_t fbendptr = wstate->fb_address + PIXELS_TO_BYTES_SPECIAL4(fb_end_index, wstate->fb_size);

        for (int j = 0; j <= length; j += fbadvance) {
            int ss = s >> 16;
            int st = t >> 16;
            int sw = w >> 16;

            // Texture perspective correction
            int sss, sst;
            wstate->tcdiv_ptr(ss, st, sw, &sss, &sst);

            // Cycle LOD pipeline, the resulting tile is actually used?
            // TODO is lod_frac computed even if copy mode can't use it? e.g. switch to 1-cycle mode and try to
            // see it in the first pixel?
            tclod_copy(wstate, &sss, &sst, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1);

            // TMEM sampling
            uint32_t hidword, lowdword;
            fetch_qword_copy(wstate, &hidword, &lowdword, sss, sst, tile1);

            // When pixel size is 4-bit, force to 0? (still samples TMEM in that it cycles the texture pipeline)
            uint64_t copyqword = 0;
            if (wstate->fb_size != PIXEL_SIZE_4BIT)
                copyqword = ((uint64_t)hidword << 32) | ((uint64_t)lowdword);

            // Alpha compare write-enable mask

            int alphamask;
            if (!wstate->other_modes.alpha_compare_en) {
                alphamask = 0xff;
            } else if (wstate->fb_size == PIXEL_SIZE_16BIT) {
                // Check LSBit of each 16-bit texel (assumed to be rgba16), alpha compare passes if the bit is set
                alphamask = 0;
                alphamask |= (copyqword >> 48 & 1) ? 0xC0 : 0;
                alphamask |= (copyqword >> 32 & 1) ? 0x30 : 0;
                alphamask |= (copyqword >> 16 & 1) ? 0x0C : 0;
                alphamask |= (copyqword >> 0 & 1) ? 0x03 : 0;
            } else if (wstate->fb_size == PIXEL_SIZE_8BIT) {
                // Compare each 8-bit texel with configured alpha compare threshold, steps noise for each byte
                uint8_t threshold0;
                uint8_t threshold1;
                uint8_t threshold2;
                uint8_t threshold3;
                if (wstate->other_modes.dither_alpha_en) {
                    threshold0 = irand(&wstate->rseed);
                    threshold1 = ((threshold0 & 0x03) << 6) | (threshold0 >> 2);
                    threshold2 = ((threshold0 & 0x0F) << 4) | (threshold0 >> 4);
                    threshold3 = ((threshold0 & 0x3F) << 2) | (threshold0 >> 6);
                } else {
                    threshold0 = threshold1 = threshold2 = threshold3 = wstate->blend_color.a;
                }
                alphamask = 0;
                alphamask |= ((uint8_t)(copyqword >> 24) >= threshold0) ? 0xC0 : 0;
                alphamask |= ((uint8_t)(copyqword >> 16) >= threshold1) ? 0x30 : 0;
                alphamask |= ((uint8_t)(copyqword >> 8) >= threshold2) ? 0x0C : 0;
                alphamask |= ((uint8_t)(copyqword >> 0) >= threshold3) ? 0x03 : 0;
            } else {
                // 4-bit always fails alpha compare
                alphamask = 0;
            }

            // Write

            int copywmask = flip ? (fbendptr - fbptr + bytesperpixel) : (fbptr - fbendptr + bytesperpixel);
            if (copywmask > 8)
                copywmask = 8;

            int k;
            uint32_t tempdword;
            for (tempdword = fbptr, k = 7; copywmask > 0; copywmask--, k--, tempdword += xinc) {
                if (alphamask & (1 << k)) {
                    rdram_write_pair8(tempdword, copyqword >> (k << 3), flip, &delayedhbwidx);
#ifdef N64VIDEO_METRICS
                    // Copy mode writes raw bytes here and bypasses fbwrite_*, so the
                    // overdraw heatmap must be bumped explicitly (otherwise copy-mode
                    // blits like SM64's HUD/text show no overdraw at all). Count one
                    // write per framebuffer pixel: a 16-bit pixel spans two consecutive
                    // bytes (k and k-1), so only bump on the high byte to match fbwrite_16.
                    if (wstate->fb_size != PIXEL_SIZE_16BIT || (k & 1))
                        fb_heatmap_bump(fb_heatmap_writes, tempdword >> 1);
#endif
                }
            }

            // Increment attributes/pointer
            s += dsinc;
            t += dtinc;
            w += dwinc;
            fbptr += fbptr_advance;
        }
    }

    if (delayedhbwidx >= 0 && flip && wstate->fb_size == PIXEL_SIZE_8BIT)
        rdram_complete_delayed_hbwrites(delayedhbwidx);
}

static void
edgewalker_for_prims(struct rdp_state *wstate, uint32_t *ewdata)
{
    if (wstate->other_modes.f.stalederivs) {
        deduce_derivatives(wstate);
        wstate->other_modes.f.stalederivs = 0;
    }

    if (wstate->fb_size == PIXEL_SIZE_8BIT) {
        rdram_hidden_old[0] &= ~2;
        rdram_hidden_old[4] &= ~2;
    }

    int oldhb_diff = wstate->fb_size == PIXEL_SIZE_16BIT ? 7 : 3;
    wstate->last_overwriting_scanline = -1;

    int flip = (ewdata[0] & 0x800000) != 0;
    int tilenum = (ewdata[0] >> 16) & 7;
    wstate->max_level = (ewdata[0] >> 19) & 7;

    // bit 13 is the sign bit => [-8192, 8191] = [-0x2000, 0x1FFF]
    // SIGN(x, numb)	(((x) & ((1 << (numb)) - 1)) | -((x) & (1 << ((numb) - 1))))
    // SIGN(x, 14)	((x & 0x3FFF) | -(x & (1 << 13)))
    // 0011111111111111
    // 0010000000000000
    int32_t yl = SIGN(ewdata[0], 14); // s11.2
    int32_t ym = SIGN(ewdata[1] >> 16, 14);
    int32_t yh = SIGN(ewdata[1], 14);

    // bit 27 is the sign bit => [-0x8000000, 0x7FFFFFF]
    int32_t xl = SIGN(ewdata[2], 28);
    int32_t xh = SIGN(ewdata[4], 28); // s11.16
    int32_t xm = SIGN(ewdata[6], 28);

    // bit 29 is the sign bit => [-0x20000000, 0x1FFFFFFF]
    int32_t dxldy = SIGN(ewdata[3], 30);
    int32_t dxhdy = SIGN(ewdata[5], 30); // s13.16
    int32_t dxmdy = SIGN(ewdata[7], 30);

    // wacky, dxhdy sign bit is the ORIGINAL bit 31 prior to sign extending?
    int sign_dxhdy = (ewdata[5] & 0x80000000) != 0;

    // unpack shade coefficients (9.16)
    int r = (ewdata[8] & 0xffff0000) | ((ewdata[12] >> 16) & 0x0000ffff);
    int g = ((ewdata[8] << 16) & 0xffff0000) | (ewdata[12] & 0x0000ffff);
    int b = (ewdata[9] & 0xffff0000) | ((ewdata[13] >> 16) & 0x0000ffff);
    int a = ((ewdata[9] << 16) & 0xffff0000) | (ewdata[13] & 0x0000ffff);

    // s15.16
    int drdx = (ewdata[10] & 0xffff0000) | ((ewdata[14] >> 16) & 0x0000ffff); // 0x7FFFFFF, sign at bit 27
    int dgdx = ((ewdata[10] << 16) & 0xffff0000) | (ewdata[14] & 0x0000ffff);
    int dbdx = (ewdata[11] & 0xffff0000) | ((ewdata[15] >> 16) & 0x0000ffff);
    int dadx = ((ewdata[11] << 16) & 0xffff0000) | (ewdata[15] & 0x0000ffff);
    // s15.16
    int drde = (ewdata[16] & 0xffff0000) | ((ewdata[20] >> 16) & 0x0000ffff);
    int dgde = ((ewdata[16] << 16) & 0xffff0000) | (ewdata[20] & 0x0000ffff);
    int dbde = (ewdata[17] & 0xffff0000) | ((ewdata[21] >> 16) & 0x0000ffff);
    int dade = ((ewdata[17] << 16) & 0xffff0000) | (ewdata[21] & 0x0000ffff);
    // s15.16
    int drdy = (ewdata[18] & 0xffff0000) | ((ewdata[22] >> 16) & 0x0000ffff);
    int dgdy = ((ewdata[18] << 16) & 0xffff0000) | (ewdata[22] & 0x0000ffff);
    int dbdy = (ewdata[19] & 0xffff0000) | ((ewdata[23] >> 16) & 0x0000ffff);
    int dady = ((ewdata[19] << 16) & 0xffff0000) | (ewdata[23] & 0x0000ffff);

    // unpack texture coefficients
    // s,t,w are s10.21
    // d[stw]d[xye] are s15.11 ???
    int s = (ewdata[24] & 0xffff0000) | ((ewdata[28] >> 16) & 0x0000ffff);
    int t = ((ewdata[24] << 16) & 0xffff0000) | (ewdata[28] & 0x0000ffff);
    int w = (ewdata[25] & 0xffff0000) | ((ewdata[29] >> 16) & 0x0000ffff);
    int dsdx = (ewdata[26] & 0xffff0000) | ((ewdata[30] >> 16) & 0x0000ffff);
    int dtdx = ((ewdata[26] << 16) & 0xffff0000) | (ewdata[30] & 0x0000ffff);
    int dwdx = (ewdata[27] & 0xffff0000) | ((ewdata[31] >> 16) & 0x0000ffff);
    int dsde = (ewdata[32] & 0xffff0000) | ((ewdata[36] >> 16) & 0x0000ffff);
    int dtde = ((ewdata[32] << 16) & 0xffff0000) | (ewdata[36] & 0x0000ffff);
    int dwde = (ewdata[33] & 0xffff0000) | ((ewdata[37] >> 16) & 0x0000ffff);
    int dsdy = (ewdata[34] & 0xffff0000) | ((ewdata[38] >> 16) & 0x0000ffff);
    int dtdy = ((ewdata[34] << 16) & 0xffff0000) | (ewdata[38] & 0x0000ffff);
    int dwdy = (ewdata[35] & 0xffff0000) | ((ewdata[39] >> 16) & 0x0000ffff);

    // unpack depth coefficients
    int z = ewdata[40];    // s15.16
    int dzdx = ewdata[41]; // s15.16
    int dzde = ewdata[42]; // s15.16
    int dzdy = ewdata[43]; // s15.16

    // d*dx, remove lower 5 bits for all but z
    wstate->spans_dsdx = dsdx & ~0x1f;
    wstate->spans_dtdx = dtdx & ~0x1f;
    wstate->spans_dwdx = dwdx & ~0x1f;
    wstate->spans_drdx = drdx & ~0x1f;
    wstate->spans_dgdx = dgdx & ~0x1f;
    wstate->spans_dbdx = dbdx & ~0x1f;
    wstate->spans_dadx = dadx & ~0x1f;
    wstate->spans_dzdx = dzdx;

    // d*dx for subpixel correction
    wstate->spans_cdrdx = SIGN(wstate->spans_drdx >> 14, 13); // [-0x1000, 0xFFF]     s10.2 ?
    wstate->spans_cdgdx = SIGN(wstate->spans_dgdx >> 14, 13);
    wstate->spans_cdbdx = SIGN(wstate->spans_dbdx >> 14, 13);
    wstate->spans_cdadx = SIGN(wstate->spans_dadx >> 14, 13);
    // sign is in bit 31 originally, so it's in bit 21 after shifting right by 10
    wstate->spans_cdzdx = SIGN(wstate->spans_dzdx >> 10, 22);

    // d*dy
    wstate->spans_dsdy = dsdy & ~0x7fff; // remove lower 15 bits
    wstate->spans_dtdy = dtdy & ~0x7fff;
    wstate->spans_dwdy = dwdy & ~0x7fff;
    wstate->spans_drdy = SIGN(drdy >> 14, 13); // [-0x1000, 0xFFF]
    wstate->spans_dgdy = SIGN(dgdy >> 14, 13);
    wstate->spans_dbdy = SIGN(dbdy >> 14, 13);
    wstate->spans_dady = SIGN(dady >> 14, 13);
    wstate->spans_dzdy = SIGN(dzdy >> 10, 22);

    // Compute pixel dz value
    // extract integer parts, sign bits in bit 15
    int dzdy_dz = (dzdy >> 16) & 0xffff;
    int dzdx_dz = (dzdx >> 16) & 0xffff;
    // this abs() is 1-off for negative values
#define ABS16(x) (((x)&0x8000) ? ((~(x)) & 0x7fff) : (x))
    wstate->spans_dzpix = ABS16(dzdx_dz) + ABS16(dzdy_dz);
    wstate->spans_dzpix = normalize_dzpix(wstate->spans_dzpix);

    int xleft_inc = (dxmdy >> 2) & ~1;
    int xright_inc = (dxhdy >> 2) & ~1;

    int xleft = xm & ~1;
    int xright = xh & ~1;

    int dsdiff, dtdiff, dwdiff, drdiff, dgdiff, dbdiff, dadiff, dzdiff;
    if (!(sign_dxhdy ^ flip)) {
        int dsdeh = dsde & ~0x1ff;
        int dtdeh = dtde & ~0x1ff;
        int dwdeh = dwde & ~0x1ff;
        int drdeh = drde & ~0x1ff;
        int dgdeh = dgde & ~0x1ff;
        int dbdeh = dbde & ~0x1ff;
        int dadeh = dade & ~0x1ff;
        int dzdeh = dzde & ~0x1ff;

        int dsdyh = dsdy & ~0x1ff;
        int dtdyh = dtdy & ~0x1ff;
        int dwdyh = dwdy & ~0x1ff;
        int drdyh = drdy & ~0x1ff;
        int dgdyh = dgdy & ~0x1ff;
        int dbdyh = dbdy & ~0x1ff;
        int dadyh = dady & ~0x1ff;
        int dzdyh = dzdy & ~0x1ff;

        // x - x >> 2 ~= 3 * x / 4
        dsdiff = dsdeh - (dsdeh >> 2) - dsdyh + (dsdyh >> 2);
        dtdiff = dtdeh - (dtdeh >> 2) - dtdyh + (dtdyh >> 2);
        dwdiff = dwdeh - (dwdeh >> 2) - dwdyh + (dwdyh >> 2);
        drdiff = drdeh - (drdeh >> 2) - drdyh + (drdyh >> 2);
        dgdiff = dgdeh - (dgdeh >> 2) - dgdyh + (dgdyh >> 2);
        dbdiff = dbdeh - (dbdeh >> 2) - dbdyh + (dbdyh >> 2);
        dadiff = dadeh - (dadeh >> 2) - dadyh + (dadyh >> 2);
        dzdiff = dzdeh - (dzdeh >> 2) - dzdyh + (dzdyh >> 2);
    } else {
        dsdiff = dtdiff = dwdiff = drdiff = dgdiff = dbdiff = dadiff = dzdiff = 0;
    }

    int dsdxh, dtdxh, dwdxh, drdxh, dgdxh, dbdxh, dadxh, dzdxh;
    if (wstate->other_modes.cycle_type != CYCLE_TYPE_COPY) {
        dsdxh = (dsdx >> 8) & ~1;
        dtdxh = (dtdx >> 8) & ~1;
        dwdxh = (dwdx >> 8) & ~1;
        drdxh = (drdx >> 8) & ~1;
        dgdxh = (dgdx >> 8) & ~1;
        dbdxh = (dbdx >> 8) & ~1;
        dadxh = (dadx >> 8) & ~1;
        dzdxh = (dzdx >> 8) & ~1;
    } else {
        dsdxh = dtdxh = dwdxh = drdxh = dgdxh = dbdxh = dadxh = dzdxh = 0;
    }

    int ystart = yh & ~3;
    int ldflag = (sign_dxhdy ^ flip) ? 0 : 3;

    // Compute the yl limit and yl "far"

    int32_t yllimit; // = min(yl, wstate->clip.yl)
    bool yl_in_clip;
    if (yl & (1 << 13)) // yl is negative, always < clip.yl since clip.yl is unsigned
        yl_in_clip = true;
    else if (yl & (1 << 12)) // yl is very positive, clip.yl is only 10.2 fixed point so this is always > clip.yl
        yl_in_clip = false;
    else
        yl_in_clip = (yl & 0xfff) < wstate->clip.yl; // clip.yl is 10.2 fixed point
    yllimit = yl_in_clip ? yl : wstate->clip.yl;

    int ylfar = yllimit | 3;
    if ((yl >> 2) > (ylfar >> 2)) // increment 1 extra line if yl integer is larger than ylfar integer
        ylfar += 4;
    else if ((yllimit >> 2) >= 0 && (yllimit >> 2) < 1023)  // yllimit integer in [0,1022]
        wstate->span[(yllimit >> 2) + 1].validline = false; // invalidate next line?

    // Compute the yh limit and yh "close"
    int32_t yhlimit; // = max(yh, wstate->clip.yh)
    bool yh_in_clip;
    if (yh & (1 << 13))
        yh_in_clip = false;
    else if (yh & (1 << 12))
        yh_in_clip = true;
    else
        yh_in_clip = yh >= wstate->clip.yh;
    yhlimit = yh_in_clip ? yh : wstate->clip.yh;

    int yhclose = yhlimit & ~3;

    // Pre-shift the clip x limits before entering the loop
    int32_t clipxlshift = wstate->clip.xl << 1;
    int32_t clipxhshift = wstate->clip.xh << 1;

    // These three variables are always initialized before use, but compiler may complain
    bool allover = true;
    bool allunder = true;
    bool allinval = true;
    int32_t minx = 0;
    int32_t maxx = 0;

    int xfrac;

    for (int ycur = ystart; ycur <= ylfar; ycur++) {
        if (ycur == ym) {
            xleft = xl & ~1;
            xleft_inc = (dxldy >> 2) & ~1;
        }

        int ycur_int = ycur >> 2;
        int ycur_frac = ycur & 3;

        if (ycur >= yhclose) {
            bool invaly = ycur < yhlimit || ycur >= yllimit;

            if (ycur_frac == 0) {
                maxx = 0;
                minx = 0xfff;
                allover = allunder = true;
                allinval = true;
            }

            bool stickybit_r = (xright >> 1 & 0x1fff) > 0;
            bool stickybit_l = (xleft >> 1 & 0x1fff) > 0;

            int32_t xrsc = (xright >> 13 & 0x1ffe) | stickybit_r;
            int32_t xlsc = (xleft >> 13 & 0x1ffe) | stickybit_l;

            bool curunder_r = (xright & 0x8000000) || (xrsc < clipxhshift && !(xright & 0x4000000));
            bool curunder_l = (xleft & 0x8000000) || (xlsc < clipxhshift && !(xleft & 0x4000000));

            xrsc = curunder_r ? clipxhshift : ((xright >> 13 & 0x3ffe) | stickybit_r);
            xlsc = curunder_l ? clipxhshift : ((xleft >> 13 & 0x3ffe) | stickybit_l);

            bool curover_r = (xrsc & 0x2000) || (xrsc & 0x1fff) >= clipxlshift;
            bool curover_l = (xlsc & 0x2000) || (xlsc & 0x1fff) >= clipxlshift;

            xrsc = curover_r ? clipxlshift : xrsc;
            xlsc = curover_l ? clipxlshift : xlsc;

            wstate->span[ycur_int].majorx[ycur_frac] = xrsc & 0x1fff;
            wstate->span[ycur_int].minorx[ycur_frac] = xlsc & 0x1fff;

            allover &= curover_r & curover_l;
            allunder &= curunder_r & curunder_l;

            if (flip)
                invaly |= ((xleft ^ (1 << 27)) & (0x3fff << 14)) < ((xright ^ (1 << 27)) & (0x3fff << 14));
            else
                invaly |= ((xright ^ (1 << 27)) & (0x3fff << 14)) < ((xleft ^ (1 << 27)) & (0x3fff << 14));

            wstate->span[ycur_int].invalyscan[ycur_frac] = invaly;
            allinval &= invaly;

            if (!invaly) {
                if (flip) {
                    maxx = MAX(xlsc >> 3 & 0xfff, maxx);
                    minx = MIN(xrsc >> 3 & 0xfff, minx);
                } else {
                    maxx = MAX(xrsc >> 3 & 0xfff, maxx);
                    minx = MIN(xlsc >> 3 & 0xfff, minx);
                }
            }

            if (ycur_frac == ldflag) {
                wstate->span[ycur_int].unscrx = SIGN(xright >> 16, 12);
                xfrac = (xright >> 8) & 0xff;
                wstate->span[ycur_int].s = ((s & ~0x1ff) + dsdiff - (xfrac * dsdxh)) & ~0x3ff;
                wstate->span[ycur_int].t = ((t & ~0x1ff) + dtdiff - (xfrac * dtdxh)) & ~0x3ff;
                wstate->span[ycur_int].w = ((w & ~0x1ff) + dwdiff - (xfrac * dwdxh)) & ~0x3ff;
                wstate->span[ycur_int].r = ((r & ~0x1ff) + drdiff - (xfrac * drdxh)) & ~0x3ff;
                wstate->span[ycur_int].g = ((g & ~0x1ff) + dgdiff - (xfrac * dgdxh)) & ~0x3ff;
                wstate->span[ycur_int].b = ((b & ~0x1ff) + dbdiff - (xfrac * dbdxh)) & ~0x3ff;
                wstate->span[ycur_int].a = ((a & ~0x1ff) + dadiff - (xfrac * dadxh)) & ~0x3ff;
                wstate->span[ycur_int].z = ((z & ~0x1ff) + dzdiff - (xfrac * dzdxh)) & ~0x3ff;
            }

            if (ycur_frac == 3) {
                wstate->span[ycur_int].lx = flip ? maxx : minx;
                wstate->span[ycur_int].rx = flip ? minx : maxx;
                wstate->span[ycur_int].validline =
                    !allinval && !allover && !allunder &&
                    (!wstate->scfield || (wstate->scfield && !(wstate->sckeepodd ^ (ycur_int & 1))));

                bool cond;
                if (flip)
                    cond = (wstate->span[ycur_int].lx - wstate->span[ycur_int].rx) >= oldhb_diff;
                else
                    cond = (wstate->span[ycur_int].rx - wstate->span[ycur_int].lx) >= oldhb_diff;

                if (wstate->span[ycur_int].validline && wstate->fb_size > PIXEL_SIZE_8BIT && cond)
                    wstate->last_overwriting_scanline = ycur_int;

                // skip line if not assigned to this worker
                wstate->span[ycur_int].validline &= (!wstate->stride || ycur_int % wstate->stride == wstate->offset);
            }
        }

        if (ycur_frac == 3) {
            s += dsde;
            t += dtde;
            w += dwde;
            r += drde;
            g += dgde;
            b += dbde;
            a += dade;
            z += dzde;
        }

        xleft += xleft_inc;
        xright += xright_inc;
    }

    switch (wstate->other_modes.cycle_type) {
        case_no_default;

        case CYCLE_TYPE_1:
            render_spans_1cycle_complete(wstate, yhlimit >> 2, yllimit >> 2, tilenum, flip);
#if 0
            switch (wstate->other_modes.f.textureuselevel0) {
                case 0:
                    render_spans_1cycle_complete(wstate, yhlimit >> 2, yllimit >> 2, tilenum, flip);
                    break;
                case 1:
                    render_spans_1cycle_notexel1(wstate, yhlimit >> 2, yllimit >> 2, tilenum, flip);
                    break;
                case 2:
                default:
                    render_spans_1cycle_notex(wstate, yhlimit >> 2, yllimit >> 2, tilenum, flip);
                    break;
            }
#endif
            break;
        case CYCLE_TYPE_2:
            render_spans_2cycle_complete(wstate, yhlimit >> 2, yllimit >> 2, tilenum, flip);
#if 0
            switch (wstate->other_modes.f.textureuselevel1) {
                case 0:
                    render_spans_2cycle_complete(wstate, yhlimit >> 2, yllimit >> 2, tilenum, flip);
                    break;
                case 1:
                    render_spans_2cycle_notexelnext(wstate, yhlimit >> 2, yllimit >> 2, tilenum, flip);
                    break;
                case 2:
                    render_spans_2cycle_notexel1(wstate, yhlimit >> 2, yllimit >> 2, tilenum, flip);
                    break;
                case 3:
                default:
                    render_spans_2cycle_notex(wstate, yhlimit >> 2, yllimit >> 2, tilenum, flip);
                    break;
            }
#endif
            break;
        case CYCLE_TYPE_COPY:
            render_spans_copy(wstate, yhlimit >> 2, yllimit >> 2, tilenum, flip);
            break;
        case CYCLE_TYPE_FILL:
            render_spans_fill(wstate, yhlimit >> 2, yllimit >> 2, flip);
            break;
    }
}

static void
rasterizer_init(struct rdp_state *wstate)
{
    wstate->clip.xh = 0x2000;
    wstate->clip.yh = 0x2000;
}

void
rdp_tri_noshade(struct rdp_state *wstate, const uint32_t *args)
{
    uint32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 8 * sizeof(uint32_t));
    memset(&ewdata[8], 0, 36 * sizeof(uint32_t));
    edgewalker_for_prims(wstate, ewdata);
}

void
rdp_tri_noshade_z(struct rdp_state *wstate, const uint32_t *args)
{
    uint32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 8 * sizeof(uint32_t));
    memset(&ewdata[8], 0, 32 * sizeof(uint32_t));
    memcpy(&ewdata[40], args + 8, 4 * sizeof(uint32_t));
    edgewalker_for_prims(wstate, ewdata);
}

void
rdp_tri_tex(struct rdp_state *wstate, const uint32_t *args)
{
    uint32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 8 * sizeof(uint32_t));
    memset(&ewdata[8], 0, 16 * sizeof(uint32_t));
    memcpy(&ewdata[24], args + 8, 16 * sizeof(uint32_t));
    memset(&ewdata[40], 0, 4 * sizeof(uint32_t));
    edgewalker_for_prims(wstate, ewdata);
}

void
rdp_tri_tex_z(struct rdp_state *wstate, const uint32_t *args)
{
    uint32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 8 * sizeof(uint32_t));
    memset(&ewdata[8], 0, 16 * sizeof(uint32_t));
    memcpy(&ewdata[24], args + 8, 16 * sizeof(uint32_t));
    memcpy(&ewdata[40], args + 24, 4 * sizeof(uint32_t));

    edgewalker_for_prims(wstate, ewdata);
}

void
rdp_tri_shade(struct rdp_state *wstate, const uint32_t *args)
{
    uint32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 24 * sizeof(uint32_t));
    memset(&ewdata[24], 0, 20 * sizeof(uint32_t));
    edgewalker_for_prims(wstate, ewdata);
}

void
rdp_tri_shade_z(struct rdp_state *wstate, const uint32_t *args)
{
    uint32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 24 * sizeof(uint32_t));
    memset(&ewdata[24], 0, 16 * sizeof(uint32_t));
    memcpy(&ewdata[40], args + 24, 4 * sizeof(uint32_t));
    edgewalker_for_prims(wstate, ewdata);
}

void
rdp_tri_texshade(struct rdp_state *wstate, const uint32_t *args)
{
    uint32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 40 * sizeof(uint32_t));
    memset(&ewdata[40], 0, 4 * sizeof(uint32_t));
    edgewalker_for_prims(wstate, ewdata);
}

void
rdp_tri_texshade_z(struct rdp_state *wstate, const uint32_t *args)
{
    uint32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, CMD_MAX_SIZE);

    edgewalker_for_prims(wstate, ewdata);
}

void
rdp_tex_rect(struct rdp_state *wstate, const uint32_t *args)
{
    uint32_t tilenum = (args[1] >> 24) & 0x7;
    uint32_t xl = (args[0] >> 12) & 0xfff;
    uint32_t yl = (args[0] >> 0) & 0xfff;
    uint32_t xh = (args[1] >> 12) & 0xfff;
    uint32_t yh = (args[1] >> 0) & 0xfff;

    int32_t s = (args[2] >> 16) & 0xffff;
    int32_t t = (args[2] >> 0) & 0xffff;
    int32_t dsdx = (args[3] >> 16) & 0xffff;
    int32_t dtdy = (args[3] >> 0) & 0xffff;

    dsdx = SIGN16(dsdx);
    dtdy = SIGN16(dtdy);

    if (wstate->other_modes.cycle_type == CYCLE_TYPE_FILL || wstate->other_modes.cycle_type == CYCLE_TYPE_COPY)
        yl |= 3;

    uint32_t xlint = (xl >> 2) & 0x3ff;
    uint32_t xhint = (xh >> 2) & 0x3ff;

    uint32_t ewdata[CMD_MAX_INTS];
    ewdata[0] = (0x24 << 24) | ((0x80 | tilenum) << 16) | yl;
    ewdata[1] = (yl << 16) | yh;
    ewdata[2] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[3] = 0;
    ewdata[4] = (xhint << 16) | ((xh & 3) << 14);
    ewdata[5] = 0;
    ewdata[6] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[7] = 0;
    memset(&ewdata[8], 0, 16 * sizeof(uint32_t));
    ewdata[24] = (s << 16) | t;
    ewdata[25] = 0;
    ewdata[26] = ((dsdx >> 5) << 16);
    ewdata[27] = 0;
    ewdata[28] = 0;
    ewdata[29] = 0;
    ewdata[30] = ((dsdx & 0x1f) << 11) << 16;
    ewdata[31] = 0;
    ewdata[32] = (dtdy >> 5) & 0xffff;
    ewdata[33] = 0;
    ewdata[34] = (dtdy >> 5) & 0xffff;
    ewdata[35] = 0;
    ewdata[36] = (dtdy & 0x1f) << 11;
    ewdata[37] = 0;
    ewdata[38] = (dtdy & 0x1f) << 11;
    ewdata[39] = 0;
    memset(&ewdata[40], 0, 4 * sizeof(int32_t));

    edgewalker_for_prims(wstate, ewdata);
}

void
rdp_tex_rect_flip(struct rdp_state *wstate, const uint32_t *args)
{
    uint32_t tilenum = (args[1] >> 24) & 0x7;
    uint32_t xl = (args[0] >> 12) & 0xfff;
    uint32_t yl = (args[0] >> 0) & 0xfff;
    uint32_t xh = (args[1] >> 12) & 0xfff;
    uint32_t yh = (args[1] >> 0) & 0xfff;

    int32_t s = (args[2] >> 16) & 0xffff;
    int32_t t = (args[2] >> 0) & 0xffff;
    int32_t dsdx = (args[3] >> 16) & 0xffff;
    int32_t dtdy = (args[3] >> 0) & 0xffff;

    dsdx = SIGN16(dsdx);
    dtdy = SIGN16(dtdy);

    if (wstate->other_modes.cycle_type == CYCLE_TYPE_FILL || wstate->other_modes.cycle_type == CYCLE_TYPE_COPY)
        yl |= 3;

    uint32_t xlint = (xl >> 2) & 0x3ff;
    uint32_t xhint = (xh >> 2) & 0x3ff;

    uint32_t ewdata[CMD_MAX_INTS];
    ewdata[0] = (0x25 << 24) | ((0x80 | tilenum) << 16) | yl;
    ewdata[1] = (yl << 16) | yh;
    ewdata[2] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[3] = 0;
    ewdata[4] = (xhint << 16) | ((xh & 3) << 14);
    ewdata[5] = 0;
    ewdata[6] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[7] = 0;
    memset(&ewdata[8], 0, 16 * sizeof(uint32_t));
    ewdata[24] = (s << 16) | t;
    ewdata[25] = 0;

    ewdata[26] = (dtdy >> 5) & 0xffff;
    ewdata[27] = 0;
    ewdata[28] = 0;
    ewdata[29] = 0;
    ewdata[30] = ((dtdy & 0x1f) << 11);
    ewdata[31] = 0;
    ewdata[32] = (dsdx >> 5) << 16;
    ewdata[33] = 0;
    ewdata[34] = (dsdx >> 5) << 16;
    ewdata[35] = 0;
    ewdata[36] = (dsdx & 0x1f) << 27;
    ewdata[37] = 0;
    ewdata[38] = (dsdx & 0x1f) << 27;
    ewdata[39] = 0;
    memset(&ewdata[40], 0, 4 * sizeof(uint32_t));

    edgewalker_for_prims(wstate, ewdata);
}

void
rdp_fill_rect(struct rdp_state *wstate, const uint32_t *args)
{
    uint32_t xl = (args[0] >> 12) & 0xfff;
    uint32_t yl = (args[0] >> 0) & 0xfff;
    uint32_t xh = (args[1] >> 12) & 0xfff;
    uint32_t yh = (args[1] >> 0) & 0xfff;

    if (wstate->other_modes.cycle_type == CYCLE_TYPE_FILL || wstate->other_modes.cycle_type == CYCLE_TYPE_COPY)
        yl |= 3;

    uint32_t xlint = (xl >> 2) & 0x3ff;
    uint32_t xhint = (xh >> 2) & 0x3ff;

    uint32_t ewdata[CMD_MAX_INTS];
    ewdata[0] = (0x3680 << 16) | yl;
    ewdata[1] = (yl << 16) | yh;
    ewdata[2] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[3] = 0;
    ewdata[4] = (xhint << 16) | ((xh & 3) << 14);
    ewdata[5] = 0;
    ewdata[6] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[7] = 0;
    memset(&ewdata[8], 0, 36 * sizeof(uint32_t));

    edgewalker_for_prims(wstate, ewdata);
}

void
rdp_set_prim_depth(struct rdp_state *wstate, const uint32_t *args)
{
    wstate->primitive_z = args[1] & (0x7fff << 16);

    wstate->primitive_delta_z = (uint16_t)(args[1]);
}

void
rdp_set_scissor(struct rdp_state *wstate, const uint32_t *args)
{
    wstate->clip.xh = (args[0] >> 12) & 0xfff;
    wstate->clip.yh = (args[0] >> 0) & 0xfff;
    wstate->clip.xl = (args[1] >> 12) & 0xfff;
    wstate->clip.yl = (args[1] >> 0) & 0xfff;

    wstate->scfield = (args[1] >> 25) & 1;
    wstate->sckeepodd = (args[1] >> 24) & 1;
}

#endif // N64VIDEO_C
