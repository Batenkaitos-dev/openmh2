/* CRI ADX's decoder: the 4-bit ADPCM of an ADX stream (header encoding 3),
 * for snd's ADXT (api/adxt.c). The platform layer's own (plan 4.3: a library
 * function's behaviour at its interface), written from the format's public
 * description (the header: 0x8000, the data's offset - 4, the encoding, the
 * frame's bytes, the sample bits, the channels, the rate, the samples, the
 * high-pass cutoff; then frames of 18 bytes per channel, interleaved: a
 * big-endian 16-bit scale, then 32 signed nibbles, high nibble first), and
 * observed bit for bit at the original's SIF: the PCM the EE sends into the
 * IOP's rings on the village route (AFS00's file 0x2F: 106 transfers, 0x17400
 * bytes per channel to VBlank 2800, every one equal). What the observation
 * fixed:
 *  - the coefficients: c1 = (int)(c * 8192), c2 = (int)(c * c * -4096),
 *    truncated (not floored or rounded), with
 *    a = sqrt(2) - cos(2 pi cutoff / rate), b = sqrt(2) - 1,
 *    c = (a - sqrt((a + b) (a - b))) / b;
 *    the disc's ADX files all have one pair (48000 Hz, cutoff 500: 7400 and
 *    -3342), the same in single and double precision;
 *  - the scale is the frame's word plus one;
 *  - a sample is nibble * scale + ((c1 * h1 + c2 * h2) >> 12), the shift
 *    arithmetic, clamped to 16 bits (no other rounding), h1 and h2 the
 *    channel's last two samples as clamped. */
#include "common.h"
#include "snd/snd.h"

/* cos by its series, in double: the same digits on every host (no libm). */
static double Cos(double x)
{
    double term = 1.0, sum = 1.0;
    for (int k = 1; k < 30; k++) {
        term *= -x * x / (double)((2 * k - 1) * (2 * k));
        sum += term;
    }
    return sum;
}

/* sqrt by Newton's method from above, in double (no libm). */
static double Sqrt(double v)
{
    double x = v > 1.0 ? v : 1.0, y;
    if (v <= 0.0) {
        return 0.0;
    }
    for (;;) {
        y = 0.5 * (x + v / x);
        if (y >= x) {
            return x;
        }
        x = y;
    }
}

void AdxDec_Coefs(u32 cutoff, u32 rate, s32 *c1, s32 *c2)
{
    const double pi = 3.14159265358979323846;
    double s2 = Sqrt(2.0);
    double a, b, c;

    if (rate == 0) {
        *c1 = 0;
        *c2 = 0;
        return;
    }
    a = s2 - Cos(2.0 * pi * (double)cutoff / (double)rate);
    b = s2 - 1.0;
    c = (a - Sqrt((a + b) * (a - b))) / b;
    *c1 = (s32)(c * 8192.0);
    *c2 = (s32)(c * c * -4096.0);
}

void AdxDec_Frame(const u8 *frame, s16 *out, s32 c1, s32 c2, s32 *h1, s32 *h2)
{
    s32 scale = ((s32)frame[0] << 8 | frame[1]) + 1;
    s32 p1 = *h1, p2 = *h2;

    for (s32 i = 0; i < 32; i++) {
        u8 byte = frame[2 + i / 2];
        s32 nib = (i & 1) ? (byte & 15) : (byte >> 4);
        s32 s;
        if (nib >= 8) {
            nib -= 16;
        }
        s = nib * scale + ((c1 * p1 + c2 * p2) >> 12);
        if (s > 32767) {
            s = 32767;
        } else if (s < -32768) {
            s = -32768;
        }
        out[i] = (s16)s;
        p2 = p1;
        p1 = s;
    }
    *h1 = p1;
    *h2 = p2;
}
