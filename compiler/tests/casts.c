#include <stdio.h>

typedef struct { int a; int b; } Two;

int main(void)
{
    int n = 300;
    char c;
    unsigned int u = 7;
    unsigned short us = 3;
    long big = 1234;
    short s = -5;
    int *p;
    void *vp;
    Two t;

    c = (char) n;
    print_int(c); putchar('\n');            /* 300: chars are 16 bit words */

    print_int((int) 'A'); putchar('\n');    /* 65 */
    print_int(u + us); putchar('\n');       /* 10 */
    print_int((int) big); putchar('\n');    /* 1234 */
    print_int(s * 2); putchar('\n');        /* -10 */

    t.a = 11; t.b = 22;
    p = (int *) &t;
    vp = (void *) p;
    p = (int *) vp;
    print_int(p[0]); print_int(p[1]); putchar('\n');   /* 1122 */

    print_int(sizeof(char)); print_int(sizeof(int));
    print_int(sizeof(int *)); print_int(sizeof(Two));
    putchar('\n');                          /* 1112 */
    print_int(sizeof n); putchar('\n');     /* 1 */
    print_int(sizeof(long)); putchar('\n'); /* 1 */

    n = 32767;
    n++;
    print_int(n); putchar('\n');            /* -32768 (wraparound) */
    return 0;
}
