/* Mandelbrot set, 80×24, for the Logisim TTY.
 *
 * Q11.4 fixed point (scale = 16).  Products fit in 16 bits.
 * All down-scaling uses signed division (/ SCALE), not >>,
 * because RISKY's hardware shr is logical (unsigned) but the
 * intermediate values can be negative.
 */

#include <stdio.h>

#define SCALE 16
#define WIDTH  80
#define HEIGHT 24
#define ITERS  24

int main(void)
{
    int row, col, n;
    int zr, zi, zr2, zi2;
    int cx, cy;
    /* Centered on the main cardioid at c ~= -0.75 + 0i.
     * Visual aspect 80:48 ~ 5:3, so x_range:y_range = 5:3. */
    int xmin = (-3 * SCALE) / 2;     /*  -1.500 */
    int xmax = 0;                    /*   0.000 */
    int ymax = (9 * SCALE) / 20;     /*   0.450 */
    int ymin = -ymax;
    int limit = 4 * SCALE;           /*   4.0 * 16 = 64 */
    char shade[] = " .:-=+*#%@";

    /* Accumulate coordinates with 4 extra fractional bits (Q15.4) to
     * avoid the step being truncated to 0 by integer division. */
    int cy_q = ymax << 4;                    /* Q15.4 */
    int dx_q = ((xmax - xmin) << 4) / (WIDTH - 1);
    int dy_q = ((ymax - ymin) << 4) / (HEIGHT - 1);

    for (row = 0; row < HEIGHT; row++) {
        int cx_q = xmin << 4;                /* Q15.4 */
        for (col = 0; col < WIDTH; col++) {
            cx = cx_q / SCALE;               /* truncate to Q11.4 (use / not >>,
                                                >> is logical on RISKY hardware) */
            cy = cy_q / SCALE;
            zr = 0;
            zi = 0;
            n = 0;
            while (n < ITERS) {
                zr2 = (zr * zr) / SCALE;     /* squares are >=0, but use / anyway */
                zi2 = (zi * zi) / SCALE;
                if (zr2 + zi2 >= limit)
                    break;
                /* zi' = 2*zr*zi + ci,  zr' = zr2 - zi2 + cr */
                zi = ((zr * zi) / (SCALE / 2) + cy);
                zr = zr2 - zi2 + cx;
                n++;
            }
            putchar(shade[n >= ITERS ? 8 : n * 8 / ITERS]);
            cx_q += dx_q;
        }
        if (row < HEIGHT - 1)
            putchar('\n');
        cy_q -= dy_q;
    }
    return 0;
}
