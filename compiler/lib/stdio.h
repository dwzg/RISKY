/* RISKY libc: stdio
 *
 * The terminal is a write-only character register at page 0xa, address 0.
 * Character input comes from the read-only data ROM at page 0xb (the
 * simulator preloads it with the -i file; in Logisim it is the DATA ROM).
 * The libc keeps its input cursor in page 9, address 0.
 *
 * Page switching happens strictly around single load/store instructions,
 * never around pushes, calls or r15-relative accesses: the stack lives in
 * the page 0 RAM, so touching it while another page is selected would
 * read/write the wrong memory.
 */

#ifndef _STDIO_H
#define _STDIO_H

#define EOF (-1)
#define NULL 0

int putchar(int c)
{
    /* parameter c is at r15+3 */
    __asm__("\n\tldo r0,*r15,#3"
            "\n\tmov r2,page"
            "\n\tin r1,#0xa"
            "\n\tmov page,r1"
            "\n\tstod *0,r0"
            "\n\tmov page,r2");
    return c;
}

int getchar(void)
{
    int c;
    /* local c is at r15+0; input cursor lives at page 9, address 0 */
    __asm__("\n\tmov r3,page"
            "\n\tin r1,#9"
            "\n\tmov page,r1"
            "\n\tldd r1,*0"
            "\n\tin r2,#0xb"
            "\n\tmov page,r2"
            "\n\tldr r0,*r1"
            "\n\taddi r1,#1"
            "\n\tin r2,#9"
            "\n\tmov page,r2"
            "\n\tstod *0,r1"
            "\n\tmov page,r3"
            "\n\tstoo *r15,#0,r0");
    if (c == 0)
        return EOF;
    return c;
}

int puts(char *s)
{
    while (*s)
        putchar(*s++);
    putchar('\n');
    return 0;
}

int print_string(char *s)
{
    while (*s)
        putchar(*s++);
    return 0;
}

int print_int(int n)
{
    char buffer[6];
    int i = 0;

    if (n == -32768) {          /* -(-32768) does not exist in 16 bit */
        print_string("-32768");
        return 0;
    }
    if (n < 0) {
        putchar('-');
        n = -n;
    }
    do {
        buffer[i++] = '0' + n % 10;
        n /= 10;
    } while (n != 0);
    while (i > 0)
        putchar(buffer[--i]);
    return 0;
}

int print_hex(int n)
{
    char *digits = "0123456789abcdef";
    int shift = 12;
    int started = 0;
    int nibble;

    while (shift >= 0) {
        nibble = (n >> shift) & 0xf;
        if (nibble != 0 || started || shift == 0) {
            putchar(digits[nibble]);
            started = 1;
        }
        shift -= 4;
    }
    return 0;
}

/* minimal printf: %d %x %c %s %% */
int printf(char *format, ...)
{
    int *ap = &format + 1;

    while (*format) {
        if (*format == '%') {
            format++;
            if (*format == 'd')
                print_int(*ap++);
            else if (*format == 'x')
                print_hex(*ap++);
            else if (*format == 'c')
                putchar(*ap++);
            else if (*format == 's')
                print_string((char *) *ap++);
            else if (*format == '%')
                putchar('%');
            else {
                putchar('%');
                putchar(*format);
            }
        } else {
            putchar(*format);
        }
        format++;
    }
    return 0;
}

#endif
