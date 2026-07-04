# RISKY — A Homebrew 16-bit CPU

A self-designed 16-bit word-addressed CPU built in Logisim, with an assembler,
C89 compiler, simulator, and a small multitasking OS.

## Directory layout

```
RISKY/
├── hardware/         Logisim circuit + microcode
│   ├── RISKY.circ    Main circuit
│   └── microcode/    Microcode ROM images
├── compiler/         C89 compiler, assembler, simulator
│   ├── risky_c.py    C compiler (C89 subset)
│   ├── risky_asm.py  Assembler
│   ├── risky_sim.py  ISA simulator
│   ├── cc            Driver script (compile → assemble → simulate)
│   ├── lib/          Header-only libc (stdio, stdlib, string, ctype, softfloat)
│   └── tests/        17 language conformance tests
├── os/               Multitasking OS + demos
│   ├── kernel.c      Cooperative multitasking OS
│   ├── rom.txt/hex   Shell input (simulator / Logisim ROM)
│   └── demos/        Standalone programs
└── docs/             Documentation
    ├── isa.txt       Instruction set reference
    ├── compiler.md   Compiler design, pipeline, dialect, deviations
    ├── assembler.md  Assembler pipeline, pseudo-ops, encoding formats
    ├── simulator.md  Machine model, I/O, flags, hardware differences
    ├── os.md         Kernel architecture, IPC, FS, shell
    └── roadmap.md    Future features and ideas
```

## Quick start

```sh
# Compile, assemble, and simulate a C program
compiler/cc -r compiler/tests/sieve.c

# Run the OS
compiler/cc -r os/kernel.c os/rom.txt

# Build for Logisim
compiler/cc os/demos/sierpinski.c     # produces sierpinski.hex
# Load sierpinski.hex into the instruction ROM in RISKY.circ
```

## Hardware

- 16-bit word-addressed, 16 general-purpose registers
- Paged memory: pages 0–9 (RAM), 0xa (terminal output), 0xb (input ROM)
- Stack grows down from 0xffff in page 0
- Microcoded ALU with hardware multiply (lmul/hmul), divide, shift
- See `docs/isa.txt` for the full instruction set

## Compiler

Nearly C89-compatible. Supported: pointers, arrays, structs/unions, enums,
typedef, function pointers, switch/goto, `__naked` functions, `__asm__`,
preprocessor (`#include`, `#define`, `#if`).  Not supported: float/double
(use `lib/softfloat.h` for binary16 software float), bit fields, K&R syntax.

## OS features

- Cooperative multitasking with true stack switching
- Process table: spawn/exit/wait/zombie reaping
- Message-passing IPC with blocking send/recv
- File descriptors: stdin (ROM), stdout/stderr (terminal), files, pipes
- Hierarchical file system with directories
- Shell that reads scripts from the input ROM

## Demos

| Program | Description |
|---------|-------------|
| `os/demos/sierpinski.c` | Sierpinski triangle via Rule 90 automaton |
| `os/demos/rule30.c` | Wolfram Rule 30 chaotic pattern |
| `os/demos/bounce.c` | Bouncing ball animation |
| `os/demos/mandel.c` | Mandelbrot set (fixed-point) |
| `os/demos/sf.c` | Softfloat library test (binary16) |
| `os/demos/newton.c` | Newton's method with softfloat |
| `os/demos/kernel.c` | Simple round-robin executive (v1) |
