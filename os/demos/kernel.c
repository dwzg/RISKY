#include <stdio.h>

/* Cooperative multitasking kernel with true stack switching.
 *
 * Fixed addresses 0x7FF0-0x7FF3 bridge asm and C:
 *   0x7FF0  saved sp   (asm writes, C reads)
 *   0x7FF1  saved r15
 *   0x7FF2  next sp    (C writes, asm reads)
 *   0x7FF3  next r15
 *
 * current_task always identifies the RUNNING task.  main() is task 0.
 */

#define MAX       4
#define STK       128

#define SAVE_SP   0x7FF0
#define SAVE_R15  0x7FF1
#define NEXT_SP   0x7FF2
#define NEXT_R15  0x7FF3

int task_sp[MAX];
int task_r15[MAX];
int task_state[MAX];
int current_task;

/* ---- scheduler (called from asm after saving context) ---- */
void __sched(void)
{
    int *p = (int *) SAVE_SP;
    task_sp[current_task]   = p[0];
    task_r15[current_task]  = p[1];

    /* pick next ready task */
    do {
        current_task = (current_task + 1) % MAX;
    } while (task_state[current_task] == 0);

    p[2] = task_sp[current_task];
    p[3] = task_r15[current_task];
}

/* ---- task creation ---- */
void spawn(int *stack, void (*fn)(void), int id)
{
    int *sp = stack + STK - 2;
    sp[1] = (int) fn;           /* "return address" */
    sp[0] = 0;                  /* dummy, overwritten by fn's prologue */
    task_sp[id]   = (int) sp;
    task_r15[id]  = (int)(sp + 1);
    task_state[id] = 1;
}

/* ---- yield: save, schedule, switch, return ---- */
__naked void yield(void)
{
    /* __naked: no C prologue. sp points to the return address pushed
     * by 'call yield'.  Save sp/r15, call C scheduler, switch, ret. */
    __asm__(
        "\n\tldsp r0"
        "\n\tstod *" "0x7FF0" ",r0"            /* save sp */
        "\n\tstod *" "0x7FF1" ",r15"           /* save r15 */

        "\n\tcall __sched"                     /* C picks next, fills 0x7FF2/3 */

        "\n\tldd r0,*" "0x7FF2"                /* load next sp */
        "\n\tstosp r0"                         /* switch stacks */
        "\n\tldd r15,*" "0x7FF3"               /* load next r15 */
        "\n\tret"                              /* -> next task */
    );
}

/* ---- demo tasks ---- */
void counter_task(void)
{
    int i;
    for (i = 0; i < 26; i++) {
        putchar('0' + (i % 10));
        yield();
    }
    putchar('!');
    task_state[1] = 0;
    while (1) yield();
}

void spinner_task(void)
{
    char *c = "|/-\\";
    int i;
    for (i = 0; i < 36; i++) {
        putchar(c[i & 3]);
        yield();
    }
    task_state[2] = 0;
    while (1) yield();
}

void bracket_task(void)
{
    int i;
    for (i = 0; i < 20; i++) {
        putchar('['); yield();
        putchar(']'); yield();
    }
    task_state[3] = 0;
    while (1) yield();
}

int alive(void)
{
    int i;
    for (i = 1; i < MAX; i++)
        if (task_state[i]) return 1;
    return 0;
}

/* ---- main (task 0) ---- */
int main(void)
{
    static int stk1[STK], stk2[STK], stk3[STK];
    int i;

    for (i = 0; i < MAX; i++)
        task_state[i] = 0;

    spawn(stk1, counter_task, 1);
    spawn(stk2, spinner_task, 2);
    spawn(stk3, bracket_task, 3);

    task_state[0] = 1;       /* main is task 0 */
    current_task = 0;

    do {
        yield();
    } while (alive());

    putchar('\n');
    __asm__("\n\tjmp __halt");
    return 0;
}
