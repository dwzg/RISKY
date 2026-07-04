/* Bouncing ball animation on the 32×8 terminal.
 * Redraws the full 31×8 grid each frame; terminal scrolling
 * creates the animation effect.  Pure integer adds/subtracts. */

#include <stdio.h>

#define WIDTH  31
#define HEIGHT  8
#define FRAMES  40

int main(void)
{
    int x, y, vx, vy, px, py;
    int row, col, frame;

    /* fixed-point position Q8.8: ball centre at integer grid coords
     * px = X * 256, py = Y * 256.  Bounce when the ball hits an edge. */
    px = 5 * 256;                  /* start near top-left */
    py = 2 * 256;
    vx = 137;                      /* ~0.535 pixels/frame in Q8.8 */
    vy = 89;                       /* ~0.348 pixels/frame */

    for (frame = 0; frame < FRAMES; frame++) {
        /* move */
        px = px + vx;
        py = py + vy;

        x = px >> 8;                 /* px always >=0, logical shift = /256 */
        y = py >> 8;

        /* bounce */
        if (x <= 0)  { x = 0;  vx = -vx; px = 0; }
        if (x >= 30) { x = 30; vx = -vx; px = 30 * 256; }
        if (y <= 0)  { y = 0;  vy = -vy; py = 0; }
        if (y >= 7)  { y = 7;  vy = -vy; py = 7 * 256; }

        /* draw frame */
        for (row = 0; row < HEIGHT; row++) {
            for (col = 0; col < WIDTH; col++) {
                /* draw the ball as 'O' with a 1-pixel trail: show a
                 * dimmer char where the ball was last frame */
                if (col == x && row == y)
                    putchar('O');
                else
                    putchar(' ');
            }
            if (row < HEIGHT - 1 || frame < FRAMES - 1)
                putchar('\n');
        }
    }
    return 0;
}
