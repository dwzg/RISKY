#include <stdio.h>

int squares[5] = {0, 1, 4, 9, 16};
int table[2][3] = {{1, 2, 3}, {4, 5, 6}};

int sum(int *values, int n)
{
    int total = 0;
    int i;
    for (i = 0; i < n; i++)
        total += values[i];
    return total;
}

int main(void)
{
    int local[4] = {10, 20, 30, 40};
    int inferred[] = {1, 2, 3, 4, 5, 6};
    int partial[5] = {1, 2};
    int i, j;
    int *p;

    print_int(sum(squares, 5)); putchar('\n');   /* 30 */
    print_int(sum(local, 4)); putchar('\n');     /* 100 */
    print_int(sizeof inferred); putchar('\n');   /* 6 */
    print_int(sum(partial, 5)); putchar('\n');   /* 3 */

    for (i = 0; i < 2; i++)
        for (j = 0; j < 3; j++)
            print_int(table[i][j]);
    putchar('\n');                               /* 123456 */

    table[1][2] = 99;
    print_int(table[1][2]); putchar('\n');       /* 99 */
    print_int(sizeof table); putchar('\n');      /* 6 */
    print_int(sizeof table[0]); putchar('\n');   /* 3 */

    p = local;
    print_int(*p); putchar('\n');                /* 10 */
    p++;
    print_int(*p); putchar('\n');                /* 20 */
    print_int(*(p + 2)); putchar('\n');          /* 40 */
    print_int(p[-1]); putchar('\n');             /* 10 */
    print_int(p - local); putchar('\n');         /* 1 */
    print_int(2[local]); putchar('\n');          /* 30 */

    squares[0] = 100;
    print_int(squares[0] + squares[4]); putchar('\n'); /* 116 */
    return 0;
}
