#include <stdio.h>

#define ANSWER 42
#define SQUARE(x) ((x) * (x))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define GREETING "defined string"

#define FEATURE
#ifdef FEATURE
#define HAS_FEATURE 1
#else
#define HAS_FEATURE 0
#endif

#ifndef MISSING
#define MISSING_UNDEFINED 1
#endif

#if ANSWER == 42 && defined(FEATURE)
#define CONDITIONAL_OK 1
#elif ANSWER == 43
#define CONDITIONAL_OK 2
#else
#define CONDITIONAL_OK 0
#endif

#undef FEATURE
#ifdef FEATURE
#define STILL_THERE 1
#else
#define STILL_THERE 0
#endif

int main(void)
{
    print_int(ANSWER); putchar('\n');
    print_int(SQUARE(5)); putchar('\n');          /* 25 */
    print_int(SQUARE(2 + 3)); putchar('\n');      /* 25 */
    print_int(MAX(3, 9)); putchar('\n');          /* 9 */
    print_int(MAX(SQUARE(3), 8)); putchar('\n');  /* 9 */
    puts(GREETING);
    print_int(HAS_FEATURE); putchar('\n');        /* 1 */
    print_int(MISSING_UNDEFINED); putchar('\n');  /* 1 */
    print_int(CONDITIONAL_OK); putchar('\n');     /* 1 */
    print_int(STILL_THERE); putchar('\n');        /* 0 */
    print_int(__RISKY__); putchar('\n');          /* 1 */
    return 0;
}
