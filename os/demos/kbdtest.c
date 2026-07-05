/* Keyboard echo test — reads keys and prints them.
 * Press Enter (0x0a) to exit. */
#include <stdio.h>

int main(void)
{
    int c;
    puts("Keyboard echo test. Press Enter to exit.");
    puts("Keys: ");
    while (1) {
        while (!kbhit()) {}    /* wait for key */
        c = getkey();
        if (c == '\n') break;
        putchar(c);
    }
    puts("\nDone.");
    return 0;
}
