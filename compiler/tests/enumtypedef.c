#include <stdio.h>

enum Color { RED, GREEN = 5, BLUE };

typedef int Number;
typedef struct Pair {
    Number first;
    Number second;
} Pair;
typedef char *String;
typedef int Row[4];

Pair makePair(void);

int main(void)
{
    Number n = 42;
    Pair p;
    String s = "typedefed";
    Row row;
    enum Color c = BLUE;

    print_int(RED); print_int(GREEN); print_int(BLUE); putchar('\n'); /* 056 */
    print_int(c); putchar('\n');           /* 6 */
    print_int(n); putchar('\n');           /* 42 */

    p.first = 8;
    p.second = 9;
    print_int(p.first * p.second); putchar('\n'); /* 72 */
    puts(s);

    row[0] = 1; row[3] = 4;
    print_int(row[0] + row[3]); putchar('\n');  /* 5 */
    print_int(sizeof(Row)); putchar('\n');      /* 4 */
    print_int(sizeof(Pair)); putchar('\n');     /* 2 */

    switch (c) {
    case RED: puts("red"); break;
    case GREEN: puts("green"); break;
    case BLUE: puts("blue"); break;
    }
    return 0;
}
