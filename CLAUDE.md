# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Commands

```sh
# Run all compiler tests
python3 compiler/tests/run_tests.py

# Compile, assemble, and simulate a single C program
compiler/cc -r os/demos/sierpinski.c

# Compile + assemble only (produces .hex for Logisim)
compiler/cc os/demos/sierpinski.c

# Simulate a pre-built .hex with stdin from a file
python3 compiler/risky_sim.py prog.hex -i input.txt

# Simulate with register dump and trace
python3 compiler/risky_sim.py prog.hex -i input.txt -v --trace --max-cycles 50000
```

## Architecture

### Hardware (RISKY.circ)

16-bit word-addressed CPU in Logisim. 16 registers (r0–r15). Paged memory: pages 0–9 are RAM, page 0xa is write-only terminal (address 0 prints the low byte), page 0xb is read-only input ROM. No interrupts, no keyboard — the terminal is output-only. **SP and PAGE share the same RAM address bus**, so push/pop go through paged RAM. Never touch the stack while a page other than 0 is selected.

Full ISA is in `docs/isa.txt`. Key hardware behaviors that differ from C expectations:
- `DIV`/`MOD` are **unsigned** — the compiler emits `__sdiv`/`__smod` runtime helpers to wrap them with sign handling
- `SHR` is **logical** (unsigned) right shift — `>>` on signed ints gives the wrong result for negative values; use `/` for signed division
- `CMP` sets eq/lt/gt flags but does **not** set the sign flag in the simulator

### Compiler (`risky_c.py`, ~3100 lines)

Single-pass design: regex lexer → recursive-descent parser → AST classes with `generate()` methods that emit RISKY assembly text. No intermediate representation, no optimization passes.

**Key classes:**
- `Preprocessor` — handles `#include`/`#define`/`#if` before lexing, produces a flat token stream
- `Parser` — recursive descent, produces AST nodes
- `Codegen` (via `Node.generate(scope)`) — each node emits asm text. `Scope` tracks local variable offsets relative to r15. Global variables are allocated from address 0 upward by `Codegen.allocGlobal()`
- `Program.generate()` — emits startup code (init page, sp, globals, strings, call main, halt), then all function bodies, then any needed runtime helpers (`__sdiv`, `__smod`)

**Startup code** initializes page to 0, sp to 0xffff, writes global/string initializers into RAM, calls `main`, then jumps to `__halt: jmp __halt` (simulator detects this jump-to-self as termination).

**Dead code elimination:** after generation, functions not reachable from main are stripped (`--keep-dead` disables).

**`__naked` functions:** skip the C prologue/epilogue (no `push r15; mov r15,sp` / epilogue). Body must be pure `__asm__`. Used for context-switch routines in the OS.

**Include path:** `#include <...>` searches `compiler/lib/` by default. `#include "..."` also searches the including file's directory.

### Assembler (`risky_asm.py`, ~250 lines)

Table-driven: opcode mnemonics → encoding formats (rrr, rr, r, ri, ir, rir, rri, i, n) → 32-bit hex words. Output is Logisim "v2.0 raw" format.

**Pseudo-ops:** `call` (ldpc/addi/push/jmp), `callr` (call through register), `ret` (pop/jmpr), auto-conversion of `op r,reg` to `opi r,#imm` when operand is not a register, `push #imm` → `pushi`, bare `pop` → `popi`, 32-bit register pair expansion.

**Labels:** resolved in any operand position (not just jumps), case-sensitive. Mnemonics and register names are case-insensitive.

### Simulator (`risky_sim.py`, ~350 lines)

Instruction-level interpreter. Executes .hex files directly. Models all ISA features including paged memory, terminal output (page 0xa), and ROM input (page 0xb). Input files can be plain text (each character → one ROM word) or "v2.0 raw" hex format. Halts on jump-to-self detection (the `__halt: jmp __halt` idiom).

### OS (`os/kernel.c`, ~700 lines)

Cooperative multitasking with true stack switching via `__naked yield()`. Scheduler is round-robin. Fixed addresses 0x7FF0-0x7FF3 bridge asm and C during context switches (save sp/r15, call C scheduler, restore next task's sp/r15, ret into it).

Process model: spawn/exit/wait with zombie reaping. Message-passing IPC with ring buffers and blocking send/recv. File descriptors (0=ROM stdin, 1-2=terminal, 3+=files/pipes). Flat→hierarchical FS with directories stored as inode tables in page 0 RAM, data blocks at 0x8000+.

The shell reads commands from the input ROM (page 0xb); there is no keyboard.

### libc (`compiler/lib/`)

Header-only (no linker). `stdio.h` provides `putchar`/`getchar`/`puts`/`printf` (varargs via `&fixedParam + 1`). `stdlib.h` provides `malloc` (bump allocator from 0x8000, `free` is no-op). `string.h` provides standard string/memory functions. `softfloat.h` provides IEEE-754 binary16 software floating point (add/sub/mul/div/cmp/print). `float32.h` provides IEEE-754 binary32 backing the compiler's `float` type: full 24-bit-mantissa add/sub/mul/div (round toward zero, ≤1 ulp; denormals flush to zero), comparison, int/long conversions, and decimal/hex printing. Programs using `float` arithmetic or casts must `#include <float32.h>`. `long` is the library's carrier type: `f32_*` functions traffic in raw bit patterns, so `long`↔`float` conversion is a bit-level pass-through (use `f32_from_long`/`f32_to_long` for numeric conversion), while `int`↔`float` converts numerically.

## Key constraints

- **C89 only:** declarations must precede statements in each block. No `for (int i = ...)`. No `//` comments in C code (though the preprocessor strips them).
- **`>>` on signed values:** the hardware `shr` is logical, not arithmetic. The compiled `>>` gives wrong results for negative values. Use division instead.
- **`call` clobbers r0:** the assembler's `call` pseudo-op expands to `ldpc r0; addi r0,#4; push r0; jmp`. Any value in r0 is destroyed. The `__sdiv`/`__smod` helpers and the OS use a custom `ldpc r2` sequence instead.
- **No linker:** single translation unit. Use `#include` to combine sources. The libc is header-only for this reason.
- **`sizeof` everything is 1** (one 16-bit word). All `malloc`/`memcpy` sizes are in words, not bytes.
