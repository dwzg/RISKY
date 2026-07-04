/* RISKY software floating point: IEEE-754 half precision (binary16)
 *
 * The RISKY CPU has no FPU, so floating point must be emulated in
 * software.  binary16 is the natural choice here: it is exactly 16 bits,
 * so one float fits in a single machine word and a "float" is just an
 * int holding the bit pattern.
 *
 *     bit 15    : sign
 *     bits 14-10: exponent, 5 bits, bias 15
 *     bits  9-0 : mantissa, 10 bits (implicit leading 1 for normals)
 *
 * Supported: +/-0, normal numbers, +/-inf, NaN, and all four basic
 * operations plus comparison, int<->float conversion and printing.
 * Subnormals are flushed to zero and results are truncated toward zero
 * (no round-to-nearest) - both are documented simplifications, not bugs.
 *
 * All values are passed and returned as plain int (the raw bit pattern).
 * Use f16_from_int / f16_to_int to convert, f16_print to display.
 */

#ifndef _SOFTFLOAT_H
#define _SOFTFLOAT_H

#include <stdio.h>

#define F16_INF  0x7c00
#define F16_NINF 0xfc00
#define F16_NAN  0x7e00
#define F16_ZERO 0x0000
#define F16_ONE  0x3c00

/* high 16 bits of an unsigned 16x16 -> 32 bit product, via the CPU's
 * hardware hmul instruction (the C '*' operator only yields the low 16
 * bits).  Both mantissas are < 0x800, so the product is at most 22 bits
 * and this returns the top 6. */
int f16_mulhi(int a, int b)
{
    int result;
    /* hmul of the two arguments into the result register, then store */
    __asm__("\n\tldo r0,*r15,#3"
            "\n\tldo r1,*r15,#4"
            "\n\thmul r0,r0,r1"
            "\n\tstoo *r15,#0,r0");      /* store r0 into local 'result' */
    return result;
}

int f16_neg(int a)
{
    if ((a & 0x7fff) == 0)
        return a;           /* leave NaN/zero sign alone where sensible */
    return a ^ 0x8000;
}

int f16_from_int(int n)
{
    int sign = 0;
    int e;
    int m;
    int msb;
    int t;

    if (n == 0)
        return 0;
    if (n < 0) {
        sign = 0x8000;
        n = -n;
    }

    /* position of the most significant set bit */
    msb = 0;
    t = n;
    while (t > 1) {
        t = t >> 1;
        msb++;
    }

    e = 15 + msb;                       /* biased exponent */
    if (msb > 10)
        m = n >> (msb - 10);            /* drop low bits (truncate) */
    else
        m = n << (10 - msb);

    return sign | (e << 10) | (m & 0x3ff);
}

int f16_to_int(int x)
{
    int sign = x & 0x8000;
    int e = (x >> 10) & 0x1f;
    int m = x & 0x3ff;
    int shift;
    int r;

    if (e == 0)
        return 0;                       /* zero or subnormal */
    if (e == 31)
        return sign ? -32768 : 32767;   /* inf / nan: clamp */

    m = m | 0x400;                      /* restore implicit bit, value = m * 2^(e-25) */
    shift = e - 25;
    if (shift >= 0)
        r = m << shift;
    else
        r = m >> (-shift);
    return sign ? -r : r;
}

int f16_add(int a, int b)
{
    int sa = a & 0x8000;
    int sb = b & 0x8000;
    int ea = (a >> 10) & 0x1f;
    int eb = (b >> 10) & 0x1f;
    int ma = a & 0x3ff;
    int mb = b & 0x3ff;
    int wa;
    int wb;
    int shift;
    int w;
    int e;
    int sign;
    int t;

    /* NaN / infinity */
    if ((ea == 31 && ma) || (eb == 31 && mb))
        return F16_NAN;
    if (ea == 31 && eb == 31) {
        if (sa != sb)
            return F16_NAN;             /* inf + -inf */
        return a;
    }
    if (ea == 31)
        return a;
    if (eb == 31)
        return b;

    /* zero (and subnormal, which we flush to zero) */
    if (ea == 0 && ma == 0) {
        if (eb == 0 && mb == 0)
            return sa & sb;             /* -0 only if both -0 */
        return b;
    }
    if (eb == 0 && mb == 0)
        return a;
    if (ea == 0)
        return b;
    if (eb == 0)
        return a;

    ma = ma | 0x400;
    mb = mb | 0x400;

    /* make a the operand with the larger magnitude */
    if (ea < eb || (ea == eb && ma < mb)) {
        t = sa; sa = sb; sb = t;
        t = ea; ea = eb; eb = t;
        t = ma; ma = mb; mb = t;
    }

    wa = ma << 3;                       /* Q13 with 3 guard bits */
    wb = mb << 3;
    shift = ea - eb;
    if (shift > 13)
        wb = 0;
    else
        wb = wb >> shift;

    sign = sa;
    e = ea;
    if (sa == sb) {
        w = wa + wb;
        if (w & 0x4000) {               /* carry out of bit 13 */
            w = w >> 1;
            e = e + 1;
        }
    } else {
        w = wa - wb;
        if (w == 0)
            return 0;                   /* exact cancellation */
        while ((w & 0x2000) == 0) {     /* renormalize left */
            w = w << 1;
            e = e - 1;
            if (e <= 0)
                return sign;            /* underflow */
        }
    }

    if (e >= 31)
        return sign | 0x7c00;           /* overflow -> inf */
    if (e <= 0)
        return sign;
    return sign | (e << 10) | ((w >> 3) & 0x3ff);
}

int f16_sub(int a, int b)
{
    return f16_add(a, f16_neg(b));
}

int f16_mul(int a, int b)
{
    int sign = (a ^ b) & 0x8000;
    int ea = (a >> 10) & 0x1f;
    int eb = (b >> 10) & 0x1f;
    int ma = a & 0x3ff;
    int mb = b & 0x3ff;
    int e;
    int phi;
    int plo;
    int w;

    if ((ea == 31 && ma) || (eb == 31 && mb))
        return F16_NAN;
    if (ea == 31 || eb == 31) {
        if ((ea == 0 && ma == 0) || (eb == 0 && mb == 0))
            return F16_NAN;             /* inf * 0 */
        return sign | 0x7c00;
    }
    if ((ea == 0 && ma == 0) || (eb == 0 && mb == 0))
        return sign;                    /* signed zero */
    if (ea == 0 || eb == 0)
        return sign;                    /* subnormal flush */

    ma = ma | 0x400;
    mb = mb | 0x400;
    e = ea + eb - 15;

    phi = f16_mulhi(ma, mb);            /* high 6 bits of the product */
    plo = ma * mb;                      /* low 16 bits */

    if (phi & 0x20) {                   /* product >= 2^21: leading bit 21 */
        w = (phi << 5) | ((plo >> 11) & 0x1f);
        e = e + 1;
    } else {                            /* leading bit 20 */
        w = (phi << 6) | ((plo >> 10) & 0x3f);
    }

    if (e >= 31)
        return sign | 0x7c00;
    if (e <= 0)
        return sign;
    return sign | (e << 10) | (w & 0x3ff);
}

int f16_div(int a, int b)
{
    int sign = (a ^ b) & 0x8000;
    int ea = (a >> 10) & 0x1f;
    int eb = (b >> 10) & 0x1f;
    int ma = a & 0x3ff;
    int mb = b & 0x3ff;
    int e;
    int rem;
    int q;
    int count;
    int m;

    if ((ea == 31 && ma) || (eb == 31 && mb))
        return F16_NAN;
    if (ea == 31 && eb == 31)
        return F16_NAN;                 /* inf / inf */
    if (ea == 31)
        return sign | 0x7c00;           /* inf / finite */
    if (eb == 31)
        return sign;                    /* finite / inf = 0 */
    if (eb == 0 && mb == 0) {           /* division by zero */
        if (ea == 0 && ma == 0)
            return F16_NAN;             /* 0 / 0 */
        return sign | 0x7c00;           /* x / 0 = inf */
    }
    if (ea == 0 && ma == 0)
        return sign;                    /* 0 / x = 0 */
    if (ea == 0)
        return sign;                    /* subnormal flush */
    if (eb == 0)
        return sign | 0x7c00;

    ma = ma | 0x400;
    mb = mb | 0x400;
    e = ea - eb + 15;

    /* Restoring division computing q = floor(ma * 2^12 / mb), a value in
     * [2^11, 2^13).  Both mantissas are in [2^10, 2^11), so the quotient's
     * integer part is 0 or 1 (ma < 2*mb).  The fractional loop needs its
     * running remainder to start below mb, so peel the integer bit off
     * first - without this, ma >= mb makes the remainder grow instead of
     * shrink and the quotient comes out garbage. */
    if (ma >= mb)
        rem = ma - mb;                  /* reduce; add the integer bit below */
    else
        rem = ma;
    q = 0;
    count = 0;
    while (count < 12) {
        rem = rem << 1;
        q = q << 1;
        if (rem >= mb) {
            rem = rem - mb;
            q = q | 1;
        }
        count++;
    }
    if (ma >= mb)
        q = q | 0x1000;                 /* restore the integer bit (position 12) */

    if (q & 0x1000) {                   /* quotient >= 1: leading bit 12 */
        m = q >> 2;
    } else {                            /* quotient in [0.5, 1) */
        m = q >> 1;
        e = e - 1;
    }

    if (e >= 31)
        return sign | 0x7c00;
    if (e <= 0)
        return sign;
    return sign | (e << 10) | (m & 0x3ff);
}

/* -1 if a < b, 0 if equal, 1 if a > b; unordered (NaN) reports 0 */
int f16_cmp(int a, int b)
{
    int ea = (a >> 10) & 0x1f;
    int eb = (b >> 10) & 0x1f;
    int va;
    int vb;

    if ((ea == 31 && (a & 0x3ff)) || (eb == 31 && (b & 0x3ff)))
        return 0;                       /* NaN: unordered */
    if ((a & 0x7fff) == 0 && (b & 0x7fff) == 0)
        return 0;                       /* +0 == -0 */
    if (a == b)
        return 0;

    /* map sign-magnitude to a monotonic ordering */
    va = (a & 0x8000) ? (0 - (a & 0x7fff)) : (a & 0x7fff);
    vb = (b & 0x8000) ? (0 - (b & 0x7fff)) : (b & 0x7fff);
    return va < vb ? -1 : 1;
}

int f16_isnan(int x)
{
    return ((x >> 10) & 0x1f) == 31 && (x & 0x3ff) != 0;
}

int f16_isinf(int x)
{
    return (x & 0x7fff) == 0x7c00;
}

/* print with sign, integer part and 4 fractional digits (binary16 only
 * carries ~3 significant decimal digits, so the last one is noisy) */
void f16_print(int x)
{
    int sign = x & 0x8000;
    int e = (x >> 10) & 0x1f;
    int m = x & 0x3ff;
    int ip;
    int frac;
    int digit;
    int i;

    if (e == 31) {
        if (m)
            print_string("nan");
        else {
            if (sign)
                putchar('-');
            print_string("inf");
        }
        return;
    }

    if (sign) {
        putchar('-');
        x = x & 0x7fff;
    }

    ip = f16_to_int(x);
    print_int(ip);
    putchar('.');

    frac = f16_sub(x, f16_from_int(ip));
    for (i = 0; i < 4; i++) {
        frac = f16_mul(frac, f16_from_int(10));
        digit = f16_to_int(frac);
        if (digit > 9)
            digit = 9;
        if (digit < 0)
            digit = 0;
        putchar('0' + digit);
        frac = f16_sub(frac, f16_from_int(digit));
    }
}

#endif
