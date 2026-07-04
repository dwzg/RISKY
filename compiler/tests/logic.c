#include <stdio.h>

int calls = 0;

int touch(int value)
{
    calls++;
    return value;
}

int main(void)
{
    int a;

    calls = 0;
    a = touch(0) && touch(1);
    print_int(a); print_int(calls); putchar('\n');   /* 0 1 (short circuit) */

    calls = 0;
    a = touch(1) && touch(2);
    print_int(a); print_int(calls); putchar('\n');   /* 1 2 */

    calls = 0;
    a = touch(3) || touch(4);
    print_int(a); print_int(calls); putchar('\n');   /* 1 1 */

    calls = 0;
    a = touch(0) || touch(0);
    print_int(a); print_int(calls); putchar('\n');   /* 0 2 */

    a = 1 ? 10 : 20;
    print_int(a); putchar('\n');                     /* 10 */
    a = 0 ? 10 : 20;
    print_int(a); putchar('\n');                     /* 20 */
    a = (3 > 2) ? (1 ? 111 : 222) : 333;
    print_int(a); putchar('\n');                     /* 111 */

    a = (touch(1), touch(2), 5);
    print_int(a); putchar('\n');                     /* 5 */

    print_int(3 > 2 && 2 > 1 || 0); putchar('\n');   /* 1 */
    print_int(!(5 == 5) || 4 < 3); putchar('\n');    /* 0 */
    return 0;
}
