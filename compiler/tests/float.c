#include <stdio.h>
#include <float32.h>

float half(float x)
{
    return x / 2.0;
}

float fsquare(float x)
{
    return x * x;
}

float globalF = 1.5;
float globalNeg = -0.25;

int main(void)
{
    float a, b, c;
    float arr[3];
    int i;
    long l;

    /* literals and exact arithmetic */
    a = 1.5;
    b = 2.25;
    f32_print_hex(a + b); putchar('\n');     /* 3.75    = 0x40700000 */
    f32_print_hex(a - b); putchar('\n');     /* -0.75   = 0xbf400000 */
    f32_print_hex(a * b); putchar('\n');     /* 3.375   = 0x40580000 */
    f32_print_hex(b / a); putchar('\n');     /* 1.5     = 0x3fc00000 */

    /* unary minus, literal folding */
    c = -a;
    f32_print_hex(c); putchar('\n');         /* -1.5    = 0xbfc00000 */
    c = -1.25;
    f32_print_hex(c); putchar('\n');         /* 0xbfa00000 */

    /* comparisons */
    print_int(a < b); putchar('\n');         /* 1 */
    print_int(a > b); putchar('\n');         /* 0 */
    print_int(a == 1.5); putchar('\n');      /* 1 */
    print_int(a != b); putchar('\n');        /* 1 */
    print_int(-a < 0.0); putchar('\n');      /* 1 */
    print_int(a <= 1.5); putchar('\n');      /* 1 */
    print_int(b >= 3.0); putchar('\n');      /* 0 */

    /* int <-> float conversion */
    c = 5;
    f32_print_hex(c); putchar('\n');         /* 5.0     = 0x40a00000 */
    i = 7;
    c = (float)i;
    f32_print_hex(c); putchar('\n');         /* 7.0     = 0x40e00000 */
    i = -12;
    c = (float)i;
    f32_print_hex(c); putchar('\n');         /* -12.0   = 0xc1400000 */
    i = (int)(a * 10.0);
    print_int(i); putchar('\n');             /* 15 */
    i = (int)(-a);
    print_int(i); putchar('\n');             /* -1 */

    /* mixed int/float arithmetic */
    f32_print_hex(a + 1); putchar('\n');     /* 2.5     = 0x40200000 */
    f32_print_hex(2 * b); putchar('\n');     /* 4.5     = 0x40900000 */
    i = 3;
    f32_print_hex(a * i); putchar('\n');     /* 4.5     = 0x40900000 */

    /* compound assignment */
    c = 1.5;
    c += 2.25;
    f32_print_hex(c); putchar('\n');         /* 3.75    = 0x40700000 */
    c *= 2.0;
    f32_print_hex(c); putchar('\n');         /* 7.5     = 0x40f00000 */
    c /= 3.0;
    f32_print_hex(c); putchar('\n');         /* 2.5     = 0x40200000 */
    c -= 0.5;
    f32_print_hex(c); putchar('\n');         /* 2.0     = 0x40000000 */

    /* function params and returns */
    f32_print_hex(half(3.0)); putchar('\n'); /* 1.5     = 0x3fc00000 */
    f32_print_hex(fsquare(1.5)); putchar('\n'); /* 2.25 = 0x40100000 */

    /* globals */
    f32_print_hex(globalF); putchar('\n');   /* 1.5     = 0x3fc00000 */
    f32_print_hex(globalNeg); putchar('\n'); /* -0.25   = 0xbe800000 */
    globalF = globalF + 1.0;
    f32_print_hex(globalF); putchar('\n');   /* 2.5     = 0x40200000 */

    /* arrays of float */
    arr[0] = 0.5;
    arr[1] = arr[0] + 1.0;
    arr[2] = arr[1] * arr[1];
    f32_print_hex(arr[2]); putchar('\n');    /* 2.25    = 0x40100000 */

    /* library conversions */
    f32_print_hex(f32_from_int(1000)); putchar('\n');       /* 0x447a0000 */
    f32_print_hex(f32_from_long(100000000)); putchar('\n'); /* 0x4cbebc20 */
    print_long(f32_to_long(f32_make(0x4CBE, 0xBC20)));
    putchar('\n');                                          /* 100000000 */
    print_int(f32_to_int(f32_make(0xC2F6, 0xE979)));
    putchar('\n');                                          /* -123 */

    /* rounding (truncate toward zero): 1/3 and 0.1*10 */
    f32_print_hex(f32_div(f32_from_int(1), f32_from_int(3)));
    putchar('\n');                           /* 0x3eaaaaaa */
    f32_print_hex(f32_make(0x3DCC, 0xCCCD) * 10.0);
    putchar('\n');                           /* ~1.0 = 0x3f800000 */

    /* specials */
    f32_print_hex(f32_div(f32_from_int(1), 0)); putchar('\n');  /* inf 0x7f800000 */
    print_int(f32_isinf(f32_make(0x7F80, 0))); putchar('\n');   /* 1 */
    print_int(f32_isnan(f32_make(0x7FC0, 0))); putchar('\n');   /* 1 */
    print_int(f32_isnan(a)); putchar('\n');                     /* 0 */
    f32_print_hex(f32_abs(-a)); putchar('\n');                  /* 0x3fc00000 */

    /* decimal printing */
    f32_print(3.140625); putchar('\n');      /* 3.140625 */
    f32_print(-0.375); putchar('\n');        /* -0.375000 */
    f32_print(f32_make(0x7F80, 0)); putchar('\n');  /* inf */
    f32_print(f32_make(0xFF80, 0)); putchar('\n');  /* -inf */
    f32_print(f32_make(0x7FC0, 0)); putchar('\n');  /* nan */
    l = 0;
    f32_print(l); putchar('\n');             /* 0.000000 */

    /* float in conditions */
    c = 0.5;
    if (c)
        puts("truthy");
    c = 0.0;
    if (!c)
        puts("zero");
    while (c < 2.0) {
        c += 1.0;
    }
    f32_print(c); putchar('\n');             /* 2.000000 */

    return 0;
}
