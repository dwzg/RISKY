#include <stdio.h>
#include <softfloat.h>

int main(void)
{
    int a, b;

    print_int(f16_to_int(f16_from_int(0)));   putchar(' ');
    print_int(f16_to_int(f16_from_int(1)));   putchar(' ');
    print_int(f16_to_int(f16_from_int(42)));  putchar(' ');
    print_int(f16_to_int(f16_from_int(-7)));  putchar(' ');
    print_int(f16_to_int(f16_from_int(1000)));putchar('\n');

    a = f16_from_int(3);
    b = f16_from_int(2);
    f16_print(f16_add(a, b)); putchar(' ');
    f16_print(f16_sub(a, b)); putchar(' ');
    f16_print(f16_mul(a, b)); putchar(' ');
    f16_print(f16_div(a, b)); putchar('\n');

    a = f16_div(f16_from_int(1), f16_from_int(2));
    b = f16_div(f16_from_int(1), f16_from_int(4));
    f16_print(a); putchar(' ');
    f16_print(b); putchar(' ');
    f16_print(f16_add(a, b)); putchar('\n');

    f16_print(f16_div(f16_from_int(1), f16_from_int(3))); putchar(' ');
    f16_print(f16_div(f16_from_int(2), f16_from_int(3))); putchar(' ');
    f16_print(f16_mul(f16_div(f16_from_int(1), f16_from_int(3)),
                      f16_from_int(3))); putchar('\n');

    a = f16_div(f16_from_int(22), f16_from_int(7));
    f16_print(a); putchar(' ');
    f16_print(f16_neg(a)); putchar(' ');
    f16_print(f16_mul(a, a)); putchar('\n');

    print_int(f16_cmp(f16_from_int(3), f16_from_int(5))); putchar(' ');
    print_int(f16_cmp(f16_from_int(5), f16_from_int(3))); putchar(' ');
    print_int(f16_cmp(a, a)); putchar(' ');
    print_int(f16_cmp(f16_neg(a), a)); putchar('\n');

    f16_print(f16_div(f16_from_int(1), f16_from_int(0))); putchar(' ');
    f16_print(f16_div(f16_from_int(-1), f16_from_int(0))); putchar(' ');
    f16_print(f16_div(f16_from_int(0), f16_from_int(0))); putchar(' ');
    f16_print(f16_mul(f16_from_int(300), f16_from_int(300)));
    putchar('\n');

    print_int(f16_isinf(f16_div(f16_from_int(1), f16_from_int(0)))); putchar(' ');
    print_int(f16_isnan(f16_div(f16_from_int(0), f16_from_int(0)))); putchar('\n');
    return 0;
}
