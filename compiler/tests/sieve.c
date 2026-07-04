#include <stdio.h>
#include <string.h>

#define LIMIT 50

int primes[LIMIT];

void sort(int *a, int n)
{
    int i, j;
    for (i = 0; i < n - 1; i++)
        for (j = 0; j < n - 1 - i; j++)
            if (a[j] > a[j + 1]) {
                int t = a[j];
                a[j] = a[j + 1];
                a[j + 1] = t;
            }
}

int main(void)
{
    int flags[LIMIT + 1];
    int found[20];
    int count = 0;
    int i, j;

    memset(flags, 1, LIMIT + 1);
    for (i = 2; i <= LIMIT; i++) {
        if (flags[i]) {
            found[count++] = i;
            for (j = i + i; j <= LIMIT; j += i)
                flags[j] = 0;
        }
    }
    for (i = 0; i < count; i++)
        printf("%d ", found[i]);
    putchar('\n');

    /* reverse, then sort back */
    for (i = 0; i < count / 2; i++) {
        int t = found[i];
        found[i] = found[count - 1 - i];
        found[count - 1 - i] = t;
    }
    printf("%d %d\n", found[0], found[count - 1]);
    sort(found, count);
    printf("%d %d\n", found[0], found[count - 1]);
    return 0;
}
