/* Sierpinski triangle via Rule 90: new[i] = old[i-1] ^ old[i+1].
 * 31 cells wide × 8 rows for the 32×8 terminal.
 * First row has a single '#' in the middle. */

#include <stdio.h>

#define WIDTH 31

int main(void)
{
    int row[WIDTH];
    int next[WIDTH];
    int i, gen;

    /* row 0: single seed in the middle */
    for (i = 0; i < WIDTH; i++)
        row[i] = 0;
    row[WIDTH / 2] = 1;

    for (gen = 0; gen < 8; gen++) {
        /* draw current row */
        for (i = 0; i < WIDTH; i++)
            putchar(row[i] ? '#' : ' ');
        if (gen < 7)
            putchar('\n');

        /* compute next row: new[i] = old[i-1] ^ old[i+1] */
        for (i = 1; i < WIDTH - 1; i++)
            next[i] = row[i - 1] ^ row[i + 1];
        next[0] = 0;
        next[WIDTH - 1] = 0;

        /* copy next -> row */
        for (i = 0; i < WIDTH; i++)
            row[i] = next[i];
    }
    return 0;
}
