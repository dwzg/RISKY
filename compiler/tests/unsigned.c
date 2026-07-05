#include <stdio.h>

int main(void)
{
    unsigned int a = 32768;    /* 0x8000 */
    unsigned int b = 65535;    /* 0xFFFF */
    unsigned int zero = 0;
    int c = 0x8000;            /* signed -32768 */

    /* unsigned comparison: 32768 > 0 should be true */
    print_int(a > 0); putchar('\n');

    /* unsigned comparison: 65535 > 32768 should be true */
    print_int(b > a); putchar('\n');

    /* unsigned comparison: 0 < 32768 should be true */
    print_int(zero < a); putchar('\n');

    /* unsigned division: 65535 / 2 = 32767 (not -1 / 2 = 0) */
    print_int(b / 2); putchar('\n');

    /* unsigned modulo: 65535 % 3 = 0 (65535 = 21845*3, exactly) */
    print_int(b % 3); putchar('\n');

    /* signed comparison for contrast: -32768 < 0 should be true */
    print_int(c < 0); putchar('\n');

    /* unsigned right shift: 0x8000 >> 1 = 0x4000 = 16384 */
    print_int(a >> 1); putchar('\n');

    /* unsigned <= */
    print_int(a <= b); putchar('\n');

    /* unsigned >= */
    print_int(b >= a); putchar('\n');

    return 0;
}
