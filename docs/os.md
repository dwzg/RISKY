# RISKY OS (`os/kernel.c`)

A cooperative multitasking operating system with true stack switching,
message-passing IPC, a hierarchical file system, and a shell. ~700 lines
of C, runs on the bare-metal RISKY CPU (no interrupts).

## Architecture

### Boot sequence

```
_start → init page 0, sp=0xffff, call main
main   → init kernel arrays, init FS, spawn shell, event loop
shell  → read commands from page 0xb ROM, execute, yield
halt   → jmp __halt (detected by simulator)
```

### Memory layout (all page 0)

```
0x0000–0x1FFF   kernel globals + libc data + compiler globals
0x2000–0x5FFF   process stacks (512 words × 16 procs)
0x6000–0x7FFF   file descriptor table, pipe buffers
0x7FF0–0x7FF3   context-switch bridge (fixed addresses for asm↔C)
0x8000–0xEFFF   file system data blocks
0xF000–0xFFFF   main stack (grows down from 0xffff)
page 0xa, @0    terminal output register
page 0xb         input ROM (shell script)
page 9, @0       stdin cursor (read by libc getchar)
```

### Context switch mechanism

Fixed addresses `0x7FF0`–`0x7FF3` bridge the asm context-switch routine
and the C scheduler:

| Address | Direction | Purpose |
|---------|-----------|---------|
| 0x7FF0 | asm → C | Saved stack pointer of yielding task |
| 0x7FF1 | asm → C | Saved frame pointer (r15) of yielding task |
| 0x7FF2 | C → asm | Stack pointer of next task to run |
| 0x7FF3 | C → asm | Frame pointer (r15) of next task to run |

The `yield()` function is declared `__naked` (no C prologue/epilogue).
Its asm body:
1. Reads `sp` via `ldsp`, stores to 0x7FF0
2. Stores `r15` to 0x7FF1
3. Calls the C function `__sched()`, which:
   - Reads 0x7FF0/1 → `task_sp[cur]` / `task_r15[cur]`
   - Round-robins to the next ready task, updates `cur_pid`
   - Writes `task_sp[next]` / `task_r15[next]` → 0x7FF2/3
4. Loads `sp` from 0x7FF2 via `stosp`
5. Loads `r15` from 0x7FF3
6. Executes `ret` — pops the next task's return address from its
   stack and jumps there

The initial task stack is set up so that `ret` enters the task function.
`saved_sp` points one word below a fake "return address" (the function
pointer), so `ret` (pop + jmpr) jumps to the entry point.

### Process model

**Process table:** 16 slots. Each task has:
- `sp`, `r15` — saved context (valid when task is not running)
- `state` — 0=free, 1=ready, 2=blocked, 3=zombie
- `ppid` — parent process ID
- `exit_code` — set by `proc_exit()`, reaped by `proc_wait()`
- `wait4` — PID being waited on (-1=any child, -2=message)
- `msgs[]` / `msgcnt` / `msgnext` — ring buffer for IPC

**System calls** (regular C functions, no trap mechanism):
- `proc_spawn(fn, ppid, name)` → pid
- `proc_exit(code)` — zombie until parent reaps
- `proc_wait(pid)` — blocks until child exits, returns exit code
- `getpid()` → current PID
- `send_msg(dst, msg)` / `recv_msg(&msg)` — blocking IPC

**Scheduler:** round-robin. `sched_next(cur)` skips non-ready tasks.
No priorities, no time slicing (cooperative only).

**Init (PID 0, `main`):** spawns child processes, then loops: `yield()`
until `alive()` returns false, then reaps zombies and halts.

### IPC

Message queues are 8-slot ring buffers in the process table.
`send_msg()` fails if the queue is full or the destination is not
running/blocked. `recv_msg()` blocks (state=2, wait4=-2) until a message
arrives. When a message is enqueued and the receiver is blocked on
messages, it is woken.

### File descriptor table

32 FDs. Types: `FD_FREE` (0), `FD_FILE` (1), `FD_PIPE_RD` (2),
`FD_PIPE_WR` (3), `FD_TERM` (4), `FD_ROM` (5).

| FD | Default | Purpose |
|----|---------|---------|
| 0 | ROM | stdin — reads from page 0xb via `getchar()` |
| 1 | TERM | stdout — writes to page 0xa |
| 2 | TERM | stderr — writes to page 0xa |
| 3+ | (free) | Files, pipe ends |

Each FD tracks type, inode/pipe index, offset, and a reference count.
`fd_close()` decrements the refcount; the slot is freed when it hits 0.

`sys_read(fd, buf, words)` and `sys_write(fd, buf, words)` dispatch on
FD type. Terminal writes go through `putchar()`. ROM reads go through
`getchar()` (which maintains a cursor in page 9).

### Pipes

8 pipes, each a 256-word ring buffer. `sys_pipe(fds[2])` allocates two
FDs (read end, write end). `pipe_read()`/`pipe_write()` operate on the
ring buffer. Reference counting tracks reader/writer liveness; a pipe
is freed when both ends are closed.

### File system

**Storage:** inode metadata in page 0 arrays (`fs_type[64]`,
`fs_size[64]`, `fs_blocks[64][8]`, `fs_name[64][32]`). Data blocks at
`0x8000+` in page 0 (~28K available). Block size 256 words. 8 direct
blocks per inode (max file size: 2048 words). Simple bump allocator
for blocks (starts at 256 so 0 = unallocated).

**Inode types:** 0=free, 1=regular file, 2=directory.

**Directories:** a directory inode contains a sequence of entries:
`[inode_number, name_length, name_chars...]`. Inode number -1 marks a
free slot (entries are never deleted, only marked free).

**Path resolution:** `fs_resolve("/dir/sub/file")` walks components
from the root inode (0), calling `fs_dir_lookup()` at each level.

**Operations:**
- `fs_ialloc(type, name)` → inode number (allocates a free inode)
- `fs_dir_add(dir_ino, name, file_ino)` — appends an entry
- `fs_dir_lookup(dir_ino, name)` → inode number (linear scan)
- `fs_readi(ino, offset, buf, words)` / `fs_writei(ino, offset, buf, words)`
- `fs_write_str(ino, s)` — convenience, auto-grows

### Shell

Reads commands from stdin (FD 0, which is the ROM input). Each command
is a line; the shell echoes characters as they are read. Commands:

| Command | Description |
|---------|-------------|
| `help` | Print available commands |
| `ps` | List processes (PID, PPID, state, name) |
| `ls [path]` | List directory contents |
| `cat path` | Print file contents |
| `mkfile path` | Create a regular file |
| `mkdir path` | Create a directory |
| `write path text` | Write text to a file (truncates) |
| `echo text` | Print text |
| `pipe` | Create a pipe, print the FD numbers |
| `spawn` | Spawn a second shell instance |
| `exit` | Exit the shell |

Path parsing in `mkfile`/`mkdir` splits at the last `/` to determine
the parent directory.

### Pre-populated filesystem

```
/                    (root dir, inode 0)
├── motd             "Welcome to RISKY OS v3!"
├── secrets          "the answer is 42"
├── etc/             (dir)
│   └── passwd       "root:x:0:0:root:/:/bin/sh\n"
└── bin/             (dir, empty)
```
