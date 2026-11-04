/*
 * The caller side: an x86 signalling NaN (0x7F800001) must survive a call
 * with it as an argument and a return of it, directly and through a function
 * pointer. With the i386 default convention the return goes through the x87
 * stack, whose load quiets it to 0x7FC00001.
 * Returns 0 when every bit pattern came back as it went in, else the index
 * (1-based) of the first case that did not, in the high byte, and the
 * pattern that came back.
 */
float CcPass(float x);
float CcLoad(const float *p);

static unsigned Bits(float f)
{
    unsigned u;
    __builtin_memcpy(&u, &f, 4);
    return u;
}

static float FromBits(unsigned u)
{
    float f;
    __builtin_memcpy(&f, &u, 4);
    return f;
}

unsigned CcRun(void)
{
    static const unsigned patterns[] = {0x7F800001u, 0xFF800001u, 0x7FA00000u, 0x00000001u, 0x7F800000u, 0x7FC00001u};
    float (*volatile pass)(float) = CcPass;
    float (*volatile load)(const float *) = CcLoad;
    unsigned i;

    for (i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        float f = FromBits(patterns[i]);
        unsigned a = Bits(CcPass(f));
        unsigned b = Bits(CcLoad(&f));
        unsigned c = Bits(pass(f));
        unsigned d = Bits(load(&f));
        if (a != patterns[i]) { return ((i * 4 + 1) << 24) | (a & 0xFFFFFF); }
        if (b != patterns[i]) { return ((i * 4 + 2) << 24) | (b & 0xFFFFFF); }
        if (c != patterns[i]) { return ((i * 4 + 3) << 24) | (c & 0xFFFFFF); }
        if (d != patterns[i]) { return ((i * 4 + 4) << 24) | (d & 0xFFFFFF); }
    }
    return 0;
}

/* The control: the same bits through a plain return of a loaded float. */
unsigned CcControlBits(void)
{
    float f = FromBits(0x7F800001u);
    return Bits(CcLoad(&f));
}
