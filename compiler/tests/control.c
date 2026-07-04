#include <stdio.h>

int main(void)
{
    int i;
    int sum = 0;
    int n;

    for (i = 1; i <= 10; i++)
        sum += i;
    print_int(sum); putchar('\n');           /* 55 */

    i = 0;
    while (i < 5) i++;
    print_int(i); putchar('\n');             /* 5 */

    i = 10;
    do { i--; } while (i > 3);
    print_int(i); putchar('\n');             /* 3 */

    sum = 0;
    for (i = 0; i < 10; i++) {
        if (i == 3) continue;
        if (i == 7) break;
        sum += i;
    }
    print_int(sum); putchar('\n');           /* 0+1+2+4+5+6 = 18 */

    n = 0;
    for (i = 0; i < 3; i++) {
        int j;
        for (j = 0; j < 4; j++) {
            if (j == 2 && i == 1) continue;
            n++;
        }
    }
    print_int(n); putchar('\n');             /* 11 */

    if (1) print_string("t1"); else print_string("e1");
    if (0) print_string("t2"); else print_string("e2");
    putchar('\n');

    i = 0;
again:
    i++;
    if (i < 4) goto again;
    print_int(i); putchar('\n');             /* 4 */

    for (i = 0; ; i++)
        if (i == 6) break;
    print_int(i); putchar('\n');             /* 6 */
    return 0;
}
