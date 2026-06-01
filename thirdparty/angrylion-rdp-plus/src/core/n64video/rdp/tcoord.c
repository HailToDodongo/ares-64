#ifdef N64VIDEO_C

#define TCDIV_OVERFLOWED(tc) ((tc) & (3 << 17))

// For perspective division
// HW contains this in a ROM
static const int32_t norm_point_table[64] = {
    // clang-format off
    0x4000, 0x3F04, 0x3E10, 0x3D22, 0x3C3C, 0x3B5D, 0x3A83, 0x39B1,
    0x38E4, 0x381C, 0x375A, 0x369D, 0x35E5, 0x3532, 0x3483, 0x33D9,
    0x3333, 0x3291, 0x31F4, 0x3159, 0x30C3, 0x3030, 0x2FA1, 0x2F15,
    0x2E8C, 0x2E06, 0x2D83, 0x2D03, 0x2C86, 0x2C0B, 0x2B93, 0x2B1E,
    0x2AAB, 0x2A3A, 0x29CC, 0x2960, 0x28F6, 0x288E, 0x2828, 0x27C4,
    0x2762, 0x2702, 0x26A4, 0x2648, 0x25ED, 0x2594, 0x253D, 0x24E7,
    0x2492, 0x243F, 0x23EE, 0x239E, 0x234F, 0x2302, 0x22B6, 0x226C,
    0x2222, 0x21DA, 0x2193, 0x214D, 0x2108, 0x20C5, 0x2082, 0x2041,
    // clang-format on
};

// For perspective division
// HW contains this in a ROM
static const int32_t norm_slope_table[64] = {
    // clang-format off
    0xF03, 0xF0B, 0xF11, 0xF19, 0xF20, 0xF25, 0xF2D, 0xF32,
    0xF37, 0xF3D, 0xF42, 0xF47, 0xF4C, 0xF50, 0xF55, 0xF59,
    0xF5D, 0xF62, 0xF64, 0xF69, 0xF6C, 0xF70, 0xF73, 0xF76,
    0xF79, 0xF7C, 0xF7F, 0xF82, 0xF84, 0xF87, 0xF8A, 0xF8C,
    0xF8E, 0xF91, 0xF93, 0xF95, 0xF97, 0xF99, 0xF9B, 0xF9D,
    0xF9F, 0xFA1, 0xFA3, 0xFA4, 0xFA6, 0xFA8, 0xFA9, 0xFAA,
    0xFAC, 0xFAE, 0xFAF, 0xFB0, 0xFB2, 0xFB3, 0xFB5, 0xFB5,
    0xFB7, 0xFB8, 0xFB9, 0xFBA, 0xFBC, 0xFBC, 0xFBE, 0xFBE,
    // clang-format on
};

static void
tcdiv_persp(int32_t ss, int32_t st, int32_t sw, int32_t *sss, int32_t *sst);
static void
tcdiv_nopersp(int32_t ss, int32_t st, int32_t sw, int32_t *sss, int32_t *sst);

static void (*tcdiv_func[2])(int32_t, int32_t, int32_t, int32_t *, int32_t *) = {
    tcdiv_nopersp,
    tcdiv_persp,
};

static int32_t maskbits_table[16];
static int32_t log2table[256];
static int32_t tcdiv_table[0x8000];

static STRICTINLINE void
tcmask_copy(struct tile *tile, int32_t *S0, int32_t *S1, int32_t *S2, int32_t *S3, int32_t *T)
{
    int32_t wrap;

    if (tile->mask_s) {
        if (tile->ms) {
            int32_t swrapthreshold = tile->f.masksclamped;

            wrap = (*S0 >> swrapthreshold) & 1;
            *S0 ^= (-wrap);

            wrap = (*S1 >> swrapthreshold) & 1;
            *S1 ^= (-wrap);

            wrap = (*S2 >> swrapthreshold) & 1;
            *S2 ^= (-wrap);

            wrap = (*S3 >> swrapthreshold) & 1;
            *S3 ^= (-wrap);
        }

        int32_t maskbits_s = maskbits_table[tile->mask_s];
        *S0 &= maskbits_s;
        *S1 &= maskbits_s;
        *S2 &= maskbits_s;
        *S3 &= maskbits_s;
    }

    if (tile->mask_t) {
        if (tile->mt) {
            wrap = *T >> tile->f.masktclamped;
            wrap &= 1;
            *T ^= (-wrap);
        }
        *T &= maskbits_table[tile->mask_t];
    }
}

static STRICTINLINE void
tcshift_single(int32_t *C, int32_t *Cmax, int shift, uint16_t upperbound)
{
    int32_t coord = *C;

    // Small shift values are right shifts while large shift values
    // are right shifts by the inverse
    if (shift < 11)
        coord = SIGN16(coord) >> shift;
    else
        coord = SIGN16(coord << (16 - shift));

    *C = coord;
    *Cmax = (coord >> 3) >= upperbound;
}

static STRICTINLINE void
tcshift_cycle(struct tile *tile, int32_t *S, int32_t *T, int32_t *maxs, int32_t *maxt)
{
    tcshift_single(S, maxs, tile->shift_s, tile->sh);
    tcshift_single(T, maxt, tile->shift_t, tile->th);
}

static STRICTINLINE void
tcshift_copy(struct tile *tile, int32_t *S, int32_t *T)
{
    // Same as 1-cycle/2-cycle except the max check is not used
    int32_t maxs, maxt;
    tcshift_single(S, &maxs, tile->shift_s, tile->sh);
    tcshift_single(T, &maxt, tile->shift_t, tile->th);
}

static STRICTINLINE void
tcclamp_cycle_single(int clamp_en, int clamp_diff, int32_t *C, int32_t *CFRAC, int32_t max)
{
    if (!clamp_en)
        return; // No clamping, nothing to do

    if (max) { // >= lrs/lrt
        // Positive clamp
        *C = clamp_diff;
        *CFRAC = 0;
    } else if (*C & (1 << 11)) { // Negative
        // Negative clamp
        *C = 0;
        *CFRAC = 0;
    } else {
        // Otherwise within bounds, nothing to do
    }
}

static STRICTINLINE void
tcclamp_cycle(struct tile *tile, int32_t *S, int32_t *T, int32_t *SFRAC, int32_t *TFRAC, int32_t maxs, int32_t maxt)
{
    tcclamp_cycle_single(tile->f.clampens, tile->f.clampdiffs, S, SFRAC, maxs);
    tcclamp_cycle_single(tile->f.clampent, tile->f.clampdifft, T, TFRAC, maxt);
}

static STRICTINLINE void
tcclamp_cycle_light(struct tile *tile, int32_t *S, int32_t *T, int32_t maxs, int32_t maxt)
{
    // Ignores fraction
    int32_t FRAC;
    tcclamp_cycle_single(tile->f.clampens, tile->f.clampdiffs, S, &FRAC, maxs);
    tcclamp_cycle_single(tile->f.clampent, tile->f.clampdifft, T, &FRAC, maxt);
}

static STRICTINLINE void
tclod_4x17_to_15(int32_t scurr, int32_t snext, int32_t tcurr, int32_t tnext, int32_t previous, int32_t *lod)
{
    // 17-bit subtraction
    int dels = SIGN(snext, 17) - SIGN(scurr, 17);
    if (dels & 0x20000) // Almost abs() but 1-off for negative values
        dels = ~dels & 0x1ffff;

    int delt = SIGN(tnext, 17) - SIGN(tcurr, 17);
    if (delt & 0x20000)
        delt = ~delt & 0x1ffff;

    // delmax = max(dels, delt, previous)
    int delmax;
    delmax = MAX(dels, delt);
    delmax = MAX(previous, delmax);

    // lod is essentially max(|dsdx|, |dtdy|)

    int lod_out = delmax & 0x7fff; // 15 bits (signed 14-bit ?)
    if (delmax & 0x1C000)          // Top 3 bits of the 17-bit value set
        lod_out |= 0x4000;         // Set bit 14 (sign bit, or very distant?)

    // 15-bit
    *lod = lod_out;
}

static STRICTINLINE int32_t
tclod_tcclamp_single(int32_t tc)
{
    if (tc & (2 << 17))
        return 0x7fff;
    if (tc & (1 << 17))
        return 0x8000;

    int32_t tcmasked = tc & 0x18000;

    if (tcmasked == 0x8000)
        return 0x7fff;
    if (tcmasked == 0x10000)
        return 0x8000;

    return tc & 0xffff;
}

static STRICTINLINE void
tclod_tcclamp(int32_t *sss, int32_t *sst)
{
    *sss = tclod_tcclamp_single(*sss);
    *sst = tclod_tcclamp_single(*sst);
}

static STRICTINLINE void
lodfrac_lodtile_signals(struct rdp_state *wstate, int lodclamp, int32_t lod, uint32_t *l_tile, bool *magnify,
                        bool *distant, int32_t *lfdst)
{
    uint32_t ltil;
    bool dis;
    bool mag;
    int32_t lf;

    if ((lod & 0x4000) || lodclamp) { // lod is large or tpersp overflowed
        mag = false;
        dis = true;                                   // distant
        lf = 0xff;                                    // max lod frac
        ltil = 0;                                     // level 0
    } else if (lod < wstate->min_level || lod < 32) { // if this condition passes, lod < 32 since min_level < 32
        mag = true;
        dis = wstate->max_level == 0; // distant only if max_level is level 0
        ltil = 0;                     // level 0

        if (!wstate->other_modes.sharpen_tex_en && !wstate->other_modes.detail_tex_en) {
            lf = dis * 0xff; // either max or min lod frac
        } else {
            // take the larger of lod or min_level (5-bit) and make it 8-bit, 3 lsbits are 0
            lf = MAX(lod, wstate->min_level) << 3;
            if (wstate->other_modes.sharpen_tex_en)
                lf |= 0x100; // if sharpen, set 9th bit
        }
    } else {
        mag = false;
        ltil = log2table[(lod >> 5) & 0xff]; // lod is 15-bit, take upper 10 bits and ignore upper 2 bits

        // Distant if max_level is 0, lod is large
        dis = (wstate->max_level == 0) || ((lod & 0x6000) != 0) || (ltil >= wstate->max_level);

        if (!wstate->other_modes.sharpen_tex_en && !wstate->other_modes.detail_tex_en && dis)
            lf = 0xff; // No sharpen or detail, and distant
        else
            lf = ((lod << 3) >> ltil) & 0xff; // Right shift of at most 7, essentially (lod * 8) / 2^n
    }

    *distant = dis;
    *l_tile = ltil;
    *magnify = mag;
    *lfdst = lf;
}

static STRICTINLINE void
tclod_2cycle(struct rdp_state *wstate, int32_t *sss, int32_t *sst, int32_t s, int32_t t, int32_t w, int32_t dsinc,
             int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t *tile1, int32_t *tile2, int32_t *lod_frac)
{
    int preclamps = *sss, preclampt = *sst;

    tclod_tcclamp(sss, sst);

    // Renderer optimization, don't compute lod if it doesn't get used
    if (!wstate->other_modes.f.dolod)
        return;

    int nexts = (s + dsinc) >> 16;
    int nextt = (t + dtinc) >> 16;
    int nextw = (w + dwinc) >> 16;
    int nextys = (s + wstate->spans_dsdy) >> 16;
    int nextyt = (t + wstate->spans_dtdy) >> 16;
    int nextyw = (w + wstate->spans_dwdy) >> 16;

    wstate->tcdiv_ptr(nexts, nextt, nextw, &nexts, &nextt);
    wstate->tcdiv_ptr(nextys, nextyt, nextyw, &nextys, &nextyt);

    // Check if anything overflowed
    bool lodclamp = TCDIV_OVERFLOWED(preclamps) || TCDIV_OVERFLOWED(preclampt) || TCDIV_OVERFLOWED(nexts) ||
                    TCDIV_OVERFLOWED(nextt) || TCDIV_OVERFLOWED(nextys) || TCDIV_OVERFLOWED(nextyt);

    int32_t lod = 0;
    if (!lodclamp) {
        tclod_4x17_to_15(preclamps, nexts, preclampt, nextt, 0, &lod);
        tclod_4x17_to_15(preclamps, nextys, preclampt, nextyt, lod, &lod);
    }

    uint32_t l_tile;
    bool distant;
    bool magnify;
    lodfrac_lodtile_signals(wstate, lodclamp, lod, &l_tile, &magnify, &distant, lod_frac);
    // can't move lodfrac_lodtile_signals below tex_lod_en since lodfrac gets cycled regardless

    if (!wstate->other_modes.tex_lod_en)
        return;

    // Distant, lod either clamped or we are at or exceeded max_level
    if (distant)
        l_tile = wstate->max_level;

    if (!wstate->other_modes.detail_tex_en) {
        *tile1 = (prim_tile + l_tile) & 7;

        // This condition is sort of like a clamp at either max or min levels,
        // distant means lod clamped or the tile selected was >= max_level
        // magnify means lod < min_level or lod < 32
        if (!(distant || (!wstate->other_modes.sharpen_tex_en && magnify)))
            *tile2 = (*tile1 + 1) & 7;
        else
            *tile2 = *tile1;
    } else {
        // In detail mode, step the tile up once more if not min_level
        // Also step the other tile up once more if the first tile was stepped up and it isn't distant
        // (to avoid overstepping max_level)
        *tile1 = (prim_tile + l_tile + 0 + (!magnify)) & 7;
        *tile2 = (prim_tile + l_tile + 1 + (!magnify && !distant)) & 7;
    }
}

static STRICTINLINE void
tclod_2cycle_next(struct rdp_state *wstate, int32_t *sss, int32_t *sst, int32_t *sss2, int32_t *sst2, int32_t s,
                  int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t *t1,
                  int32_t *t2, int32_t *lod_frac, int scanline)
{
    UNUSED(s);
    UNUSED(t);
    UNUSED(w);

    int preclamps2 = *sss2, preclampt2 = *sst2;

    tclod_tcclamp(sss, sst);
    tclod_tcclamp(sss2, sst2);

    // Renderer optimization, don't compute lod if it doesn't get used
    if (!wstate->other_modes.f.dolod)
        return;

    int nextscan = scanline + 1;

    // For dtdy

    int nextys = (wstate->span[nextscan].s + wstate->spans_dsdy) >> 16;
    int nextyt = (wstate->span[nextscan].t + wstate->spans_dtdy) >> 16;
    int nextysw = (wstate->span[nextscan].w + wstate->spans_dwdy) >> 16;

    wstate->tcdiv_ptr(nextys, nextyt, nextysw, &nextys, &nextyt);

    bool lodclamp = TCDIV_OVERFLOWED(preclamps2) || TCDIV_OVERFLOWED(preclampt2) || TCDIV_OVERFLOWED(nextys) ||
                    TCDIV_OVERFLOWED(nextyt);

    int32_t lod = 0;
    if (!lodclamp)
        tclod_4x17_to_15(preclamps2, nextys, preclampt2, nextyt, 0, &lod);

    uint32_t l_tile;
    bool distant;
    bool magnify;
    lodfrac_lodtile_signals(wstate, lodclamp, lod, &l_tile, &magnify, &distant, lod_frac);

    if (!wstate->other_modes.tex_lod_en)
        return;

    if (distant)
        l_tile = wstate->max_level;

    if (!wstate->other_modes.detail_tex_en)
        *t1 = (prim_tile + l_tile) & 7;
    else
        *t1 = (prim_tile + l_tile + (!magnify)) & 7;

    // For dsdx

    int nexts = (wstate->span[nextscan].s + dsinc) >> 16;
    int nextt = (wstate->span[nextscan].t + dtinc) >> 16;
    int nextw = (wstate->span[nextscan].w + dwinc) >> 16;

    wstate->tcdiv_ptr(nexts, nextt, nextw, &nexts, &nextt);

    lodclamp = lodclamp || TCDIV_OVERFLOWED(nextt) || TCDIV_OVERFLOWED(nexts);
    if (!lodclamp)
        tclod_4x17_to_15(preclamps2, nexts, preclampt2, nextt, lod, &lod);

    int32_t dummy_lod_frac;
    lodfrac_lodtile_signals(wstate, lodclamp, lod, &l_tile, &magnify, &distant, &dummy_lod_frac);

    if (distant)
        l_tile = wstate->max_level;

    if (!wstate->other_modes.detail_tex_en)
        *t2 = (prim_tile + l_tile) & 7;
    else
        *t2 = (prim_tile + l_tile + (!magnify)) & 7;
}

static STRICTINLINE void
tclod_2cycle_notexel1(struct rdp_state *wstate, int32_t *sss, int32_t *sst, int32_t s, int32_t t, int32_t w,
                      int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t *t1)
{
    int inits = *sss, initt = *sst;
    tclod_tcclamp(sss, sst);

    if (!wstate->other_modes.f.dolod)
        return;

    int nexts = (s + dsinc) >> 16;
    int nextt = (t + dtinc) >> 16;
    int nextw = (w + dwinc) >> 16;
    int nextys = (s + wstate->spans_dsdy) >> 16;
    int nextyt = (t + wstate->spans_dtdy) >> 16;
    int nextyw = (w + wstate->spans_dwdy) >> 16;

    wstate->tcdiv_ptr(nexts, nextt, nextw, &nexts, &nextt);
    wstate->tcdiv_ptr(nextys, nextyt, nextyw, &nextys, &nextyt);

    bool lodclamp = TCDIV_OVERFLOWED(initt) || TCDIV_OVERFLOWED(nextt) || TCDIV_OVERFLOWED(inits) ||
                    TCDIV_OVERFLOWED(nexts) || TCDIV_OVERFLOWED(nextys) || TCDIV_OVERFLOWED(nextyt);

    int32_t lod = 0;
    if (!lodclamp) {
        tclod_4x17_to_15(inits, nexts, initt, nextt, 0, &lod);
        tclod_4x17_to_15(inits, nextys, initt, nextyt, lod, &lod);
    }

    uint32_t l_tile;
    bool distant;
    bool magnify;
    lodfrac_lodtile_signals(wstate, lodclamp, lod, &l_tile, &magnify, &distant, &wstate->lod_frac);

    if (!wstate->other_modes.tex_lod_en)
        return;

    if (distant)
        l_tile = wstate->max_level;

    *t1 = (prim_tile + l_tile + ((wstate->other_modes.detail_tex_en && !magnify))) & 7;
}

static STRICTINLINE void
tclod_1cycle_current(struct rdp_state *wstate, int32_t *sss, int32_t *sst, int32_t nexts, int32_t nextt, int32_t s,
                     int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline,
                     int32_t prim_tile, int32_t *t1, struct spansigs *sigs)
{
    tclod_tcclamp(sss, sst);

    if (!wstate->other_modes.f.dolod)
        return;

    int nextscan = scanline + 1;

    int fars, fart, farw;
    if (wstate->span[nextscan].validline) {
        if (sigs->endspan && sigs->longspan) {
            // peek next scanline if at the end of the current scanline, which was a long span
            fars = (wstate->span[nextscan].s + dsinc) >> 16;
            fart = (wstate->span[nextscan].t + dtinc) >> 16;
            farw = (wstate->span[nextscan].w + dwinc) >> 16;
        } else if (!(sigs->preendspan && sigs->longspan) && !(sigs->endspan && sigs->midspan)) {
            fars = (s + (dsinc << 1)) >> 16;
            fart = (t + (dtinc << 1)) >> 16;
            farw = (w + (dwinc << 1)) >> 16;
        } else {
            fars = (s - dsinc) >> 16;
            fart = (t - dtinc) >> 16;
            farw = (w - dwinc) >> 16;
        }
    } else {
        // If next scanline is invalid
        fars = (s + (dsinc << 1)) >> 16;
        fart = (t + (dtinc << 1)) >> 16;
        farw = (w + (dwinc << 1)) >> 16;
    }

    wstate->tcdiv_ptr(fars, fart, farw, &fars, &fart);

    bool lodclamp =
        TCDIV_OVERFLOWED(fart) || TCDIV_OVERFLOWED(nextt) || TCDIV_OVERFLOWED(fars) || TCDIV_OVERFLOWED(nexts);

    int32_t lod = 0;
    if (!lodclamp)
        tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

    uint32_t l_tile;
    bool distant;
    bool magnify;
    lodfrac_lodtile_signals(wstate, lodclamp, lod, &l_tile, &magnify, &distant, &wstate->lod_frac);

    if (!wstate->other_modes.tex_lod_en)
        return;

    if (distant)
        l_tile = wstate->max_level;

    *t1 = (prim_tile + l_tile + (wstate->other_modes.detail_tex_en && !magnify)) & 7;
}

static STRICTINLINE void
tclod_1cycle_current_simple(struct rdp_state *wstate, int32_t *sss, int32_t *sst, int32_t s, int32_t t, int32_t w,
                            int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile,
                            int32_t *t1, struct spansigs *sigs)
{
    tclod_tcclamp(sss, sst);

    if (!wstate->other_modes.f.dolod)
        return;

    int nextscan = scanline + 1;

    int fars, fart, farw, nexts, nextt, nextw;
    if (wstate->span[nextscan].validline) {
        if (!sigs->endspan || !sigs->longspan) {
            nexts = (s + dsinc) >> 16;
            nextt = (t + dtinc) >> 16;
            nextw = (w + dwinc) >> 16;

            if (!(sigs->preendspan && sigs->longspan) && !(sigs->endspan && sigs->midspan)) {
                fars = (s + (dsinc << 1)) >> 16;
                fart = (t + (dtinc << 1)) >> 16;
                farw = (w + (dwinc << 1)) >> 16;
            } else {
                fars = (s - dsinc) >> 16;
                fart = (t - dtinc) >> 16;
                farw = (w - dwinc) >> 16;
            }
        } else {
            nexts = wstate->span[nextscan].s >> 16;
            nextt = wstate->span[nextscan].t >> 16;
            nextw = wstate->span[nextscan].w >> 16;
            fars = (wstate->span[nextscan].s + dsinc) >> 16;
            fart = (wstate->span[nextscan].t + dtinc) >> 16;
            farw = (wstate->span[nextscan].w + dwinc) >> 16;
        }
    } else {
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
        nextw = (w + dwinc) >> 16;
        fars = (s + (dsinc << 1)) >> 16;
        fart = (t + (dtinc << 1)) >> 16;
        farw = (w + (dwinc << 1)) >> 16;
    }

    wstate->tcdiv_ptr(nexts, nextt, nextw, &nexts, &nextt);
    wstate->tcdiv_ptr(fars, fart, farw, &fars, &fart);

    bool lodclamp =
        TCDIV_OVERFLOWED(fart) || TCDIV_OVERFLOWED(nextt) || TCDIV_OVERFLOWED(fars) || TCDIV_OVERFLOWED(nexts);

    int32_t lod = 0;
    if (!lodclamp)
        tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

    uint32_t l_tile;
    bool distant;
    bool magnify;
    lodfrac_lodtile_signals(wstate, lodclamp, lod, &l_tile, &magnify, &distant, &wstate->lod_frac);

    if (!wstate->other_modes.tex_lod_en)
        return;

    if (distant)
        l_tile = wstate->max_level;

    *t1 = (prim_tile + l_tile + (wstate->other_modes.detail_tex_en && !magnify)) & 7;
}

static STRICTINLINE void
tclod_1cycle_next(struct rdp_state *wstate, int32_t *sss, int32_t *sst, int32_t s, int32_t t, int32_t w, int32_t dsinc,
                  int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t *t1, struct spansigs *sigs,
                  int32_t *prelodfrac)
{
    tclod_tcclamp(sss, sst);

    if (!wstate->other_modes.f.dolod)
        return;

    int nextscan = scanline + 1;

    int nexts, nextt, nextw, fars, fart, farw;
    if (wstate->span[nextscan].validline) {
        if (!sigs->nextspan) {
            if (!sigs->endspan || !sigs->longspan) {
                nexts = (s + dsinc) >> 16;
                nextt = (t + dtinc) >> 16;
                nextw = (w + dwinc) >> 16;

                if (!(sigs->preendspan && sigs->longspan) && !(sigs->endspan && sigs->midspan)) {
                    fars = (s + (dsinc << 1)) >> 16;
                    fart = (t + (dtinc << 1)) >> 16;
                    farw = (w + (dwinc << 1)) >> 16;
                } else {
                    fars = (s - dsinc) >> 16;
                    fart = (t - dtinc) >> 16;
                    farw = (w - dwinc) >> 16;
                }
            } else {
                nexts = wstate->span[nextscan].s;
                nextt = wstate->span[nextscan].t;
                nextw = wstate->span[nextscan].w;
                fars = (nexts + dsinc) >> 16;
                fart = (nextt + dtinc) >> 16;
                farw = (nextw + dwinc) >> 16;
                nexts >>= 16;
                nextt >>= 16;
                nextw >>= 16;
            }
        } else {

            if (sigs->longspan) {
                nexts = (wstate->span[nextscan].s + dsinc) >> 16;
                nextt = (wstate->span[nextscan].t + dtinc) >> 16;
                nextw = (wstate->span[nextscan].w + dwinc) >> 16;
                fars = (wstate->span[nextscan].s + (dsinc << 1)) >> 16;
                fart = (wstate->span[nextscan].t + (dtinc << 1)) >> 16;
                farw = (wstate->span[nextscan].w + (dwinc << 1)) >> 16;
            } else if (sigs->midspan) {
                nexts = wstate->span[nextscan].s >> 16;
                nextt = wstate->span[nextscan].t >> 16;
                nextw = wstate->span[nextscan].w >> 16;
                fars = (wstate->span[nextscan].s + dsinc) >> 16;
                fart = (wstate->span[nextscan].t + dtinc) >> 16;
                farw = (wstate->span[nextscan].w + dwinc) >> 16;
            } else if (sigs->onelessthanmid) {
                nexts = (s + dsinc) >> 16;
                nextt = (t + dtinc) >> 16;
                nextw = (w + dwinc) >> 16;
                fars = (s - dsinc) >> 16;
                fart = (t - dtinc) >> 16;
                farw = (w - dwinc) >> 16;
            } else {
                nexts = (s + dsinc) >> 16;
                nextt = (t + dtinc) >> 16;
                nextw = (w + dwinc) >> 16;
                fars = (s + (dsinc << 1)) >> 16;
                fart = (t + (dtinc << 1)) >> 16;
                farw = (w + (dwinc << 1)) >> 16;
            }
        }
    } else {
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
        nextw = (w + dwinc) >> 16;
        farw = (w + (dwinc << 1)) >> 16;
        fars = (s + (dsinc << 1)) >> 16;
        fart = (t + (dtinc << 1)) >> 16;
    }

    wstate->tcdiv_ptr(nexts, nextt, nextw, &nexts, &nextt);
    wstate->tcdiv_ptr(fars, fart, farw, &fars, &fart);

    bool lodclamp =
        TCDIV_OVERFLOWED(fart) || TCDIV_OVERFLOWED(nextt) || TCDIV_OVERFLOWED(fars) || TCDIV_OVERFLOWED(nexts);

    int32_t lod = 0;
    if (!lodclamp)
        tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

    uint32_t l_tile;
    bool distant;
    bool magnify;
    lodfrac_lodtile_signals(wstate, lodclamp, lod, &l_tile, &magnify, &distant, prelodfrac);

    if (!wstate->other_modes.tex_lod_en)
        return;

    if (distant)
        l_tile = wstate->max_level;

    *t1 = (prim_tile + l_tile + (wstate->other_modes.detail_tex_en && !magnify)) & 7;
}

static STRICTINLINE void
tclod_copy(struct rdp_state *wstate, int32_t *sss, int32_t *sst, int32_t s, int32_t t, int32_t w, int32_t dsinc,
           int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t *t1)
{
    tclod_tcclamp(sss, sst);

    if (!wstate->other_modes.tex_lod_en)
        return;

    int nexts = (s + dsinc) >> 16;
    int nextt = (t + dtinc) >> 16;
    int nextw = (w + dwinc) >> 16;
    int fars = (s + (dsinc << 1)) >> 16;
    int fart = (t + (dtinc << 1)) >> 16;
    int farw = (w + (dwinc << 1)) >> 16;

    wstate->tcdiv_ptr(nexts, nextt, nextw, &nexts, &nextt);
    wstate->tcdiv_ptr(fars, fart, farw, &fars, &fart);

    bool lodclamp =
        TCDIV_OVERFLOWED(fart) || TCDIV_OVERFLOWED(nextt) || TCDIV_OVERFLOWED(fars) || TCDIV_OVERFLOWED(nexts);

    int32_t lod = 0;
    if (!lodclamp)
        tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

    uint32_t l_tile;
    bool distant;
    bool magnify;
    if ((lod & 0x4000) || lodclamp) {
        l_tile = wstate->max_level;
        distant = false;
        magnify = false;
    } else if (lod < 32) {
        l_tile = 0;
        distant = false;
        magnify = true;
    } else {
        l_tile = log2table[(lod >> 5) & 0xff];
        distant = (!wstate->max_level) || (lod & 0x6000) || (l_tile >= wstate->max_level);
        magnify = false;

        if (distant)
            l_tile = wstate->max_level;
    }

    *t1 = (prim_tile + l_tile + (wstate->other_modes.detail_tex_en && !magnify)) & 7;
}

static STRICTINLINE void
tc_pipeline_copy(struct tile *tile, int32_t *sss0, int32_t *sss1, int32_t *sss2, int32_t *sss3, int32_t *sst)
{
    int ss0, ss1, ss2, ss3;
    int st;

    ss0 = *sss0;
    st = *sst;
    tcshift_copy(tile, &ss0, &st);

    ss0 = TRELATIVE(ss0, tile->sl) >> 5;
    st = TRELATIVE(st, tile->tl) >> 5;

    ss0 = ss0 + 0;
    ss1 = ss0 + 1;
    ss2 = ss0 + 2;
    ss3 = ss0 + 3;

    tcmask_copy(tile, &ss0, &ss1, &ss2, &ss3, &st);

    *sss0 = ss0;
    *sss1 = ss1;
    *sss2 = ss2;
    *sss3 = ss3;
    *sst = st;
}

static STRICTINLINE void
tc_pipeline_load(struct tile *tile, int32_t *sss, int32_t *sst, int coord_quad)
{
    int sss1 = *sss;
    int sst1 = *sst;

    sss1 = SIGN16(sss1);
    sst1 = SIGN16(sst1);

    sss1 = TRELATIVE(sss1, tile->sl);
    sst1 = TRELATIVE(sst1, tile->tl);

    sss1 = sss1 >> (3 + 2 * (!coord_quad));
    sst1 = sst1 >> (3 + 2 * (!coord_quad));

    *sss = sss1;
    *sst = sst1;
}

static void
tcdiv_nopersp(int32_t ss, int32_t st, int32_t sw, int32_t *sss, int32_t *sst)
{
    UNUSED(sw);

    *sss = (SIGN16(ss)) & 0x1ffff;
    *sst = (SIGN16(st)) & 0x1ffff;
}

static void
tcdiv_persp_one(int16_t C, int shift, int rcp, int temp_mask, bool W_carry, int32_t *C_out)
{
    int prod = C * rcp;
    int out_of_bounds = prod & temp_mask;
    int overflow = 0;
    int32_t temp;

    if (shift != 14)
        temp = prod >>= (13 - shift);
    else // 13 - 14 = -1 so shift left by 1
        temp = prod << 1;

    // compute overflow
    if (out_of_bounds != temp_mask && out_of_bounds != 0)
        overflow = (prod & (1 << 29)) ? (1 << 17) : (2 << 17);

    if (W_carry)
        overflow |= 2 << 17;

    *C_out = overflow | (temp & 0x1ffff);
}

static void
tcdiv_persp(int32_t S, int32_t T, int32_t W, int32_t *S_out, int32_t *T_out)
{
    int W_carry = W <= 0;
    int shift = tcdiv_table[W & 0x7FFF] & 0xF;
    int rcp = tcdiv_table[W & 0x7FFF] >> 4;
    int temp_mask = ((1 << 30) - 1) & -((1 << 29) >> shift);

    tcdiv_persp_one(S, shift, rcp, temp_mask, W_carry, S_out);
    tcdiv_persp_one(T, shift, rcp, temp_mask, W_carry, T_out);
}

static void
tcoord_init_lut(void)
{
    int i, k;

    log2table[0] = log2table[1] = 0;
    for (i = 2; i < 256; i++) {
        for (k = 7; k > 0; k--) {
            if ((i >> k) & 1) {
                log2table[i] = k;
                break;
            }
        }
    }

    int temppoint, tempslope;
    int normout;
    int wnorm;
    int shift, tlu_rcp;

    for (i = 0; i < 0x8000; i++) {
        for (k = 1; k <= 14 && !((i << k) & 0x8000); k++)
            ;
        shift = k - 1;
        normout = (i << shift) & 0x3fff;
        wnorm = (normout & 0xff) << 2;
        normout >>= 8;

        temppoint = norm_point_table[normout];
        tempslope = norm_slope_table[normout];

        tempslope = (tempslope | ~0x3ff) + 1;

        tlu_rcp = (((tempslope * wnorm) >> 10) + temppoint) & 0x7fff;

        tcdiv_table[i] = shift | (tlu_rcp << 4);
    }

    maskbits_table[0] = 0x3ff;
    for (i = 1; i < 16; i++)
        maskbits_table[i] = ((uint16_t)(0xffff) >> (16 - i)) & 0x3ff;
}

static void
tcoord_init(struct rdp_state *wstate)
{
    wstate->tcdiv_ptr = tcdiv_func[0];
}

#endif // N64VIDEO_C
