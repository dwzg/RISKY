# Known Issues

## Compiler

### fs_resolve fails with non-literal string pointers at certain call depths

**Status:** Worked around (kernel-side), root cause unknown.

`fs_resolve()` returns -1 when called with a pointer to a non-literal
string (variable, array element, file-scope global buffer) from within
the pipe-spawn loop or from within pipe-segment processes.  String
literals (e.g. `fs_resolve("/motd")`) always work, as do calls from
`shell_task` and the top of `sh_run_pipeline`.

The trigger appears to be any variable-indexed array read (`arr[i]`)
performed before the `fs_resolve` call — the read somehow corrupts the
compiler's ability to pass the string pointer correctly through the
call chain.  The array size also matters: a 6-word local works, a
17-word local does not.

**Workaround in the kernel:** File paths are pre-resolved with
`fs_resolve()` at the top of `sh_run_pipeline()` (before pipe creation
and spawning).  The resolved inode is stored in `sh_seg_ino[pid]` and
consumed by `sh_cat_inode()`, avoiding any `fs_resolve` call inside
the spawn loop or pipe segment.

**Reproducing:** Move the `fs_resolve(ta)` call from the top of
`sh_run_pipeline` into the spawn loop (around `targ`), or call it from
`sh_pipe_segment_task`.  `cat /motd | cat` will break with "not found".

---

### Global arrays accessed with variable index may produce wrong addresses

**Status:** Suspected, not confirmed.

In the spawn loop of `sh_run_pipeline`, writing to `sh_seg_ino[pid]`
with a file-scope global buffer and reading it back via
`sh_seg_ino[cur_pid]` in the child process appeared to access
different memory locations (the child read 0).  The generated assembly
looked correct, suggesting a runtime addressing issue.

This may be the same underlying bug as the `fs_resolve`
string-pointer issue, or a separate register-allocation problem.

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
