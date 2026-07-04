/* RISKY libc: stdlib
 *
 * The heap is a simple bump allocator in page 0 RAM, growing up from
 * 0x8000.  Globals and strings live below it (from address 0 up), the
 * stack grows down from 0xffff.  free() is a no-op.
 */

#ifndef _STDLIB_H
#define _STDLIB_H

#ifndef NULL
#define NULL 0
#endif

int __heap_ptr = 0x8000;

void *malloc(int words)
{
    int p = __heap_ptr;
    __heap_ptr += words;
    return (void *) p;
}

void *calloc(int count, int words)
{
    int total = count * words;
    int *p = (int *) malloc(total);
    int i;
    for (i = 0; i < total; i++)
        p[i] = 0;
    return (void *) p;
}

void free(void *p)
{
}

int abs(int n)
{
    return n < 0 ? -n : n;
}

int atoi(char *s)
{
    int n = 0;
    int sign = 1;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return sign * n;
}

void exit(int status)
{
    __asm__("\n\tjmp __halt");
}

int rand_seed = 12345;

int rand(void)
{
    rand_seed = rand_seed * 25173 + 13849;
    return rand_seed & 0x7fff;
}

void srand(int seed)
{
    rand_seed = seed;
}

#endif
