#include <stdio.h>

char *name(int n)
{
    switch (n) {
    case 0:
        return "zero";
    case 1:
    case 2:
        return "few";
    case 3: {
        int x = n * 111;
        print_int(x);
        return "three";
    }
    default:
        return "many";
    }
}

int main(void)
{
    int i;
    int state = 0;

    for (i = 0; i < 6; i++) {
        puts(name(i));
    }

    /* fallthrough */
    switch (2) {
    case 1:
        state += 1;
    case 2:
        state += 10;
    case 3:
        state += 100;
        break;
    case 4:
        state += 1000;
    }
    print_int(state); putchar('\n');      /* 110 */

    switch (42) {
    case 1: break;
    }
    puts("done");
    return 0;
}
