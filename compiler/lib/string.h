/* RISKY libc: string
 *
 * Remember that this is a word addressed machine: a char is one 16 bit
 * word, so mem* sizes are in words.
 */

#ifndef _STRING_H
#define _STRING_H

int strlen(char *s)
{
    int n = 0;
    while (*s++)
        n++;
    return n;
}

char *strcpy(char *dst, char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != 0)
        ;
    return dst;
}

char *strncpy(char *dst, char *src, int n)
{
    char *d = dst;
    while (n > 0 && *src) {
        *d++ = *src++;
        n--;
    }
    while (n-- > 0)
        *d++ = 0;
    return dst;
}

char *strcat(char *dst, char *src)
{
    char *d = dst;
    while (*d)
        d++;
    strcpy(d, src);
    return dst;
}

int strcmp(char *a, char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a - *b;
}

int strncmp(char *a, char *b, int n)
{
    while (n > 1 && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return *a - *b;
}

char *strchr(char *s, int c)
{
    while (*s) {
        if (*s == c)
            return s;
        s++;
    }
    if (c == 0)
        return s;
    return 0;
}

void *memset(void *dst, int value, int words)
{
    int *d = (int *) dst;
    while (words-- > 0)
        *d++ = value;
    return dst;
}

void *memcpy(void *dst, void *src, int words)
{
    int *d = (int *) dst;
    int *s = (int *) src;
    while (words-- > 0)
        *d++ = *s++;
    return dst;
}

int memcmp(void *a, void *b, int words)
{
    int *pa = (int *) a;
    int *pb = (int *) b;
    while (words-- > 0) {
        if (*pa != *pb)
            return *pa - *pb;
        pa++;
        pb++;
    }
    return 0;
}

#endif
