/* RISKY float32: IEEE-754 binary32 software library (C89)
 *
 * 32-bit floats fit in 'long' (2 words, er0 = r0:r1 = hi:lo).
 *     bit 31    : sign   (hi word bit 15)
 *     bits 30-23: exponent, 8 bits, bias 127
 *     bits 22-0 : mantissa, 23 bits, implicit leading 1 for normals
 */

#ifndef _FLOAT32_H
#define _FLOAT32_H

#include <stdio.h>

/* construct a float from hi and lo words */
long f32_make(int hi, int lo)
{
    long r, t;
    r = (long)hi << 16;
    t = (long)((unsigned int)lo & 0xFFFF);
    r = r | t;
    return r;
}

long f32_neg(long x) {
    long t;
    t = x >> 16;
    return f32_make((int)(t & 0xFFFF) ^ 0x8000, (int)(x & 0xFFFF));
}

/* ---- addition (all field extraction inlined) ---- */

long f32_add(long a, long b)
{
    long ra, rb, t;
    int sa, sb, ea, eb, e, sign;
    int mhi_a, mlo_a, mhi_b, mlo_b;
    int whi, wlo, shift, tmp;

    /* extract fields from a */
    t = a >> 16;
    sa = (int)(t & 0x8000);
    t = t & 0x7F80;
    ea = (int)(t >> 7);
    t = a >> 16;
    mhi_a = (int)(t & 0x7F);
    mlo_a = (int)(a & 0xFFFF);

    /* extract fields from b */
    t = b >> 16;
    sb = (int)(t & 0x8000);
    t = t & 0x7F80;
    eb = (int)(t >> 7);
    t = b >> 16;
    mhi_b = (int)(t & 0x7F);
    mlo_b = (int)(b & 0xFFFF);

    /* NaN check */
    if ((ea == 255 && (mhi_a || mlo_a)) || (eb == 255 && (mhi_b || mlo_b)))
        return f32_make(0x7FC0, 0);

    /* infinity check */
    if (ea == 255 && eb == 255) {
        if (sa != sb) return f32_make(0x7FC0, 0);
        return a;
    }
    if (ea == 255) return a;
    if (eb == 255) return b;

    /* zero check */
    if (ea == 0 && mhi_a == 0 && mlo_a == 0) {
        if (eb == 0 && mhi_b == 0 && mlo_b == 0) {
            if (sa && sb) return f32_make(0x8000, 0);
            return f32_make(0, 0);
        }
        return b;
    }
    if (eb == 0 && mhi_b == 0 && mlo_b == 0) return a;
    if (ea == 0) return b;
    if (eb == 0) return a;

    /* restore implicit leading 1 */
    mhi_a = mhi_a | 0x80;
    mhi_b = mhi_b | 0x80;

    /* make a the larger magnitude */
    if (ea < eb || (ea == eb && mhi_a < mhi_b) ||
        (ea == eb && mhi_a == mhi_b && mlo_a < mlo_b)) {
        tmp = sa; sa = sb; sb = tmp;
        tmp = ea; ea = eb; eb = tmp;
        tmp = mhi_a; mhi_a = mhi_b; mhi_b = tmp;
        tmp = mlo_a; mlo_a = mlo_b; mlo_b = tmp;
    }

    /* align smaller operand */
    shift = ea - eb;
    if (shift > 26) { mhi_b = 0; mlo_b = 0; }
    else {
        while (shift > 0) {
            mlo_b = (mlo_b >> 1) | ((mhi_b & 1) ? 0x8000 : 0);
            mhi_b = mhi_b >> 1;
            shift = shift - 1;
        }
    }

    e = ea;
    sign = sa;
    if (sa == sb) {
        /* same sign: add */
        wlo = mlo_a + mlo_b;
        whi = mhi_a + mhi_b;
        if (wlo >= 0x10000) { wlo = wlo - 0x10000; whi = whi + 1; }
        if (whi & 0x100) {
            wlo = (wlo >> 1) | ((whi & 1) ? 0x8000 : 0);
            whi = whi >> 1;
            e = e + 1;
        }
    } else {
        /* different signs: subtract */
        wlo = mlo_a - mlo_b;
        whi = mhi_a - mhi_b;
        if (wlo < 0) { wlo = wlo + 0x10000; whi = whi - 1; }
        if (whi == 0 && wlo == 0) return f32_make(0, 0);
        while ((whi & 0x80) == 0) {
            whi = (whi << 1) & 0xff;
            if (wlo & 0x8000) whi = whi | 1;
            wlo = (wlo << 1) & 0xffff;
            e = e - 1;
            if (e <= 0) return f32_make(sign, 0);
        }
    }

    if (e >= 255) return f32_make(sign | 0x7F80, 0);
    if (e <= 0) return f32_make(sign, 0);
    return f32_make(sign | (e << 7) | (whi & 0x7F), wlo);
}

long f32_sub(long a, long b) { return f32_add(a, f32_neg(b)); }

/* ---- multiply (14-bit mantissa, decent precision) ---- */

long f32_mul(long a, long b)
{
    long ta, tb, prod;
    int sa, sb, ea, eb, e, sign, mhi, mlo;
    int ma14, mb14, phi, plo;

    ta = a >> 16; tb = b >> 16;
    sa = (int)(ta & 0x8000); sb = (int)(tb & 0x8000);
    ta = ta & 0x7F80; ea = (int)(ta >> 7);
    tb = tb & 0x7F80; eb = (int)(tb >> 7);
    ta = a >> 16; tb = b >> 16;
    sign = sa ^ sb;

    /* NaN / inf / zero */
    if (ea == 255 || eb == 255) {
        if ((ea == 255 && ((int)(ta & 0x7F) || (int)(a & 0xFFFF))) ||
            (eb == 255 && ((int)(tb & 0x7F) || (int)(b & 0xFFFF))))
            return f32_make(0x7FC0, 0);
        if ((ea == 0 && (int)(ta & 0x7F) == 0 && (int)(a & 0xFFFF) == 0) ||
            (eb == 0 && (int)(tb & 0x7F) == 0 && (int)(b & 0xFFFF) == 0))
            return f32_make(0x7FC0, 0);
        return f32_make(sign | 0x7F80, 0);
    }
    if ((ea == 0 && (int)(ta & 0x7F) == 0 && (int)(a & 0xFFFF) == 0) ||
        (eb == 0 && (int)(tb & 0x7F) == 0 && (int)(b & 0xFFFF) == 0))
        return f32_make(sign, 0);
    if (ea == 0 || eb == 0) return f32_make(sign, 0);

    /* 14-bit mantissa multiply using long arithmetic */
    ma14 = (((int)(ta & 0x7F) | 0x80) << 6) | (((int)(a & 0xFFFF) >> 10) & 0x3F);
    mb14 = (((int)(tb & 0x7F) | 0x80) << 6) | (((int)(b & 0xFFFF) >> 10) & 0x3F);
    e = ea + eb - 127;
    prod = (long)ma14 * (long)mb14;  /* 28-bit product */

    /* Extract and normalize. Product has binary point after bit 27.
     * Implicit 1 should be at bit 27. */
    {
        long t;
        t = prod >> 16;
        phi = (int)(t & 0xFFFF);
        plo = (int)(prod & 0xFFFF);

        /* Find the leading 1 position in the 28-bit phi:plo */
        if (phi == 0 && plo == 0) return f32_make(sign, 0);

        /* Normalize: shift so bit 27 (0x800 in phi) is set */
        while (phi < 0x800 && e > 0) {
            phi = (phi << 1) | ((plo >> 15) & 1);
            plo = (plo << 1) & 0xFFFF;
            e = e - 1;
        }
        /* If phi >= 0x1000, shift right */
        while (phi >= 0x1000) {
            plo = (plo >> 1) | ((phi & 1) ? 0x8000 : 0);
            phi = phi >> 1;
            e = e + 1;
        }

        /* Extract 23-bit mantissa from phi:plo (bits 26-4) */
        mhi = ((phi & 0x7FF) >> 4) & 0x7F;
        mlo = ((phi & 0xF) << 12) | ((plo >> 4) & 0xFFF);
    }

    if (e >= 255) return f32_make(sign | 0x7F80, 0);
    if (e <= 0) return f32_make(sign, 0);
    return f32_make(sign | (e << 7) | mhi, mlo);
}

/* ---- divide ---- */

long f32_div(long a, long b)
{
    long ta, tb;
    int sa, sb, ea, eb, e, sign, mhi, mlo;
    int num, den, q, rem, count;

    ta = a >> 16; tb = b >> 16;
    sa = (int)(ta & 0x8000); sb = (int)(tb & 0x8000);
    ta = ta & 0x7F80; ea = (int)(ta >> 7);
    tb = tb & 0x7F80; eb = (int)(tb >> 7);
    ta = a >> 16; tb = b >> 16;
    sign = sa ^ sb;

    if (ea == 255 || eb == 255) {
        if ((ea == 255 && ((int)(ta & 0x7F) || (int)(a & 0xFFFF))) ||
            (eb == 255 && ((int)(tb & 0x7F) || (int)(b & 0xFFFF))))
            return f32_make(0x7FC0, 0);
        if (ea == 255 && eb == 255) return f32_make(0x7FC0, 0);
        if (ea == 255) return f32_make(sign | 0x7F80, 0);
        return f32_make(sign, 0);
    }
    if (eb == 0 && (int)(tb & 0x7F) == 0 && (int)(b & 0xFFFF) == 0) {
        if (ea == 0 && (int)(ta & 0x7F) == 0 && (int)(a & 0xFFFF) == 0)
            return f32_make(0x7FC0, 0);
        return f32_make(sign | 0x7F80, 0);
    }
    if (ea == 0 && (int)(ta & 0x7F) == 0 && (int)(a & 0xFFFF) == 0)
        return f32_make(sign, 0);
    if (ea == 0 || eb == 0) return f32_make(sign, 0);

    e = ea - eb + 127;

    /* 16-bit mantissa divide: num = 1.M (top 16 bits), den = 1.M (top 16 bits) */
    num = (((int)(ta & 0x7F) | 0x80) << 8) | (((int)(a & 0xFFFF) >> 8) & 0xFF);
    den = (((int)(tb & 0x7F) | 0x80) << 8) | (((int)(b & 0xFFFF) >> 8) & 0xFF);

    /* Restoring division with 13 fractional bits */
    rem = num; q = 0; count = 0;
    while (count < 13) {
        rem = rem << 1; q = q << 1;
        if (rem >= den) { rem = rem - den; q = q | 1; }
        count = count + 1;
    }

    /* Normalize quotient */
    if (q & 0x1000) {
        mhi = (q >> 5) & 0x7F;
        mlo = (q << 11) & 0xFFFF;
    } else {
        e = e - 1;
        mhi = (q >> 4) & 0x7F;
        mlo = (q << 12) & 0xFFFF;
    }

    if (e >= 255) return f32_make(sign | 0x7F80, 0);
    if (e <= 0) return f32_make(sign, 0);
    return f32_make(sign | (e << 7) | mhi, mlo);
}

/* ---- comparison ---- */

int f32_cmp(long a, long b)
{
    long ta, tb, ma, mb;
    int sa, sb;
    ta = a >> 16; tb = b >> 16;
    if ((((int)(ta & 0x7F80) >> 7) == 255 && ((int)(ta & 0x7F) || (int)(a & 0xFFFF))) ||
        (((int)(tb & 0x7F80) >> 7) == 255 && ((int)(tb & 0x7F) || (int)(b & 0xFFFF))))
        return 0;
    if (a == b) return 0;
    sa = (int)(ta & 0x8000); sb = (int)(tb & 0x8000);
    /* both zero? */
    if (((int)(ta & 0x7FFF) == 0 && (int)(a & 0xFFFF) == 0) &&
        ((int)(tb & 0x7FFF) == 0 && (int)(b & 0xFFFF) == 0))
        return 0;
    if (sa && !sb) return -1;
    if (!sa && sb) return 1;
    ma = a & 0x7FFFFFFF;
    mb = b & 0x7FFFFFFF;
    if (ma < mb) return sa ? 1 : -1;
    if (ma > mb) return sa ? -1 : 1;
    return 0;
}

/* ---- print raw hex ---- */

void f32_print(long x)
{
    int hi, lo;
    hi = (int)(x >> 16);
    lo = (int)x;
    print_string("0x");
    print_hex(hi);
    print_hex(lo);
}

#endif
