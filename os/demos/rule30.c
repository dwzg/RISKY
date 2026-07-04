/* Rule 30 cellular automaton: new[i] = old[i-1] ^ (old[i] | old[i+1]).
 * Produces chaotic, organic-looking patterns.
 * 31 cells wide × 8 rows. Single seed in the middle. */

#include <stdio.h>

#define WIDTH 31

int main(void)
{
    int row[WIDTH];
    int next[WIDTH];
    int i, gen;

    for (i = 0; i < WIDTH; i++)
        row[i] = 0;
    row[WIDTH / 2] = 1;

    for (gen = 0; gen < 8; gen++) {
        for (i = 0; i < WIDTH; i++)
            putchar(row[i] ? '#' : ' ');
        if (gen < 7)
            putchar('\n');

        /* Rule 30: new = left ^ (self | right) */
        for (i = 1; i < WIDTH - 1; i++)
            next[i] = row[i - 1] ^ (row[i] | row[i + 1]);
        next[0] = 0;
        next[WIDTH - 1] = 0;

        for (i = 0; i < WIDTH; i++)
            row[i] = next[i];
    }
    return 0;
}
