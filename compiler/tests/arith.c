#include <stdio.h>

int main(void)
{
    int a = 17;
    int b = 5;

    print_int(a + b); putchar('\n');
    print_int(a - b); putchar('\n');
    print_int(a * b); putchar('\n');
    print_int(a / b); putchar('\n');
    print_int(a % b); putchar('\n');
    print_int(-a); putchar('\n');
    print_int(a + b * 2); putchar('\n');
    print_int((a + b) * 2); putchar('\n');
    print_int(1 << 10); putchar('\n');
    print_int(1024 >> 3); putchar('\n');
    print_int(0xff & 0x0f); putchar('\n');
    print_int(0xf0 | 0x0f); putchar('\n');
    print_int(0xff ^ 0x0f); putchar('\n');
    print_int(~0); putchar('\n');
    print_int(!0); putchar('\n');
    print_int(!42); putchar('\n');

    a += 3; print_int(a); putchar('\n');
    a -= 10; print_int(a); putchar('\n');
    a *= 2; print_int(a); putchar('\n');
    a /= 4; print_int(a); putchar('\n');
    a %= 3; print_int(a); putchar('\n');
    a = 6; a <<= 2; print_int(a); putchar('\n');
    a >>= 1; print_int(a); putchar('\n');
    a &= 0xa; print_int(a); putchar('\n');
    a |= 0x5; print_int(a); putchar('\n');
    a ^= 0xf; print_int(a); putchar('\n');

    a = 5;
    print_int(a++); print_int(a); putchar('\n');
    print_int(++a); print_int(a); putchar('\n');
    print_int(a--); print_int(a); putchar('\n');
    print_int(--a); print_int(a); putchar('\n');

    print_int(-7 / 2); putchar('\n');
    print_int(-7 % 2); putchar('\n');
    print_int(32767 + 1 == -32768); putchar('\n');
    return 0;
}
