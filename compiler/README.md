# RISKY C Toolchain v3

A (nearly) C89 compiler, assembler and simulator for the RISKY CPU.
This finishes the v2 compiler: same overall design (regex lexer,
recursive descent parser, AST classes that emit assembler text) and the
same ABI as v1/v2, but with complete code generation and most of C89.

## Files

| file             | purpose                                              |
|------------------|------------------------------------------------------|
| `risky_c.py`     | C compiler: `prog.c` → `prog.asm`                    |
| `risky_asm.py`   | assembler: `prog.asm` → `prog.hex` (Logisim ROM)     |
| `risky_sim.py`   | instruction level simulator for the RISKY CPU        |
| `cc.sh`          | driver: compile + assemble (+ `-r` to simulate)      |
| `lib/`           | header-only libc (`stdio.h`, `stdlib.h`, `string.h`, `ctype.h`) |
| `tests/`         | test suite: `python3 tests/run_tests.py`             |

## Quick start

```sh
./cc.sh -r tests/sieve.c          # compile, assemble and simulate
python3 risky_sim.py prog.hex -i input.txt -v   # run with stdin + registers
```

The generated `.hex` is a Logisim "v2.0 raw" image for the instruction
ROM, exactly like the v1 toolchain produced.

## Supported C

* **Types:** `void`, `char`, `short`, `int`, `long`, `signed`/`unsigned`,
  pointers (any depth), arrays (incl. multidimensional, `[]` with size
  inference from initializers), `struct`/`union` (nested, by-value
  assignment and by-value parameters), `enum`, `typedef`, function
  pointers (`int (*f)(int)`, calls through pointers).
* **Statements:** everything in C89 — `if`/`else`, `while`, `do`/`while`,
  `for`, `switch`/`case`/`default` (with fallthrough), `break`,
  `continue`, `goto`/labels, `return`, blocks, `;`.
* **Expressions:** all C89 operators, incl. compound assignment, `++`/
  `--` (pre/post, on any lvalue, pointer-scaled), `?:`, comma, `sizeof`
  (expression and type), casts, short-circuit `&&`/`||`, pointer
  arithmetic and `p - q`, `a[i]` == `i[a]`, adjacent string literal
  concatenation.
* **Declarations:** globals with constant initializers (incl. `{...}`
  lists, string literals, addresses of globals/functions), tentative
  definitions and `extern`, `static` locals, local initializers (incl.
  brace lists and `char s[] = "..."`).
* **Preprocessor:** `#include "..."` and `<...>` (searched in `lib/`),
  `#define` (object- and function-like), `#undef`, `#ifdef`, `#ifndef`,
  `#if`/`#elif`/`#else`/`#endif` with constant expressions and
  `defined()`, `#error`; `/* */` and `//` comments; `__RISKY__` is
  predefined.
* `__asm__("...")` statements insert raw assembler (strings are
  concatenated, escapes like `\n`/`\t` apply).
* Unused functions are removed from the output (`--keep-dead` disables
  this).

## Deviations from C89 (word addressed 16 bit machine)

* `sizeof(char) == sizeof(short) == sizeof(int) == sizeof(long) ==
  sizeof(T *) == 1` — one 16 bit word. All `mem*`/`malloc` sizes are in
  words. `long` is only 16 bit (warned).
* `unsigned` is accepted but arithmetic, comparison, `>>`, `/`, `%` are
  always signed 16 bit (`>>` is logical/unsigned though, matching the
  hardware `shr`).
* No `float`/`double` (no FPU — compile error), no bit fields, no
  returning structs by value, no K&R-style parameter declarations.
* One translation unit, no linker: `#include` your other sources; the
  libc is header-only for the same reason.

## Memory map (all data in page 0)

```
0x0000 … globals and string literals (initialized by startup code)
0x8000 … heap (lib/stdlib.h bump allocator, free() is a no-op)
0xffff … stack, grows down (sp is initialized by startup code)
page 0xa, address 0x0:  terminal output register (write a char to print)
page 0xb:               read-only data ROM = stdin (getchar reads it,
                        cursor kept in page 9, address 0)
```

Startup code (emitted before `main`) selects page 0, sets
`sp = 0xffff`, stores all global/string initializers into RAM, calls
`main` and halts on `__halt: jmp __halt` (the simulator detects the
jump-to-self and stops).

**Important hardware insight:** `push`/`pop` and all r15-relative
accesses go through the *paged* RAM (SP and PAGE both feed the same RAM
address bus in RISKY.circ). Code must never touch the stack while a
page other than 0 is selected — the v3 libc switches pages only around
single `ldr`/`stod` instructions. (The v1-generated `putchar` pushed
return addresses while page 0xa was selected, which the terminal
silently drops, so v1 binaries could not actually print on the real
circuit.)

## ABI (unchanged from v1/v2)

* `r0` accumulator / return value, `r1`–`r3` scratch, `r15` frame
  pointer; no callee-saved registers.
* Call: arguments pushed right to left (structs by value, word by
  word), `call` pushes the return address, callee runs
  `push r15; mov r15,sp` and reserves its whole frame at once.
  Parameter *i* starts at `r15+3`, locals grow down from `r15-0`.
  Caller pops the arguments.
* Varargs work naturally: `int *ap = &fixedParam + 1;` walks the
  caller's pushed arguments (this is how `lib/stdio.h` implements
  `printf`).

## Assembler improvements over v1

* labels are resolved in **any** operand position (`in r0,#myfunc`
  gives the code address of `myfunc` — used for function pointers)
* labels are case sensitive; mnemonics/registers stay case insensitive
* new `callr rX` pseudo instruction (call through register)
* real error messages instead of silently emitting `0` words
* output is byte-for-byte identical to the v1 assembler for all
  existing `.asm` files in this repository

## Simulator

`risky_sim.py prog.hex [-i inputfile] [--max-cycles N] [--trace] [-v]`

Implements the full instruction set (validated against
`Assembler/division.asm`'s built-in self test, `stack.asm`,
`terminal.asm` and the v1 compiler output), pages 0–9 as RAM, the
terminal on page 0xa and the input ROM on page 0xb. `-v` dumps
registers on exit, `--trace` logs every instruction to stderr.
`cmp` compares signed; flag bits follow the v2 compiler's layout
(lt=0x04, eq=0x10, neq=0x20, gt=0x40).
