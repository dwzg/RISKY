# Roadmap — Future Ideas

Features discussed but not yet implemented. Roughly ordered by impact.

## OS improvements

### IPC & concurrency
- ✅ **Proper pipe semantics:** `pipe_read` blocks when the buffer is
  empty (if writers still exist), returns 0 on EOF. `pipe_write` blocks
  when the buffer is full (if readers still exist), returns -1 on broken
  pipe. Wakeup tracking via `pipe_wait_rd`/`pipe_wait_wr`.
- ✅ **Pipe between processes in the shell:** `echo hello | cat` —
  pipeline parsing (`|`), `sys_dup2` for FD redirection, per-segment
  processes spawned with priority ordering (writer before reader).
- **Signals:** simple software signals (SIGCHLD for child exit, SIGPIPE
  for broken pipe). No signal handlers needed initially — just default
  actions.
- ✅ **Priority scheduling:** `p_priority` field; `sched_next()`
  chooses the highest-priority ready task with round-robin tie-breaking
  among equal priorities.
- ✅ **Sleep/wakeup:** `proc_sleep(ticks)` — block for N scheduler
  activations. `sys_tick` incremented on each context switch. Sleepers
  auto-wake when `sys_tick >= p_sleep_until`.

### File system
- **Multi-page storage:** use pages 1–7 for file data. Inode block
  pointers encode `(page << 16) | addr`. Requires careful page switching
  during I/O — all stack accesses must complete before switching pages.
  Need a kernel buffer at a fixed address to bounce data through.
- **Indirect blocks:** add a 9th block pointer that points to a block of
  256 block pointers. Increases max file size from 2048 to ~65K words.
- **Free block bitmap:** replace the bump allocator with a proper bitmap
  across pages 1–7. Enables file deletion and space reuse.
- **File deletion:** `unlink(path)` — mark inode free, free its blocks,
  remove the directory entry.
- **File truncation:** `ftruncate(fd, size)` — grow or shrink a file.
- **Subdirectories in path resolution:** the current shell only creates
  files in the root or one level deep. Path resolution already supports
  arbitrary depth; the shell commands need updating.
- **`.`` and `..` directory entries:** standard Unix convention.
- **File permissions:** simple owner/group/other read/write bits.
  Cosmetic with no user authentication, but useful for the API.
- **Device files:** `/dev/tty` (terminal), `/dev/rom` (input ROM),
  `/dev/pipe` — open by path instead of special syscalls.
- **Mount points:** mount a page as a filesystem volume.

### Executable programs
- **ELF-like binary format:** a header with magic, entry point, code
  size, data size. `sys_exec(path)` loads the binary from the FS,
  copies code/data into a new process's memory, and spawns it.
- **Shared libraries:** load a library binary once, map it into multiple
  processes. Needs position-independent code or a linking loader.
- **Shell as a user program:** the shell should be loadable via
  `sys_exec("/bin/sh")` rather than compiled into the kernel.
- **`#!` script support:** if a file starts with `#!`, interpret the
  rest of the first line as a command to run with the file as input.

### Device drivers
- **Block device abstraction:** `bread(dev, block)` / `bwrite(dev, block)`.
  The FS sits on top of this instead of raw page access.
- **Terminal driver:** line buffering, backspace handling, cursor
  positioning escape sequences (if the Logisim TTY supports them).
- **Timer interrupt:** if the Logisim circuit is extended with a timer,
  add preemptive scheduling (time slices) and `sleep()`.

## Compiler improvements

### Language features
- **`const`:** ✅ warns on assignment to const-qualified variables.
  No read-only section yet.
- **`volatile`:** ✅ qualifier tracked through type system. No codegen
  effect yet (relevant for memory-mapped I/O).
- **`static` functions:** file-scope visibility. Currently all functions
  are global (dead-code elimination partially mitigates this).
- **K&R-style function definitions:** `int foo(a, b) int a; char *b; { ... }`
- **`continue` in `for` loops:** ✅ verified correct — continue jumps to
  the post-expression label before the condition re-check.
- **Wider integer types:** ✅ 32-bit `long` using register pairs (er0–er7)
  with full arithmetic, comparison, function params/returns.
- **`unsigned` arithmetic:** ✅ unsigned comparison (XOR 0x8000 trick),
  unsigned division/modulo, unsigned right shift.
- **Single-precision float:** ✅ `float` type, literals, arithmetic
  operators, comparisons, int↔float conversions (casts, assignments,
  arguments, returns, mixed arithmetic), compound assignment, unary
  minus, truth tests. Runtime library (`float32.h`) with IEEE-754
  binary32 add/sub/mul/div at full 24-bit mantissa precision (round
  toward zero, ≤1 ulp; subnormals flush to zero), int/long conversions,
  and decimal/hex printing. Validated against a host-side IEEE-754
  reference on randomized vectors.

### Optimisation
- **Peephole optimiser:** ✅ basic pass removes `push r0; pop r0` and
  `push r0; pop r1 → mov r1,r0` patterns.
- **Register allocation:** currently r0 is the sole accumulator.
  Using r1–r3 for temporary values could reduce push/pop overhead.
- **Constant folding in codegen:** ✅ compile-time evaluation of constant
  arithmetic, bitwise, and equality expressions.
- **Tail call optimisation:** ✅ intra-function tail calls (same function
  name, any arguments). Cross-function TCO disabled (ABI issue).

### Debugging
- **Source-level debugging:** emit line number information as asm
  comments. The simulator could map PC → source line.
- **`__FILE__` and `__LINE__` macros:** ✅ implemented in preprocessor.
- **`#pragma message`:** print a diagnostic during preprocessing.
- **`-Wall` mode:** enable warnings for implicit declarations, unused
  variables, type mismatches.

## Simulator improvements

- **TTY emulation:** 32×8 character display with proper wrapping and
  scrolling, matching the Logisim TTY dimensions.
- **Memory watchpoints:** `--watch 0xADDR` to log reads/writes to a
  specific address.
- **Profile mode:** `--profile` to count instructions by opcode and
  report hot functions.
- **Interactive debugger:** breakpoints, single-step, register/memory
  inspection. `--debug` flag drops into a REPL.
- **Verilog/VCD output:** generate a waveform trace for debugging the
  Logisim circuit against the simulator.
- **Coverage:** track which instructions are exercised by a test
  program; report gaps in ISA coverage.

## Toolchain integration

- **Makefile / build system:** a `Makefile` or `build.py` that compiles
  the OS and all demos with one command.
- **`as` compatibility mode:** accept GNU as syntax (`.globl`, `.word`,
  `.section`) to simplify porting existing assembly code.
- **Binary utilities:** `objdump`-style disassembler for `.hex` files.
- **ROM image builder:** combine multiple `.hex` files (bootloader,
  kernel, user programs) into a single ROM image for the Logisim
  instruction ROM.

## Hardware (RISKY.circ) ideas

- **Keyboard input:** add a Logisim Keyboard component to the circuit,
  wired to page 0xb or a new page. The `jkb` instruction already exists
  in the ISA for this purpose.
- **Timer/counter:** a hardware timer generating periodic interrupts.
  Enables preemptive scheduling and `sleep()`.
- **UART/serial:** a simple serial transmitter for communicating with
  another RISKY instance or a host PC.
- **Bitmapped display:** a larger display buffer (e.g. 128×64 pixels)
  mapped to a dedicated page. Enables graphics demos.
- **Memory-mapped ALU:** expose ALU operations as memory-mapped
  registers so the CPU can read result flags without `LDSTATE`.

## OS userland

- **`init` process:** a proper init that reads `/etc/inittab` and spawns
  getty processes on terminals.
- **`getty` + `login`:** prompt for username/password, authenticate
  against `/etc/passwd`, exec the user's shell.
- **Standard Unix utilities:** `cp`, `mv`, `rm`, `mkdir -p`, `wc`,
  `head`, `tail`, `grep`, `sort`, `diff`.
- **Ed/sed:** a simple line editor or stream editor.
- **A BASIC interpreter:** integer BASIC, interactive via the terminal.
- **A FORTH system:** FORTH is a natural fit for a small word-addressed
  machine — the dictionary and stack map directly to hardware concepts.
- **Network stack:** if a UART is added, implement SLIP or PPP, then a
  minimal TCP/IP stack (or just UDP for simple datagrams).
