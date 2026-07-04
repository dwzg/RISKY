/* RISKY libc: ctype */

#ifndef _CTYPE_H
#define _CTYPE_H

int isdigit(int c)
{
    return c >= '0' && c <= '9';
}

int isupper(int c)
{
    return c >= 'A' && c <= 'Z';
}

int islower(int c)
{
    return c >= 'a' && c <= 'z';
}

int isalpha(int c)
{
    return isupper(c) || islower(c);
}

int isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}

int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r'
        || c == '\v' || c == '\f';
}

int toupper(int c)
{
    return islower(c) ? c - 32 : c;
}

int tolower(int c)
{
    return isupper(c) ? c + 32 : c;
}

#endif
