#include <stdio.h>

int main(void)
{
    printf("plain\n");
    printf("num=%d\n", 42);
    printf("neg=%d\n", -17);
    printf("hex=%x\n", 0xbeef);
    printf("char=%c\n", 'Q');
    printf("str=%s\n", "inner");
    printf("pct=%%\n");
    printf("%d+%d=%d\n", 2, 3, 2 + 3);
    printf("[%s|%c|%x]\n", "mix", 'z', 255);
    return 0;
}
