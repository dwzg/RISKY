#include <stdio.h>
#include <softfloat.h>

int f(int x)
{
    int x2 = f16_mul(x, x);
    int x3 = f16_mul(x2, x);
    int twox = f16_mul(f16_from_int(2), x);
    return f16_sub(f16_sub(x3, twox), f16_from_int(5));
}

int fp(int x)
{
    int x2 = f16_mul(x, x);
    int threex2 = f16_mul(f16_from_int(3), x2);
    return f16_sub(threex2, f16_from_int(2));
}

int main(void)
{
    int x = f16_from_int(2);
    int prev;
    int n;

    for (n = 0; n < 10; n++) {
        prev = x;
        x = f16_sub(x, f16_div(f(x), fp(x)));
        f16_print(x); putchar(' ');
        if (f16_cmp(x, prev) == 0)
            break;
    }
    putchar('\n');
    print_int(f16_cmp(f(x), f16_from_int(0))); putchar('\n');
    return 0;
}
