#ifdef N64VIDEO_C

static STRICTINLINE void
vi_vl_lerp(struct n64video_pixel *up, struct n64video_pixel down, uint32_t frac)
{
    uint32_t r0, g0, b0;

    if (frac == 0)
        return;

    r0 = up->r;
    g0 = up->g;
    b0 = up->b;

    // ((a - b) * c + 16) >> 5 + d
    // is
    // (a - b) * c + d
    // at full precision (the +16 is round-to-nearest)
    // a,b,d are 0.8 fixed point
    // c is X.5 fixed point
    up->r = ((((down.r - r0) * frac + 16) >> 5) + r0) & 0xff;
    up->g = ((((down.g - g0) * frac + 16) >> 5) + g0) & 0xff;
    up->b = ((((down.b - b0) * frac + 16) >> 5) + b0) & 0xff;
}

#endif // N64VIDEO_C
