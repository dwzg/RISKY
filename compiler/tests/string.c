#include <stdio.h>
#include <string.h>

char message[] = "global";
char *pointer = "pointed";

int main(void)
{
    char buffer[32];
    char local[] = "local";

    puts("string literal");
    puts(message);
    puts(pointer);
    puts(local);

    print_int(strlen("12345")); putchar('\n');       /* 5 */
    print_int(strlen(message)); putchar('\n');       /* 6 */

    strcpy(buffer, "copy");
    puts(buffer);
    strcat(buffer, "+cat");
    puts(buffer);

    print_int(strcmp("abc", "abc")); putchar('\n');  /* 0 */
    print_int(strcmp("abc", "abd") < 0); putchar('\n'); /* 1 */
    print_int(strcmp("b", "a") > 0); putchar('\n');  /* 1 */

    print_int('A'); putchar('\n');                   /* 65 */
    putchar('x'); putchar('\n');
    puts("esc:\ttab");
    return 0;
}
