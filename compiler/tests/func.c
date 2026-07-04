#include <stdio.h>

int fib(int n);

int fact(int n)
{
    if (n <= 1)
        return 1;
    return n * fact(n - 1);
}

int add3(int a, int b, int c)
{
    return a + b + c;
}

int main(void)
{
    print_int(fact(7)); putchar('\n');       /* 5040 */
    print_int(fib(10)); putchar('\n');       /* 55 */
    print_int(add3(1, 2, 3)); putchar('\n'); /* 6 */
    print_int(add3(fact(3), fib(5), 100)); putchar('\n'); /* 6+5+100=111 */
    return 0;
}

int fib(int n)
{
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}
