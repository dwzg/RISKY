/* RISKY float32: IEEE-754 binary32 software library (C89)
 *
 * 32-bit floats fit in 'long' (2 words, er0 = r0:r1 = hi:lo).
 *     bit 31    : sign   (hi word bit 15)
 *     bits 30-23: exponent, 8 bits, bias 127
 *     bits 22-0 : mantissa, 23 bits, implicit leading 1 for normals
 *
 * 'long' is the carrier type: every function takes and returns float
 * bit patterns in longs, and the compiler passes 'float' values to
 * these functions bit-for-bit (long <-> float never converts
 * numerically; use f32_from_long / f32_to_long for that).
 *
 * Semantics: round toward zero (results are exact or within 1 ulp),
 * denormals flush to zero, NaN is 0x7FC00000.
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

long f32_abs(long x)
{
    return x & 0x7FFFFFFF;
}

int f32_isnan(long x)
{
    int hi = (int)(x >> 16);
    if (((hi >> 7) & 0xFF) != 255)
        return 0;
    return (hi & 0x7F) != 0 || (int)(x & 0xFFFF) != 0;
}

int f32_isinf(long x)
{
    int hi = (int)(x >> 16);
    return (hi & 0x7FFF) == 0x7F80 && (int)(x & 0xFFFF) == 0;
}

/* ---- addition (25-bit mantissas: 24 bits plus one guard bit) ---- */

long f32_add(long a, long b)
{
    int ha, hb, sa, sb, ea, eb, e, sign, tmp;
    long ma, mb, m, t;

    ha = (int)(a >> 16);
    hb = (int)(b >> 16);
    sa = ha & 0x8000;
    sb = hb & 0x8000;
    ea = (ha >> 7) & 0xFF;
    eb = (hb >> 7) & 0xFF;

    /* NaN check */
    if ((ea == 255 && ((ha & 0x7F) || (int)(a & 0xFFFF))) ||
        (eb == 255 && ((hb & 0x7F) || (int)(b & 0xFFFF))))
        return f32_make(0x7FC0, 0);

    /* infinity check */
    if (ea == 255 && eb == 255) {
        if (sa != sb) return f32_make(0x7FC0, 0);
        return a;
    }
    if (ea == 255) return a;
    if (eb == 255) return b;

    /* zero check (denormals flush to zero) */
    if (ea == 0 && eb == 0) {
        if (sa && sb) return f32_make(0x8000, 0);
        return 0;
    }
    if (ea == 0) return b;
    if (eb == 0) return a;

    /* mantissas with implicit 1 and one guard bit: [2^24, 2^25) */
    ma = (((a & 0x7FFFFF) | 0x800000)) << 1;
    mb = (((b & 0x7FFFFF) | 0x800000)) << 1;

    /* make a the larger magnitude */
    if (ea < eb || (ea == eb && ma < mb)) {
        t = ma; ma = mb; mb = t;
        tmp = ea; ea = eb; eb = tmp;
        tmp = sa; sa = sb; sb = tmp;
    }
    sign = sa;
    e = ea;

    /* align smaller operand (truncate) */
    tmp = ea - eb;
    if (tmp > 25) mb = 0;
    else mb = mb >> tmp;

    if (sa == sb) {
        m = ma + mb;                            /* < 2^26 */
        if ((int)(m >> 16) & 0x200) {           /* carry into bit 25 */
            m = m >> 1;
            e = e + 1;
        }
    } else {
        m = ma - mb;                            /* >= 0 */
        if (m == 0) return 0;
        while (((int)(m >> 16) & 0x100) == 0) { /* normalize to bit 24 */
            m = m << 1;
            e = e - 1;
            if (e <= 0) return f32_make(sign, 0);
        }
    }
    m = m >> 1;                                 /* drop the guard bit */

    if (e >= 255) return f32_make(sign | 0x7F80, 0);
    if (e <= 0) return f32_make(sign, 0);
    return f32_make(sign | (e << 7) | (int)((m >> 16) & 0x7F),
                    (int)(m & 0xFFFF));
}

long f32_sub(long a, long b) { return f32_add(a, f32_neg(b)); }

/* ---- multiply (full 24x24-bit mantissa product via 12-bit limbs) ---- */

long f32_mul(long a, long b)
{
    int ha, hb, ea, eb, e, sign;
    int ah, al, bh, bl;
    long ma, mb, p2, lowp, hi25, m;

    ha = (int)(a >> 16);
    hb = (int)(b >> 16);
    sign = (ha ^ hb) & 0x8000;
    ea = (ha >> 7) & 0xFF;
    eb = (hb >> 7) & 0xFF;

    /* NaN / inf / zero */
    if (ea == 255 || eb == 255) {
        if ((ea == 255 && ((ha & 0x7F) || (int)(a & 0xFFFF))) ||
            (eb == 255 && ((hb & 0x7F) || (int)(b & 0xFFFF))))
            return f32_make(0x7FC0, 0);
        if ((ea == 0 && (ha & 0x7F) == 0 && (int)(a & 0xFFFF) == 0) ||
            (eb == 0 && (hb & 0x7F) == 0 && (int)(b & 0xFFFF) == 0))
            return f32_make(0x7FC0, 0);         /* inf * 0 */
        return f32_make(sign | 0x7F80, 0);
    }
    if (ea == 0 || eb == 0)
        return f32_make(sign, 0);               /* zero (denormals flush) */

    ma = (a & 0x7FFFFF) | 0x800000;
    mb = (b & 0x7FFFFF) | 0x800000;
    ah = (int)(ma >> 12);
    al = (int)(ma & 0xFFF);
    bh = (int)(mb >> 12);
    bl = (int)(mb & 0xFFF);

    /* 48-bit product ma*mb = (ah*bh << 24) + ((ah*bl + al*bh) << 12)
     * + al*bl, accumulated as hi25 = product >> 24 plus a 24-bit tail */
    p2 = (long)ah * bl + (long)al * bh;
    lowp = ((p2 & 0xFFF) << 12) + (long)al * bl;
    hi25 = (long)ah * bh + (p2 >> 12) + (lowp >> 24);

    e = ea + eb - 127;
    if ((int)(hi25 >> 16) & 0x80) {
        /* product in [2,4): mantissa is product >> 24 */
        e = e + 1;
        m = hi25;
    } else {
        m = (hi25 << 1) | ((lowp >> 23) & 1);
    }

    if (e >= 255) return f32_make(sign | 0x7F80, 0);
    if (e <= 0) return f32_make(sign, 0);
    return f32_make(sign | (e << 7) | (int)((m >> 16) & 0x7F),
                    (int)(m & 0xFFFF));
}

/* ---- divide (full 24-bit restoring division) ---- */

long f32_div(long a, long b)
{
    int ha, hb, ea, eb, e, sign, i;
    long num, den, rem, q;

    ha = (int)(a >> 16);
    hb = (int)(b >> 16);
    sign = (ha ^ hb) & 0x8000;
    ea = (ha >> 7) & 0xFF;
    eb = (hb >> 7) & 0xFF;

    if (ea == 255 || eb == 255) {
        if ((ea == 255 && ((ha & 0x7F) || (int)(a & 0xFFFF))) ||
            (eb == 255 && ((hb & 0x7F) || (int)(b & 0xFFFF))))
            return f32_make(0x7FC0, 0);
        if (ea == 255 && eb == 255) return f32_make(0x7FC0, 0);
        if (ea == 255) return f32_make(sign | 0x7F80, 0);
        return f32_make(sign, 0);               /* x / inf */
    }
    if (eb == 0) {
        if (ea == 0) return f32_make(0x7FC0, 0); /* 0 / 0 */
        return f32_make(sign | 0x7F80, 0);       /* x / 0 */
    }
    if (ea == 0) return f32_make(sign, 0);

    num = (a & 0x7FFFFF) | 0x800000;
    den = (b & 0x7FFFFF) | 0x800000;
    e = ea - eb + 127;
    if (num < den) { num = num << 1; e = e - 1; }

    /* num in [den, 2*den): 24 quotient bits, MSB always 1 */
    q = 0;
    rem = num;
    for (i = 0; i < 24; i++) {
        q = q << 1;
        if (rem >= den) { rem = rem - den; q = q | 1; }
        rem = rem << 1;
    }

    if (e >= 255) return f32_make(sign | 0x7F80, 0);
    if (e <= 0) return f32_make(sign, 0);
    return f32_make(sign | (e << 7) | (int)((q >> 16) & 0x7F),
                    (int)(q & 0xFFFF));
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

/* ---- int/long conversions (truncate toward zero, saturate) ---- */

long f32_from_int(int i)
{
    int sign, e;
    long m;
    if (i == 0) return 0;
    sign = 0;
    if (i < 0) { sign = 0x8000; i = -i; }   /* -32768 wraps to 0x8000 */
    m = (long)i & 0xFFFF;                   /* magnitude 1..32768 */
    e = 127 + 23;
    while (((int)(m >> 16) & 0x80) == 0) { m = m << 1; e = e - 1; }
    return f32_make(sign | (e << 7) | (int)((m >> 16) & 0x7F),
                    (int)(m & 0xFFFF));
}

long f32_from_long(long v)
{
    int sign, e, hi;
    long m;
    if (v == 0) return 0;
    hi = (int)(v >> 16);
    sign = 0;
    if (hi & 0x8000) { sign = 0x8000; v = -v; }  /* LONG_MIN wraps to 2^31 */
    m = v;
    e = 127 + 23;
    while ((int)(m >> 16) >> 8) { m = m >> 1; e = e + 1; }
    while (((int)(m >> 16) & 0x80) == 0) { m = m << 1; e = e - 1; }
    return f32_make(sign | (e << 7) | (int)((m >> 16) & 0x7F),
                    (int)(m & 0xFFFF));
}

int f32_to_int(long x)
{
    int hi, sign, e, shift, v;
    long m;
    hi = (int)(x >> 16);
    sign = hi & 0x8000;
    e = (hi >> 7) & 0xFF;
    if (e < 127) return 0;                  /* |x| < 1 (NaN would saturate) */
    shift = e - 127;
    if (e == 255 || shift >= 15) {
        if (sign) return (int)0x8000;       /* -32768 */
        return 32767;
    }
    m = (x & 0x7FFFFF) | 0x800000;
    m = m >> (23 - shift);
    v = (int)(m & 0xFFFF);
    if (sign) return -v;
    return v;
}

long f32_to_long(long x)
{
    int hi, sign, e, shift;
    long m;
    hi = (int)(x >> 16);
    sign = hi & 0x8000;
    e = (hi >> 7) & 0xFF;
    if (e < 127) return 0;
    shift = e - 127;
    if (e == 255 || shift >= 31) {
        if (sign) return f32_make(0x8000, 0);       /* -2147483648 */
        return f32_make(0x7FFF, 0xFFFF);            /*  2147483647 */
    }
    m = (x & 0x7FFFFF) | 0x800000;
    if (shift >= 23) m = m << (shift - 23);
    else m = m >> (23 - shift);
    if (sign) return -m;
    return m;
}

/* ---- printing ---- */

/* print raw bits as 0x......... */
void f32_print_hex(long x)
{
    int hi, lo;
    hi = (int)(x >> 16);
    lo = (int)x;
    print_string("0x");
    print_hex(hi);
    print_hex(lo);
}

/* print with sign, integer part and 6 fractional digits (binary32
 * carries ~7 significant decimal digits).  Integer parts beyond the
 * long range print saturated. */
void f32_print(long x)
{
    int hi, e, digit, i;
    long ip, frac;
    hi = (int)(x >> 16);
    e = (hi >> 7) & 0xFF;
    if (e == 255) {
        if ((hi & 0x7F) || (int)(x & 0xFFFF)) {
            print_string("nan");
            return;
        }
        if (hi & 0x8000) putchar('-');
        print_string("inf");
        return;
    }
    if (hi & 0x8000) {
        putchar('-');
        x = x & 0x7FFFFFFF;
    }
    ip = f32_to_long(x);
    print_long(ip);
    putchar('.');
    frac = f32_sub(x, f32_from_long(ip));
    for (i = 0; i < 6; i++) {
        frac = f32_mul(frac, f32_from_int(10));
        digit = f32_to_int(frac);
        if (digit > 9) digit = 9;
        if (digit < 0) digit = 0;
        putchar('0' + digit);
        frac = f32_sub(frac, f32_from_int(digit));
    }
}

#endif
