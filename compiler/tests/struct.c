#include <stdio.h>

struct Point {
    int x;
    int y;
};

struct Rect {
    struct Point topLeft;
    struct Point bottomRight;
};

union Value {
    int number;
    char *text;
};

struct Point makeOrigin(void);

int area(struct Rect r)
{
    return (r.bottomRight.x - r.topLeft.x) * (r.bottomRight.y - r.topLeft.y);
}

int manhattan(struct Point *p)
{
    return p->x + p->y;
}

struct Point points[3] = {{1, 2}, {3, 4}, {5, 6}};

int main(void)
{
    struct Point a;
    struct Point b = {10, 20};
    struct Rect r;
    struct Point *pp = &b;
    union Value v;
    int i;

    a.x = 3;
    a.y = 4;
    print_int(a.x + a.y); putchar('\n');         /* 7 */
    print_int(b.x + b.y); putchar('\n');         /* 30 */

    a = b;                                       /* struct copy */
    b.x = 999;
    print_int(a.x); putchar('\n');               /* 10 */

    pp->x = 100;
    print_int(b.x); putchar('\n');               /* 100 */
    print_int(manhattan(&b)); putchar('\n');     /* 120 */

    r.topLeft.x = 1;  r.topLeft.y = 1;
    r.bottomRight.x = 5; r.bottomRight.y = 4;
    print_int(area(r)); putchar('\n');           /* 12 */

    for (i = 0; i < 3; i++)
        print_int(points[i].x * points[i].y);
    putchar('\n');                               /* 2 12 30 */

    print_int(sizeof(struct Point)); putchar('\n');  /* 2 */
    print_int(sizeof(struct Rect)); putchar('\n');   /* 4 */
    print_int(sizeof(union Value)); putchar('\n');   /* 1 */
    print_int(sizeof points); putchar('\n');         /* 6 */

    v.number = 42;
    print_int(v.number); putchar('\n');          /* 42 */
    v.text = "hi";
    puts(v.text);
    return 0;
}
