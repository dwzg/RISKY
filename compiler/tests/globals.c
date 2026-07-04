#include <stdio.h>

int counter = 100;
int zeroed;
int computed = 6 * 7 + 1;
char *title = "global title";
int data[4] = {2, 4, 6, 8};
extern int later;
int later = 55;

int nextId(void)
{
    static int id = 1000;
    id++;
    return id;
}

int increment(void)
{
    counter++;
    return counter;
}

int main(void)
{
    print_int(counter); putchar('\n');       /* 100 */
    print_int(zeroed); putchar('\n');        /* 0 */
    print_int(computed); putchar('\n');      /* 43 */
    puts(title);
    print_int(data[0] + data[3]); putchar('\n'); /* 10 */
    print_int(later); putchar('\n');         /* 55 */

    increment();
    increment();
    print_int(counter); putchar('\n');       /* 102 */

    print_int(nextId()); putchar('\n');      /* 1001 */
    print_int(nextId()); putchar('\n');      /* 1002 */
    print_int(nextId()); putchar('\n');      /* 1003 */
    return 0;
}
