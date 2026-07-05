# Known Issues

## Compiler

No known compiler bugs at this time.  (The `fs_resolve` issue
previously attributed to the compiler was traced to a shell argument
parsing bug — see OS / Kernel below.)

---

## OS / Kernel

### Shell pipe argument parsing includes trailing whitespace (FIXED)

**Status:** Fixed (2026-07-05).

The argument parser in the spawn loop of `sh_run_pipeline` copied
characters until null (`\0`), which included the space before the `|`
pipe character.  A command like `cat /motd | cat` would pass
`/motd ` (with trailing space) as the file path, causing `fs_resolve`
to fail with -1 ("not found").

The pre-resolve step at the top of `sh_run_pipeline` used a different
parser that stopped at spaces, which is why pre-resolving worked.

**Fix:** The spawn loop argument parser (`for (k = 0; s[j+k]; k++)`)
now also stops at spaces and tabs, matching the pre-resolve parser's
behavior.

### Global FD table (not per-process)

---

## OS / Kernel

### Global FD table (not per-process)

**Status:** Inherent design limitation.

All processes share a single FD table.  Closing an FD in one process
closes it for everyone.  This forces careful ordering in the shell
pipe implementation:
- Pipe segments must run in priority order (writer before reader).
- Segments restore fd 1 to the terminal via `sys_dup2(2, 1)` on exit
  rather than closing it, so the slot stays valid for the next segment.
- Children must not close pipe ends that other children still need.

A per-process FD table (or at least per-process reference counting)
would eliminate these constraints.

---

### No automatic FD cleanup on process exit

**Status:** Inherent design limitation.

`proc_exit()` does not close the exiting process's open FDs.  This is
a consequence of the global FD table — without per-process tracking,
the kernel cannot know which FDs belong to which process.
Pipe segments work around this by explicitly restoring/cleaning up
fd 0 and fd 1 before exiting.

---

### Stack size is 512 words per process

**Status:** Monitor if stack usage grows.

Each process gets `STK_WORDS = 512` words of stack from a shared pool
(`stk_pool[16][512]`).  Functions with large local arrays (e.g. `buf[128]`
in `sh_write_cmd`) can consume significant stack depth when combined with
deep call chains.  The `sh_write_cmd` function was extracted from
`sh_exec_cmd` specifically to keep the common dispatch path lean.

Signs of stack overflow: seemingly impossible failures in pure functions
(like `fs_resolve` returning -1 for valid paths), or corrupted global
state after deep call chains.

---

### Path length limited to 31 characters in shell pipes

**Status:** Minor, unlikely to matter.

`sh_run_pipeline` copies only the first argument into a 32-word buffer
(`ta[32]`).  Longer paths are truncated and will fail resolution.
Standalone commands use the shell's `arg[64]` buffer (63 usable chars).

---

### File reads through pipes only work for cat

**Status:** Could be generalized.

The pre-resolve workaround (`sh_seg_ino[]`) is currently only consumed
by the `cat` handler in `sh_exec_cmd`.  Other commands that open files
(`write`, hypothetical `head`, `wc`, etc.) would need similar handling
to work inside pipe segments.  The pattern is straightforward to
replicate: check `sh_seg_ino[cur_pid]` before falling through to
`fs_resolve(arg)`.

---

## Simulator

### No keyboard input in headless mode

**Status:** By design.

The simulator's keyboard buffer is populated from the `-k <file>` flag.
Without it, `kbhit()` always returns false and the shell blocks
indefinitely on `sh_readln`.  Test scripts must provide input via `-k`.

---

### No timer / interrupt support

**Status:** Requires hardware changes to RISKY.circ.

Preemptive multitasking requires a hardware timer and CPU interrupt
support.  The current CPU has no interrupt mechanism — all context
switches are cooperative (via `yield()`).  See `docs/roadmap.md` for
the full list of hardware prerequisites.
