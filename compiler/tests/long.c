#include <stdio.h>

int main(void)
{
    long a = 100000;
    long b = 3;
    long c;

    /* long arithmetic */
    c = a + b;
    print_int((int)(c & 0xFFFF)); putchar('\n');     /* 100003 & 0xFFFF */
    c = a - b;
    print_int((int)(c & 0xFFFF)); putchar('\n');     /* 99997 & 0xFFFF */
    c = a * b;
    print_int((int)(c & 0xFFFF)); putchar('\n');     /* 300000 & 0xFFFF */
    c = a / b;
    print_int((int)(c & 0xFFFF)); putchar('\n');     /* 33333 & 0xFFFF */

    /* long comparison */
    print_int(a > b); putchar('\n');      /* 1 */
    print_int(a < b); putchar('\n');      /* 0 */

    /* long from constant */
    c = 50000;
    print_int((int)(c & 0xFFFF)); putchar('\n');     /* 50000 & 0xFFFF */

    /* long addition with int promotion */
    c = a + 1000;
    print_int((int)(c & 0xFFFF)); putchar('\n');     /* 101000 & 0xFFFF */

    /* long <= and >= */
    print_int(a >= b); putchar('\n');     /* 1 */
    print_int(a <= b); putchar('\n');     /* 0 */

    return 0;
}
