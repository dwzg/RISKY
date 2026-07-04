#include <stdio.h>

void swap(int *a, int *b)
{
    int t = *a;
    *a = *b;
    *b = t;
}

int apply(int (*f)(int, int), int a, int b)
{
    return f(a, b);
}

int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }

int globalValue = 77;

int main(void)
{
    int x = 3;
    int y = 9;
    int *p = &x;
    int **pp = &p;
    int (*op)(int, int);

    print_int(*p); putchar('\n');            /* 3 */
    *p = 42;
    print_int(x); putchar('\n');             /* 42 */
    print_int(**pp); putchar('\n');          /* 42 */
    **pp = 7;
    print_int(x); putchar('\n');             /* 7 */

    swap(&x, &y);
    print_int(x); print_int(y); putchar('\n'); /* 97 */

    p = &globalValue;
    (*p)++;
    print_int(globalValue); putchar('\n');   /* 78 */

    op = add;
    print_int(apply(op, 4, 5)); putchar('\n');   /* 9 */
    op = &mul;
    print_int(apply(op, 4, 5)); putchar('\n');   /* 20 */
    print_int(op(6, 7)); putchar('\n');          /* 42 */

    print_int(p == &globalValue); putchar('\n'); /* 1 */
    print_int(p == &x); putchar('\n');           /* 0 */
    return 0;
}
