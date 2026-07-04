#include <stdio.h>
#include <ctype.h>

int main(void)
{
    int c;
    int letters = 0;
    int digits = 0;

    while ((c = getchar()) != EOF) {
        if (isalpha(c)) letters++;
        if (isdigit(c)) digits++;
        putchar(toupper(c));
    }
    printf("letters=%d digits=%d\n", letters, digits);
    return 0;
}
