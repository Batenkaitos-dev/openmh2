/* The callee side of the calling-convention test: floats in and out, in the game's convention. */
float CcPass(float x)
{
    return x;
}

float CcLoad(const float *p)
{
    return *p;
}
