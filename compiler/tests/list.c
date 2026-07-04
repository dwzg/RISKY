#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Node {
    int value;
    struct Node *next;
};

struct Node *push(struct Node *head, int value)
{
    struct Node *node = (struct Node *) malloc(sizeof(struct Node));
    node->value = value;
    node->next = head;
    return node;
}

int main(void)
{
    struct Node *head = NULL;
    struct Node *n;
    int i;
    int words[8];

    for (i = 1; i <= 5; i++)
        head = push(head, i * i);

    for (n = head; n != NULL; n = n->next)
        printf("%d ", n->value);
    putchar('\n');                       /* 25 16 9 4 1 */

    memset(words, 7, 8);
    print_int(words[0] + words[7]); putchar('\n');   /* 14 */

    memcpy(words, "abc", 4);
    puts((char *) words);                /* abc */
    return 0;
}
