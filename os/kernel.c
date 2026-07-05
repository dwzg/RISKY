/* RISKY OS v3 — FDs, pipes, multi-page FS with directories.
 *
 * FD table:  0=stdin(ROM) 1=stdout(term) 2=stderr(term) 3+=files/pipes
 * Pipes:     kernel ring buffer, read-end + write-end FDs
 * FS:        pages 1-7, inodes in page 0, block pointers to pages 1-7,
 *            directories, path resolution (/dir/sub/file)
 */

#include <stdio.h>
#include <string.h>

/* ================================================================
 *  KERNEL
 * ================================================================ */
#define MAX_PROCS   16
#define STK_WORDS   512
#define MSG_Q       8

/* process states */
#define S_FREE      0
#define S_READY     1
#define S_BLOCKED   2
#define S_ZOMBIE    3
#define S_SLEEP     4

#define SAVE_SP    0x7FF0
#define SAVE_R15   0x7FF1
#define NEXT_SP    0x7FF2
#define NEXT_R15   0x7FF3

int p_sp[MAX_PROCS], p_r15[MAX_PROCS];
int p_state[MAX_PROCS];      /* 0=free 1=ready 2=blocked 3=zombie 4=sleep */
int p_ppid[MAX_PROCS];
int p_exit[MAX_PROCS];
int p_wait4[MAX_PROCS];
int p_priority[MAX_PROCS];   /* higher number = higher priority */
int p_sleep_until[MAX_PROCS]; /* tick when sleep expires, 0=none */
int p_msgs[MAX_PROCS][MSG_Q];
int p_msgcnt[MAX_PROCS];
int p_msgnext[MAX_PROCS];
int p_name[MAX_PROCS][4];
int cur_pid;
int sys_tick;                 /* scheduler tick counter */

static int stk_pool[MAX_PROCS][STK_WORDS];

/* Wake sleepers whose time has come, then pick the highest-priority
 * ready process.  Round-robin tie-break among equal priorities. */
int sched_next(int cur)
{
    int i, best, best_pri;
    /* wake expired sleepers */
    for (i = 0; i < MAX_PROCS; i++) {
        if (p_state[i] == S_SLEEP && p_sleep_until[i] > 0
            && sys_tick >= p_sleep_until[i]) {
            p_state[i] = S_READY;
            p_sleep_until[i] = 0;
        }
    }
    /* highest priority among ready procs */
    best_pri = -1;
    for (i = 0; i < MAX_PROCS; i++)
        if (p_state[i] == S_READY && p_priority[i] > best_pri)
            best_pri = p_priority[i];
    if (best_pri < 0) return cur;   /* nothing ready — idle */
    /* round-robin within that priority band, starting after cur */
    for (i = 1; i <= MAX_PROCS; i++) {
        int n = (cur + i) % MAX_PROCS;
        if (p_state[n] == S_READY && p_priority[n] == best_pri)
            return n;
    }
    return cur;
}

void __sched(void)
{
    int *s = (int *) SAVE_SP;
    p_sp[cur_pid]  = s[0];
    p_r15[cur_pid] = s[1];
    sys_tick++;
    cur_pid = sched_next(cur_pid);
    s[2] = p_sp[cur_pid];
    s[3] = p_r15[cur_pid];
}

/* Block the current process for at least `ticks` scheduler activations. */
__naked void yield(void);
void proc_sleep(int ticks)
{
    if (ticks <= 0) return;
    p_sleep_until[cur_pid] = sys_tick + ticks;
    p_state[cur_pid] = S_SLEEP;
    yield();
}

__naked void yield(void)
{
    __asm__(
        "\n\tldsp r0\n\tstod *" "0x7FF0" ",r0"
        "\n\tstod *" "0x7FF1" ",r15"
        "\n\tcall __sched"
        "\n\tldd r0,*" "0x7FF2" "\n\tstosp r0"
        "\n\tldd r15,*" "0x7FF3" "\n\tret"
    );
}

int proc_spawn(void (*fn)(void), int ppid, char *name)
{
    int pid, i, *stack, *sp;
    for (pid = 0; pid < MAX_PROCS; pid++)
        if (p_state[pid] == S_FREE) break;
    if (pid == MAX_PROCS) return -1;
    stack = stk_pool[pid];
    sp = stack + STK_WORDS - 4;
    sp[1] = (int) fn; sp[0] = 0; sp[-1] = (int) pid;
    p_sp[pid] = (int) sp; p_r15[pid] = (int)(sp + 1);
    p_state[pid] = S_READY; p_ppid[pid] = ppid; p_exit[pid] = 0;
    p_wait4[pid] = -1; p_msgcnt[pid] = 0; p_msgnext[pid] = 0;
    p_priority[pid] = 0; p_sleep_until[pid] = 0;
    for (i = 0; name[i] && i < 4; i++) p_name[pid][i] = name[i];
    for (; i < 4; i++) p_name[pid][i] = 0;
    return pid;
}

void proc_exit(int code)
{
    int pp = p_ppid[cur_pid];
    p_exit[cur_pid] = code; p_state[cur_pid] = S_ZOMBIE;
    if (pp >= 0 && p_state[pp] == S_BLOCKED
        && (p_wait4[pp] == cur_pid || p_wait4[pp] == -1))
        p_state[pp] = S_READY;
    while (1) yield();
}

int proc_wait(int pid)
{
    int i;
    for (i = 0; i < MAX_PROCS; i++)
        if (p_state[i] == S_ZOMBIE && p_ppid[i] == cur_pid
            && (pid == -1 || pid == i))
        { int c = p_exit[i]; p_state[i] = S_FREE; return c; }
    p_wait4[cur_pid] = pid; p_state[cur_pid] = S_BLOCKED; yield();
    for (i = 0; i < MAX_PROCS; i++)
        if (p_state[i] == S_ZOMBIE && p_ppid[i] == cur_pid
            && (pid == -1 || pid == i))
        { int c = p_exit[i]; p_state[i] = S_FREE; return c; }
    return -1;
}

int send_msg(int dst, int msg)
{
    int w;
    if (p_state[dst] != S_READY && p_state[dst] != S_BLOCKED
        && p_state[dst] != S_SLEEP) return -1;
    if (p_msgcnt[dst] >= MSG_Q) return -2;
    w = (p_msgnext[dst] + p_msgcnt[dst]) % MSG_Q;
    p_msgs[dst][w] = msg; p_msgcnt[dst]++;
    if (p_state[dst] == S_BLOCKED && p_wait4[dst] == -2)
        p_state[dst] = S_READY;
    yield(); return 0;
}

int recv_msg(int *msg)
{
    while (p_msgcnt[cur_pid] == 0)
    { p_wait4[cur_pid] = -2; p_state[cur_pid] = S_BLOCKED; yield(); }
    *msg = p_msgs[cur_pid][p_msgnext[cur_pid]];
    p_msgnext[cur_pid] = (p_msgnext[cur_pid] + 1) % MSG_Q;
    p_msgcnt[cur_pid]--; return 0;
}

/* ================================================================
 *  FILE DESCRIPTOR TABLE
 * ================================================================ */
#define MAX_FD      32
#define FD_FREE     0
#define FD_FILE     1
#define FD_PIPE_RD  2
#define FD_PIPE_WR  3
#define FD_TERM     4
#define FD_ROM      5
#define FD_KBD      6

int fd_type[MAX_FD];
int fd_ino[MAX_FD];        /* for FD_FILE: inode number */
long fd_offset[MAX_FD];    /* for FD_FILE: current read/write offset */
int fd_pipe[MAX_FD];       /* for FD_PIPE: pipe index */
int fd_refs[MAX_FD];       /* reference count */

void fd_init(void)
{
    int i;
    for (i = 0; i < MAX_FD; i++) fd_type[i] = FD_FREE;
    /* stdin (ROM input) */
    fd_type[0] = FD_ROM; fd_refs[0] = 1;
    /* stdout, stderr (terminal) */
    fd_type[1] = FD_TERM; fd_refs[1] = 1;
    fd_type[2] = FD_TERM; fd_refs[2] = 1;
    /* keyboard input */
    fd_type[3] = FD_KBD; fd_refs[3] = 1;
}

int fd_alloc(void)
{
    int i;
    for (i = 3; i < MAX_FD; i++)
        if (fd_type[i] == FD_FREE) { fd_type[i] = -1; fd_refs[i] = 1; return i; }
    return -1;
}

void fd_close(int fd)
{
    if (fd < 0 || fd >= MAX_FD) return;
    fd_refs[fd]--;
    if (fd_refs[fd] <= 0) fd_type[fd] = FD_FREE;
}

int sys_read(int fd, int *buf, int words);
int sys_write(int fd, int *buf, int words);

/* ================================================================
 *  PIPES
 * ================================================================ */
#define MAX_PIPES   8
#define PIPE_BUF    256

int pipe_buf[MAX_PIPES][PIPE_BUF];
int pipe_head[MAX_PIPES];
int pipe_tail[MAX_PIPES];
int pipe_count[MAX_PIPES];
int pipe_used[MAX_PIPES];
int pipe_readers[MAX_PIPES];      /* FD count of read ends */
int pipe_writers[MAX_PIPES];      /* FD count of write ends */
int pipe_wait_rd[MAX_PIPES];      /* pid blocked on read, or -1 */
int pipe_wait_wr[MAX_PIPES];      /* pid blocked on write, or -1 */

void pipe_init(void)
{
    int i;
    for (i = 0; i < MAX_PIPES; i++) {
        pipe_used[i] = 0;
        pipe_wait_rd[i] = -1;
        pipe_wait_wr[i] = -1;
    }
}

/* Wake a process if it's blocked (on anything). */
static void wake_if_blocked(int pid)
{
    if (pid >= 0 && p_state[pid] == S_BLOCKED)
        p_state[pid] = S_READY;
}

int sys_pipe(int fds[2])
{
    int i, rfd, wfd;
    for (i = 0; i < MAX_PIPES; i++)
        if (!pipe_used[i]) break;
    if (i == MAX_PIPES) return -1;

    rfd = fd_alloc();
    if (rfd < 0) return -1;
    wfd = fd_alloc();
    if (wfd < 0) { fd_close(rfd); return -1; }

    pipe_used[i] = 1;
    pipe_head[i] = 0; pipe_tail[i] = 0; pipe_count[i] = 0;
    pipe_readers[i] = 1; pipe_writers[i] = 1;
    pipe_wait_rd[i] = -1; pipe_wait_wr[i] = -1;

    fd_type[rfd] = FD_PIPE_RD; fd_pipe[rfd] = i;
    fd_type[wfd] = FD_PIPE_WR; fd_pipe[wfd] = i;

    fds[0] = rfd; fds[1] = wfd;
    return 0;
}

/* Blocking write: blocks when buffer is full (if readers still exist).
 * Returns -1 on broken pipe (no readers), or the number of words written. */
int pipe_write(int pi, int *buf, int words)
{
    int written, pos;
    written = 0;
    while (written < words) {
        /* block while buffer full and readers still exist */
        while (pipe_count[pi] >= PIPE_BUF) {
            if (pipe_writers[pi] <= 0 || pipe_readers[pi] <= 0)
                return written > 0 ? written : -1;
            pipe_wait_wr[pi] = cur_pid;
            p_state[cur_pid] = S_BLOCKED;
            yield();
            /* after wakeup: check if readers went away */
            if (pipe_readers[pi] <= 0)
                return written > 0 ? written : -1;
        }
        pos = (pipe_head[pi] + pipe_count[pi]) % PIPE_BUF;
        pipe_buf[pi][pos] = buf[written++];
        pipe_count[pi]++;
        /* wake a blocked reader */
        if (pipe_wait_rd[pi] >= 0) {
            wake_if_blocked(pipe_wait_rd[pi]);
            pipe_wait_rd[pi] = -1;
        }
    }
    return written;
}

/* Blocking read: blocks when buffer is empty (if writers still exist).
 * Returns 0 on EOF (no writers and buffer empty), or words read. */
int pipe_read(int pi, int *buf, int words)
{
    int nread = 0;
    while (nread < words) {
        /* block while empty and writers still exist */
        while (pipe_count[pi] == 0) {
            if (pipe_readers[pi] <= 0 || pipe_writers[pi] <= 0)
                return nread;   /* EOF */
            pipe_wait_rd[pi] = cur_pid;
            p_state[cur_pid] = S_BLOCKED;
            yield();
            /* after wakeup: check if writers went away */
            if (pipe_writers[pi] <= 0 && pipe_count[pi] == 0)
                return nread;   /* EOF */
        }
        buf[nread++] = pipe_buf[pi][pipe_tail[pi]];
        pipe_tail[pi] = (pipe_tail[pi] + 1) % PIPE_BUF;
        pipe_count[pi]--;
        /* wake a blocked writer */
        if (pipe_wait_wr[pi] >= 0) {
            wake_if_blocked(pipe_wait_wr[pi]);
            pipe_wait_wr[pi] = -1;
        }
    }
    return nread;
}

void pipe_close_read(int pi)
{
    pipe_readers[pi]--;
    /* wake a writer blocked on full buffer (it will see readers=0 and fail) */
    if (pipe_wait_wr[pi] >= 0) {
        wake_if_blocked(pipe_wait_wr[pi]);
        pipe_wait_wr[pi] = -1;
    }
    /* wake a reader blocked on empty (it will see writers=0 and get EOF) */
    if (pipe_wait_rd[pi] >= 0 && pipe_writers[pi] <= 0) {
        wake_if_blocked(pipe_wait_rd[pi]);
        pipe_wait_rd[pi] = -1;
    }
    if (pipe_readers[pi] <= 0 && pipe_writers[pi] <= 0)
        pipe_used[pi] = 0;
}

void pipe_close_write(int pi)
{
    pipe_writers[pi]--;
    /* wake a reader blocked on empty (it will see writers=0 and get EOF) */
    if (pipe_wait_rd[pi] >= 0) {
        wake_if_blocked(pipe_wait_rd[pi]);
        pipe_wait_rd[pi] = -1;
    }
    /* wake a writer blocked on full (it will see readers=0 and fail) */
    if (pipe_wait_wr[pi] >= 0 && pipe_readers[pi] <= 0) {
        wake_if_blocked(pipe_wait_wr[pi]);
        pipe_wait_wr[pi] = -1;
    }
    if (pipe_readers[pi] <= 0 && pipe_writers[pi] <= 0)
        pipe_used[pi] = 0;
}

/* ================================================================
 *  FILE SYSTEM  (pages 1-7 for data, inodes in page 0 arrays)
 * ================================================================ */
#define FS_INODES   64
#define FS_NAMELEN  32
#define FS_DIRECT   8
#define FS_BLKSZ    256

/*
 * Multi-page storage:
 *   Block pointers are 32-bit (long): (page << 16) | addr.
 *   Pages 1-7 hold file data, page 0 holds inode metadata.
 *   A free-block bitmap tracks 1792 blocks across pages 1-7.
 *
 * CRITICAL: SP and PAGE share the RAM address bus.  Never touch the
 * stack (push/pop/call/ret) while page != 0.  Cross-page access uses
 * single ldr/stor instructions with page save/restore in registers.
 * All parameter loads from the stack MUST happen before the page switch.
 */

/* ---- cross-page access primitives ----
 * Use fixed address 0x7FF4 as a one-word transfer register, just
 * after the context-switch bridge (0x7FF0-0x7FF3) and below the
 * heap (0x8000). */
#define PAGE_TMP (*(int *)0x7FF4)

static int page_rd(int page, int addr)
{
    /* params: page at r15+3, addr at r15+4.
     * ALL parameter loads happen BEFORE the page switch. */
    if (page == 0) return *((int *)addr);
    __asm__("\n\tmov r2,page"         /* save current page */
            "\n\tldo r0,*r15,#3"      /* r0 = target page */
            "\n\tldo r1,*r15,#4"      /* r1 = addr (before page switch!) */
            "\n\tmov page,r0"         /* switch page */
            "\n\tldr r0,*r1"          /* r0 = mem[addr] */
            "\n\tmov page,r2"         /* restore page */
            "\n\tstod *0x7FF4,r0");   /* store result at PAGE_TMP */
    return PAGE_TMP;
}

static void page_wr(int page, int addr, int value)
{
    /* params: page at r15+3, addr at r15+4, value at r15+5.
     * ALL parameter loads happen BEFORE the page switch. */
    if (page == 0) { *((int *)addr) = value; return; }
    PAGE_TMP = value;
    __asm__("\n\tmov r2,page"         /* save current page */
            "\n\tldo r0,*r15,#3"      /* r0 = target page */
            "\n\tldo r1,*r15,#4"      /* r1 = addr (before page switch!) */
            "\n\tldd r3,*0x7FF4"      /* r3 = value (before page switch!) */
            "\n\tmov page,r0"         /* switch page */
            "\n\tstor *r1,r3"         /* mem[addr] = value */
            "\n\tmov page,r2");       /* restore page */
}

/* ---- block pointer encoding ----
 * 32-bit block pointer: (page << 16) | addr
 * page 0 = unallocated/in-page-0, pages 1-7 = data pages */
#define BLK_PAGE(bp)  ((int)((bp) >> 16))
#define BLK_ADDR(bp)  ((int)((bp) & 0xFFFF))
#define BLK_PTR(p,a)  (((long)(p) << 16) | (long)((a) & 0xFFFF))

/* ---- inode (in page 0 RAM) ---- */
int  fs_type[FS_INODES];     /* 0=free 1=file 2=dir */
long fs_size[FS_INODES];
long fs_blocks[FS_INODES][FS_DIRECT];  /* 32-bit block pointers */
char fs_name[FS_INODES][FS_NAMELEN];

/* ---- free-block bitmap (pages 1-7, 256-word blocks) ----
 * 7 pages * (65536 / 256) = 7 * 256 = 1792 blocks.
 * 1792 bits = 112 words. */
#define FS_NBLOCKS  1792
#define FS_BMP_WORDS 112
static int fs_blkmap[FS_BMP_WORDS];

/* block index -> (page, addr) */
#define FS_BLK_PAGE(idx)  (1 + (idx) / 256)
#define FS_BLK_ADDR(idx)  (((idx) % 256) * FS_BLKSZ)

static int fs_blkmap_get(int idx)
{
    return (fs_blkmap[idx / 16] >> (idx % 16)) & 1;
}

static void fs_blkmap_set(int idx, int val)
{
    int w, b;
    w = idx / 16; b = idx % 16;
    if (val)
        fs_blkmap[w] |= (1 << b);
    else
        fs_blkmap[w] &= ~(1 << b);
}

static void fs_blkmap_init(void)
{
    int i;
    for (i = 0; i < FS_BMP_WORDS; i++) fs_blkmap[i] = 0;
}

static long fs_alloc_block(void)
{
    int i;
    for (i = 0; i < FS_NBLOCKS; i++) {
        if (fs_blkmap_get(i) == 0) {
            fs_blkmap_set(i, 1);
            return BLK_PTR(FS_BLK_PAGE(i), FS_BLK_ADDR(i));
        }
    }
    return 0;   /* no free blocks */
}

static void fs_free_block(long blk_ptr)
{
    int page, addr, idx;
    page = BLK_PAGE(blk_ptr);
    addr = BLK_ADDR(blk_ptr);
    if (page < 1 || page > 7) return;
    idx = (page - 1) * 256 + (addr / FS_BLKSZ);
    if (idx >= 0 && idx < FS_NBLOCKS)
        fs_blkmap_set(idx, 0);
}

/* ---- FS init ---- */
void fs_init_simple(void)
{
    int i, j;
    fs_blkmap_init();
    for (i = 0; i < FS_INODES; i++) {
        fs_type[i] = 0; fs_size[i] = 0;
        fs_name[i][0] = 0;
        for (j = 0; j < FS_DIRECT; j++) fs_blocks[i][j] = 0;
    }
    /* root dir */
    fs_type[0] = 2;
    fs_name[0][0] = '/'; fs_name[0][1] = 0;
    fs_size[0] = 0;
}

int fs_ialloc(int type, char *name)
{
    int i, j;
    for (i = 1; i < FS_INODES; i++)
        if (fs_type[i] == 0) {
            fs_type[i] = type; fs_size[i] = 0;
            for (j = 0; j < FS_DIRECT; j++) fs_blocks[i][j] = 0;
            for (j = 0; j < FS_NAMELEN-1 && name[j]; j++)
                fs_name[i][j] = name[j];
            fs_name[i][j] = 0;
            return i;
        }
    return -1;
}

int fs_grow(int ino, int newsize)
{
    int cur_blocks, need_blocks, i;
    long blk;
    if (ino < 0 || ino >= FS_INODES || fs_type[ino] == 0) return -1;
    cur_blocks = (int)((fs_size[ino] + FS_BLKSZ - 1) / FS_BLKSZ);
    need_blocks = (newsize + FS_BLKSZ - 1) / FS_BLKSZ;
    for (i = cur_blocks; i < need_blocks && i < FS_DIRECT; i++) {
        if (fs_blocks[ino][i] == 0) {
            blk = fs_alloc_block();
            if (blk == 0) return -1;
            fs_blocks[ino][i] = blk;
        }
    }
    if (newsize > fs_size[ino]) fs_size[ino] = newsize;
    return 0;
}

int fs_writei(int ino, int offset, int *buf, int words)
{
    int i, blk_idx, blk_off, page, addr;
    long blk;
    if (fs_grow(ino, offset + words) < 0) return -1;
    for (i = 0; i < words; i++) {
        blk_idx = (offset + i) / FS_BLKSZ;
        blk_off = (offset + i) % FS_BLKSZ;
        if (blk_idx >= FS_DIRECT) break;
        blk = fs_blocks[ino][blk_idx];
        if (blk == 0) break;
        page = BLK_PAGE(blk);
        addr = BLK_ADDR(blk) + blk_off;
        if (page == 0)
            *((int *)addr) = buf[i];
        else
            page_wr(page, addr, buf[i]);
    }
    return i;
}

/* Write string to inode */
int fs_write_str(int ino, char *s)
{
    int i, blk_idx, blk_off, page, addr;
    long blk;
    for (i = 0; s[i]; i++) {
        if (i >= fs_size[ino]) {
            blk_idx = i / FS_BLKSZ;
            if (blk_idx >= FS_DIRECT) return i;
            if (fs_blocks[ino][blk_idx] == 0) {
                long nb = fs_alloc_block();
                if (nb == 0) return i;
                fs_blocks[ino][blk_idx] = nb;
            }
            fs_size[ino] = i + 1;
        }
        blk_idx = i / FS_BLKSZ;
        blk = fs_blocks[ino][blk_idx];
        blk_off = i % FS_BLKSZ;
        page = BLK_PAGE(blk);
        addr = BLK_ADDR(blk) + blk_off;
        if (page == 0)
            *((int *)addr) = s[i];
        else
            page_wr(page, addr, s[i]);
    }
    return i;
}

int fs_readi(int ino, int offset, int *buf, int words)
{
    int i, blk_idx, blk_off, page, addr;
    long blk;
    if (ino < 0 || ino >= FS_INODES || fs_type[ino] == 0) return -1;
    for (i = 0; i < words && offset + i < (int)fs_size[ino]; i++) {
        blk_idx = (offset + i) / FS_BLKSZ;
        if (blk_idx >= FS_DIRECT) break;
        blk = fs_blocks[ino][blk_idx];
        if (blk == 0) break;
        blk_off = (offset + i) % FS_BLKSZ;
        page = BLK_PAGE(blk);
        addr = BLK_ADDR(blk) + blk_off;
        if (page == 0)
            buf[i] = *((int *)addr);
        else
            buf[i] = page_rd(page, addr);
    }
    return i;
}

/* ---- directory operations ---- */
/* A directory inode contains entries: [inode_number, name_length, name_chars...]
 * Entry format: word 0 = inode number (or -1 = free), word 1 = name length,
 * then the name chars follow.  Each entry is variable-length. */

int fs_dir_lookup(int dir_ino, char *name)
{
    int pos, ino, nlen, j, match, slen;
    slen = strlen(name);
    pos = 0;
    while (pos < fs_size[dir_ino]) {
        fs_readi(dir_ino, pos, &ino, 1); pos++;
        fs_readi(dir_ino, pos, &nlen, 1); pos++;
        if (ino != -1 && nlen == slen) {
            match = 1;
            for (j = 0; j < nlen; j++) {
                int ch; fs_readi(dir_ino, pos + j, &ch, 1);
                if (ch != name[j]) { match = 0; break; }
            }
            if (match) return ino;
        }
        pos += nlen;
    }
    return -1;
}

int fs_dir_add(int dir_ino, char *name, int ino)
{
    int pos, entry[34], nlen, i;
    nlen = strlen(name);
    if (nlen > 30) nlen = 30;
    pos = fs_size[dir_ino];
    entry[0] = ino;
    entry[1] = nlen;
    for (i = 0; i < nlen; i++) entry[2 + i] = name[i];
    fs_writei(dir_ino, pos, entry, 2 + nlen);
    /* pad to keep size tracking simple — assume writei handles it */
    return 0;
}

void fs_dir_list(int dir_ino)
{
    int pos, ino, nlen, j, ch;
    pos = 0;
    while (pos < fs_size[dir_ino]) {
        fs_readi(dir_ino, pos, &ino, 1); pos++;
        fs_readi(dir_ino, pos, &nlen, 1); pos++;
        if (ino != -1) {
            for (j = 0; j < nlen; j++) {
                fs_readi(dir_ino, pos + j, &ch, 1);
                putchar(ch);
            }
            putchar(' ');
        }
        pos += nlen;
    }
}

/* ---- file deletion ---- */

int fs_resolve(char *path);

/* Parse an absolute path into parent inode + basename.
 * e.g. "/foo/bar" → *parent = fs_resolve("/foo"), name = "bar".
 * e.g. "/bar"     → *parent = 0 (root), name = "bar".
 * Returns 0 on success, -1 on error. */
static int fs_parse_parent(char *path, int *parent, char *name)
{
    int i, slash_pos;
    if (path[0] != '/') return -1;
    slash_pos = 0;
    for (i = 0; path[i]; i++)
        if (path[i] == '/') slash_pos = i;
    if (slash_pos == 0) {
        *parent = 0;
        for (i = 0; i < 31 && path[i+1]; i++)
            name[i] = path[i+1];
        name[i] = 0;
    } else {
        char dir[32];
        for (i = 0; i < slash_pos && i < 31; i++)
            dir[i] = path[i];
        dir[slash_pos < 31 ? slash_pos : 31] = 0;
        *parent = fs_resolve(dir);
        if (*parent < 0) return -1;
        for (i = 0; i < 31 && path[slash_pos+1+i]; i++)
            name[i] = path[slash_pos+1+i];
        name[i] = 0;
    }
    return 0;
}

/* Remove a directory entry by setting its inode to -1. */
int fs_dir_remove(int dir_ino, char *name)
{
    int pos, ino, nlen, j, slen, match, minus_one;
    slen = strlen(name);
    pos = 0;
    while (pos < fs_size[dir_ino]) {
        fs_readi(dir_ino, pos, &ino, 1); pos++;
        fs_readi(dir_ino, pos, &nlen, 1); pos++;
        if (ino != -1 && nlen == slen) {
            match = 1;
            for (j = 0; j < nlen; j++) {
                int ch; fs_readi(dir_ino, pos + j, &ch, 1);
                if (ch != name[j]) { match = 0; break; }
            }
            if (match) {
                minus_one = -1;
                fs_writei(dir_ino, pos - 2, &minus_one, 1);
                return 0;
            }
        }
        pos += nlen;
    }
    return -1;
}

/* Delete a file by path. Frees all data blocks and the inode. */
int fs_unlink(char *path)
{
    int parent, ino, i;
    char name[32];
    long blk;

    if (fs_parse_parent(path, &parent, name) < 0) return -1;
    if (parent == 0 && name[0] == 0) return -1;

    ino = fs_dir_lookup(parent, name);
    if (ino < 0 || fs_type[ino] != 1) return -1;

    /* free data blocks */
    for (i = 0; i < FS_DIRECT; i++) {
        blk = fs_blocks[ino][i];
        if (blk != 0) {
            fs_free_block(blk);
            fs_blocks[ino][i] = 0;
        }
    }

    /* remove directory entry and free inode */
    fs_dir_remove(parent, name);
    fs_type[ino] = 0;
    fs_size[ino] = 0;
    fs_name[ino][0] = 0;

    return 0;
}

/* Move/rename a file or directory. */
int sys_move(char *src, char *dst)
{
    int src_parent, dst_parent, src_ino, existing, i;
    char src_name[32], dst_name[32];
    long blk;

    if (fs_parse_parent(src, &src_parent, src_name) < 0) return -1;
    src_ino = fs_dir_lookup(src_parent, src_name);
    if (src_ino < 0) return -1;
    if (src_ino == 0) return -1;   /* can't move root */

    if (fs_parse_parent(dst, &dst_parent, dst_name) < 0) return -1;
    if (dst_parent < 0 || fs_type[dst_parent] != 2) return -1;
    if (dst_name[0] == 0) return -1;

    /* if destination exists, remove it first */
    existing = fs_dir_lookup(dst_parent, dst_name);
    if (existing >= 0) {
        if (fs_type[existing] == 2) return -1;  /* can't overwrite a dir */
        for (i = 0; i < FS_DIRECT; i++) {
            blk = fs_blocks[existing][i];
            if (blk != 0) { fs_free_block(blk); fs_blocks[existing][i] = 0; }
        }
        fs_dir_remove(dst_parent, dst_name);
        fs_type[existing] = 0; fs_size[existing] = 0;
        fs_name[existing][0] = 0;
    }

    /* move entry from source dir to destination dir */
    fs_dir_remove(src_parent, src_name);
    fs_dir_add(dst_parent, dst_name, src_ino);

    /* update inode name */
    for (i = 0; i < FS_NAMELEN - 1 && dst_name[i]; i++)
        fs_name[src_ino][i] = dst_name[i];
    fs_name[src_ino][i] = 0;

    return 0;
}

/* Copy a file. */
int sys_copy(char *src, char *dst)
{
    int src_ino, dst_parent, dst_ino, existing, i, offset, n;
    char dst_name[32];
    int buf[64];
    long blk;

    src_ino = fs_resolve(src);
    if (src_ino < 0 || fs_type[src_ino] != 1) return -1;

    if (fs_parse_parent(dst, &dst_parent, dst_name) < 0) return -1;
    if (dst_parent < 0 || fs_type[dst_parent] != 2) return -1;
    if (dst_name[0] == 0) return -1;

    /* if destination exists, remove it */
    existing = fs_dir_lookup(dst_parent, dst_name);
    if (existing >= 0) {
        if (fs_type[existing] != 1) return -1;
        for (i = 0; i < FS_DIRECT; i++) {
            blk = fs_blocks[existing][i];
            if (blk != 0) { fs_free_block(blk); fs_blocks[existing][i] = 0; }
        }
        fs_dir_remove(dst_parent, dst_name);
        fs_type[existing] = 0; fs_size[existing] = 0;
        fs_name[existing][0] = 0;
    }

    /* create destination inode */
    dst_ino = fs_ialloc(1, dst_name);
    if (dst_ino < 0) return -1;
    fs_dir_add(dst_parent, dst_name, dst_ino);

    /* copy data in chunks */
    offset = 0;
    while (offset < (int)fs_size[src_ino]) {
        n = fs_readi(src_ino, offset, buf, 64);
        if (n <= 0) break;
        fs_writei(dst_ino, offset, buf, n);
        offset += n;
    }

    return 0;
}

/* ---- path resolution ---- */
int fs_resolve(char *path)
{
    int ino, start, i, j;
    char name[32];

    if (path[0] != '/') return -1;   /* only absolute paths */
    ino = 0;  /* root */

    start = 1;
    while (path[start]) {
        /* skip slashes */
        while (path[start] == '/') start++;
        if (!path[start]) break;

        /* extract component name */
        for (i = start, j = 0; path[i] && path[i] != '/' && j < 31; i++, j++)
            name[j] = path[i];
        name[j] = 0;
        start = i;

        ino = fs_dir_lookup(ino, name);
        if (ino < 0) return -1;
    }
    return ino;
}

/* ================================================================
 *  SYSTEM CALLS (fd-based)
 * ================================================================ */
int sys_write(int fd, int *buf, int words)
{
    int i, ch;
    if (fd < 0 || fd >= MAX_FD) return -1;
    if (fd_type[fd] == FD_TERM) {
        for (i = 0; i < words; i++) putchar(buf[i]);
        return words;
    }
    if (fd_type[fd] == FD_FILE) {
        i = fs_writei(fd_ino[fd], fd_offset[fd], buf, words);
        fd_offset[fd] += i;
        return i;
    }
    if (fd_type[fd] == FD_PIPE_WR) {
        return pipe_write(fd_pipe[fd], buf, words);
    }
    return -1;
}

int sys_read(int fd, int *buf, int words)
{
    int i, c, buf_int[1];
    if (fd < 0 || fd >= MAX_FD) return -1;
    if (fd_type[fd] == FD_ROM) {
        for (i = 0; i < words; i++) {
            c = getchar();
            if (c == -1 || c == 0) break;
            buf[i] = c;
        }
        return i;
    }
    if (fd_type[fd] == FD_KBD) {
        for (i = 0; i < words; i++) {
            while (!kbhit()) yield();   /* block until key available */
            buf[i] = getkey();
        }
        return i;
    }
    if (fd_type[fd] == FD_TERM) return 0;  /* can't read from terminal */
    if (fd_type[fd] == FD_FILE) {
        i = fs_readi(fd_ino[fd], fd_offset[fd], buf, words);
        fd_offset[fd] += i;
        return i;
    }
    if (fd_type[fd] == FD_PIPE_RD) {
        return pipe_read(fd_pipe[fd], buf, words);
    }
    return -1;
}

int sys_open(char *path)
{
    int ino, fd;
    ino = fs_resolve(path);
    if (ino < 0) return -1;
    if (fs_type[ino] != 1) return -1;  /* only regular files */
    fd = fd_alloc();
    if (fd < 0) return -1;
    fd_type[fd] = FD_FILE; fd_ino[fd] = ino; fd_offset[fd] = 0;
    return fd;
}

int sys_close(int fd)
{
    if (fd_type[fd] == FD_PIPE_RD) pipe_close_read(fd_pipe[fd]);
    if (fd_type[fd] == FD_PIPE_WR) pipe_close_write(fd_pipe[fd]);
    fd_close(fd);
    return 0;
}

/* dup2: make newfd refer to the same object as oldfd.
 * Closes newfd first if it was open.  Returns newfd on success, -1 on error. */
int sys_dup2(int oldfd, int newfd)
{
    int t;
    if (oldfd < 0 || oldfd >= MAX_FD || fd_type[oldfd] == FD_FREE) return -1;
    if (newfd < 0 || newfd >= MAX_FD) return -1;
    if (oldfd == newfd) return newfd;
    /* close newfd if open */
    sys_close(newfd);
    /* copy the FD entry */
    t = fd_type[oldfd];
    fd_type[newfd]  = t;
    fd_ino[newfd]   = fd_ino[oldfd];
    fd_offset[newfd]= fd_offset[oldfd];
    fd_pipe[newfd]  = fd_pipe[oldfd];
    fd_refs[newfd]  = 1;
    /* bump pipe refcounts so the pipe stays alive */
    if (t == FD_PIPE_RD) pipe_readers[fd_pipe[oldfd]]++;
    if (t == FD_PIPE_WR) pipe_writers[fd_pipe[oldfd]]++;
    return newfd;
}

int sys_unlink(char *path)
{
    return fs_unlink(path);
}

/* ================================================================
 *  SHELL
 * ================================================================ */
int scmp(char *a, char *b) { while (*a && *a == *b) { a++; b++; } return *a - *b; }

/* globals for passing arguments to pipe-segment tasks.
 * Per-pid arrays because spawns happen before children run,
 * so the last spawn would otherwise overwrite earlier segments.
 * Sizes are minimal to keep total memory low (FD table is global,
 * so memory is shared with the FS). */
char sh_seg_cmd[MAX_PROCS][16];
char sh_seg_arg[MAX_PROCS][32];
int  sh_seg_stdin[MAX_PROCS];    /* fd to dup2 to 0 */
int  sh_seg_stdout[MAX_PROCS];   /* fd to dup2 to 1 */
int  sh_seg_ino[MAX_PROCS];      /* pre-resolved inode for file paths */

/* forward declarations */
void sh_pipe_segment_task(void);
int  sh_exec_cmd(char *cmd, char *arg);

void sh_readln(char *buf, int max)
{
    int c, i, tmp[1];
    i = 0;
    while (i < max - 1) {
        /* block on keyboard input */
        if (sys_read(3, tmp, 1) <= 0) break;
        c = tmp[0];
        if (c == '\n') break;
        if (c == '\b' || c == 0x7f) {  /* backspace or delete */
            if (i > 0) {
                i--;
                putchar('\b');          /* move cursor back */
                putchar(' ');           /* erase character */
                putchar('\b');          /* move cursor back again */
            }
            continue;
        }
        buf[i++] = c;
        putchar(c);  /* echo */
    }
    buf[i] = 0;
}

/* cat a file by inode (already resolved) — avoids calling fs_resolve
 * at problematic call depths inside pipe segments. */
static void sh_cat_inode(int ino)
{
    int fd, buf[32], n, i;
    fd = fd_alloc();
    if (fd < 0) { puts("open failed"); return; }
    fd_type[fd] = FD_FILE; fd_ino[fd] = ino; fd_offset[fd] = 0;
    while ((n = sys_read(fd, buf, 32)) > 0)
        sys_write(1, buf, n);
    sys_write(1, "\n", 1);
    sys_close(fd);
}

void sh_cat(char *path)
{
    int fd, buf[32], n, i;
    if (path[0] == 0) {
        fd = 0;   /* no path → read stdin (useful for pipes) */
    } else {
        fd = sys_open(path);
        if (fd < 0) { puts("not found"); return; }
    }
    while ((n = sys_read(fd, buf, 32)) > 0)
        sys_write(1, buf, n);      /* write to stdout (may be a pipe) */
    sys_write(1, "\n", 1);
    if (fd != 0) sys_close(fd);
}

void sh_ps(void)
{
    int i; char *sts = "?RBZS";
    puts("PID PPID PR ST NAME");
    for (i = 0; i < MAX_PROCS; i++) if (p_state[i]) {
        printf(" %d   %d   %d  %c  ",
               i, p_ppid[i], p_priority[i], sts[p_state[i]]);
        { int j; for (j = 0; j < 4 && p_name[i][j]; j++) putchar(p_name[i][j]); }
        putchar('\n');
    }
}

void prio_demo_task(void);

void sh_run_pipeline(char *line);
void editor_task(char *path);
void shell_task(void)
{
    char line[64], cmd[32], arg[64];
    int i, c;

    puts("RISKY OS v3 shell.  Type 'help'.");

    while (1) {
        putchar('$'); putchar(' ');
        sh_readln(line, 64);
        if (line[0] == 0) {
            putchar('\n');
            continue;
        }
        putchar('\n');

        /* check for pipe */
        {
            char *p = line;
            while (*p && *p != '|') p++;
            if (*p == '|') {
                sh_run_pipeline(line);
                yield();
                continue;
            }
        }

        /* parse cmd and arg */
        for (i = 0; line[i] == ' ' || line[i] == '\t'; i++);
        { int j; for (j = 0; line[i] && line[i] != ' ' && line[i] != '\t' && j < 31; i++, j++)
            cmd[j] = line[i]; cmd[j] = 0; }
        while (line[i] == ' ' || line[i] == '\t') i++;
        { int j; for (j = 0; line[i] && j < 63; i++, j++)
            arg[j] = line[i]; arg[j] = 0; }

        if (cmd[0] == 0) { yield(); continue; }

        sh_exec_cmd(cmd, arg);
        yield();
    }
}

/* Execute a single builtin command (no pipes). */
/* Execute the "write" command — has a large buffer, so it lives in its
 * own function to keep sh_exec_cmd's stack frame small for pipe segments. */
static int sh_write_cmd(char *arg)
{
    char *path, *text;
    int fd, i, buf[128];
    path = arg;
    for (text = arg; *text && *text != ' '; text++);
    if (*text) { *text = 0; text++; }
    fd = sys_open(path);
    if (fd >= 0) {
        for (i = 0; text[i]; i++) buf[i] = text[i];
        sys_write(fd, buf, i);
        sys_close(fd);
        printf("wrote %d bytes to %s\n", i, path);
    } else {
        puts("open failed");
    }
    return 0;
}

/* Execute a single builtin command (no pipes).
 * Keep stack usage minimal — large buffers are in helper functions. */
int sh_exec_cmd(char *cmd, char *arg)
{
    int fd, fds[2], pid;

    if (scmp(cmd, "help") == 0) {
        puts("help ps ls cat mkfile mkdir rm mv cp edit pipe write echo nice sleep prio exit");
    } else if (scmp(cmd, "ps") == 0) {
        sh_ps();
    } else if (scmp(cmd, "ls") == 0) {
        if (arg[0]) { fs_dir_list(fs_resolve(arg)); putchar('\n'); }
        else { fs_dir_list(0); putchar('\n'); }
    } else if (scmp(cmd, "cat") == 0) {
        if (arg[0]) {
            int ino;
            /* use pre-resolved inode if available (set by parent
             * before spawning pipe children) */
            ino = sh_seg_ino[cur_pid];
            if (ino > 0 && fs_type[ino] == 1) {
                sh_cat_inode(ino);
            } else {
                ino = fs_resolve(arg);
                if (ino < 0 || fs_type[ino] != 1)
                    puts("not found");
                else
                    sh_cat_inode(ino);
            }
        } else {
            sh_cat(arg);   /* no path — read stdin */
        }
    } else if (scmp(cmd, "mkfile") == 0 || scmp(cmd, "mkdir") == 0) {
        int parent, type, ino;
        char *p, *name, *slash;
        p = arg; name = arg; slash = arg;
        while (*p) { if (*p == '/') slash = p; p++; }
        if (*name == '/' && slash == name) {
            parent = 0; name++;
        } else if (*name == '/') {
            *slash = 0; parent = fs_resolve(name); *slash = '/'; name = slash + 1;
        } else {
            parent = 0;
        }
        type = (cmd[0] == 'm' && cmd[2] == 'f') ? 1 : 2;
        ino = fs_ialloc(type, name);
        if (ino >= 0 && parent >= 0 && fs_type[parent] == 2) {
            fs_dir_add(parent, name, ino);
            printf("created %s %s (ino %d)\n", type==2?"dir":"file", name, ino);
        } else {
            puts("create failed");
        }
    } else if (scmp(cmd, "write") == 0) {
        sh_write_cmd(arg);
    } else if (scmp(cmd, "pipe") == 0) {
        if (sys_pipe(fds) == 0)
            printf("pipe: rd=%d wr=%d\n", fds[0], fds[1]);
        else
            puts("pipe failed");
    } else if (scmp(cmd, "echo") == 0) {
        int elen;
        for (elen = 0; arg[elen]; elen++);
        sys_write(1, arg, elen);
        sys_write(1, "\n", 1);
    } else if (scmp(cmd, "spawn") == 0) {
        pid = proc_spawn(shell_task, cur_pid, "sh2");
        printf("spawned %d\n", pid);
    } else if (scmp(cmd, "nice") == 0) {
        int tgt, prio; char *ap;
        tgt = 0; prio = 0;
        ap = arg;
        while (*ap >= '0' && *ap <= '9') { tgt = tgt * 10 + (*ap - '0'); ap++; }
        while (*ap == ' ') ap++;
        while (*ap >= '0' && *ap <= '9') { prio = prio * 10 + (*ap - '0'); ap++; }
        if (tgt > 0 && tgt < MAX_PROCS && p_state[tgt]) {
            p_priority[tgt] = prio;
            printf("pid %d priority = %d\n", tgt, prio);
        } else {
            puts("invalid pid");
        }
    } else if (scmp(cmd, "sleep") == 0) {
        int ticks; char *ap2;
        ticks = 0;
        ap2 = arg;
        while (*ap2 >= '0' && *ap2 <= '9') { ticks = ticks * 10 + (*ap2 - '0'); ap2++; }
        if (ticks > 0) {
            printf("sleeping %d ticks...\n", ticks);
            proc_sleep(ticks);
            printf("awake at tick %d\n", sys_tick);
        }
    } else if (scmp(cmd, "prio") == 0) {
        pid = proc_spawn(prio_demo_task, cur_pid, "prio");
        printf("spawned prio demo pid %d\n", pid);
    } else if (scmp(cmd, "rm") == 0) {
        if (arg[0] == 0) { puts("rm: missing path"); }
        else if (sys_unlink(arg) < 0) { puts("rm: failed"); }
    } else if (scmp(cmd, "mv") == 0 || scmp(cmd, "cp") == 0) {
        char src[32], dst[32];
        int si, di, is_mv;
        is_mv = (cmd[0] == 'm');
        /* parse first arg */
        for (si = 0; arg[si] && arg[si] != ' ' && arg[si] != '\t' && si < 31; si++)
            src[si] = arg[si];
        src[si] = 0;
        while (arg[si] == ' ' || arg[si] == '\t') si++;
        /* parse second arg */
        for (di = 0; arg[si+di] && arg[si+di] != ' ' && arg[si+di] != '\t' && di < 31; di++)
            dst[di] = arg[si+di];
        dst[di] = 0;
        if (src[0] == 0 || dst[0] == 0)
            printf("usage: %s <src> <dst>\n", cmd);
        else if (is_mv && sys_move(src, dst) < 0)
            puts("mv: failed");
        else if (!is_mv && sys_copy(src, dst) < 0)
            puts("cp: failed");
    } else if (scmp(cmd, "edit") == 0) {
        editor_task(arg);
    } else if (scmp(cmd, "exit") == 0) {
        puts("bye.");
        proc_exit(0);
    } else {
        printf("?: %s\n", cmd);
    }
    return 0;
}

/* ---- pipe support ---- */

/* Task entry point for a pipe segment: redirect FDs, exec command, exit.
 * The FD table is global — we must be careful not to close pipe ends
 * that other segments still need.  Segments run in pipeline order
 * (enforced by priority) so that a writer finishes before its reader.
 *
 * After execution we restore fd 1 to the terminal (dup2 from stderr)
 * rather than closing it, so the fd-1 slot stays valid for the next
 * segment / the shell.  This also correctly decrements pipe_writers. */
void sh_pipe_segment_task(void)
{
    int my_pid = cur_pid;
    char *cmd = sh_seg_cmd[my_pid];
    char *arg = sh_seg_arg[my_pid];
    int in_fd  = sh_seg_stdin[my_pid];
    int out_fd = sh_seg_stdout[my_pid];

    /* redirect stdin */
    if (in_fd >= 0 && in_fd != 0) {
        sys_dup2(in_fd, 0);
        sys_close(in_fd);
    }
    /* redirect stdout */
    if (out_fd >= 0 && out_fd != 1) {
        sys_dup2(out_fd, 1);
        sys_close(out_fd);
    }

    sh_exec_cmd(cmd, arg);

    /* Release pipe ends we hold via fd 0 / fd 1.  For the write-end we
     * dup2 stderr (terminal) to fd 1 so the slot stays valid; for the
     * read-end a simple close is fine since we are exiting. */
    if (fd_type[1] == FD_PIPE_WR) {
        sys_dup2(2, 1);    /* restore fd 1 → terminal, drops pipe_writers */
    }
    if (fd_type[0] == FD_PIPE_RD) {
        sys_close(0);      /* drop pipe_readers (fd 0 slot won't be reused) */
    }
    proc_exit(0);
}

/* Run a pipeline (up to 4 segments separated by |). */
void sh_run_pipeline(char *line)
{
    char *segs[4];
    int pipes[3][2];    /* pipes between segments: up to 3 */
    int pids[4];
    int nseg, i, j, k;

    /* ---- split line into segments ---- */
    nseg = 0;
    segs[nseg++] = line;
    for (i = 0; line[i] && nseg < 4; i++) {
        if (line[i] == '|') {
            line[i] = 0;
            segs[nseg++] = line + i + 1;
        }
    }
    if (nseg < 2) return;

    /* ---- pre-resolve file paths (before pipe creation avoids a
     *      compiler bug that makes fs_resolve fail later). ----
     *      Temporarily store inodes in pids[]; they'll be moved to
     *      sh_seg_ino[] after proc_spawn assigns real pids. */
    for (i = 0; i < nseg; i++) {
        char *s = segs[i]; char ta[32]; int ik;
        pids[i] = 0;
        while (*s == ' ' || *s == '\t') s++;
        while (*s && *s != ' ' && *s != '\t') s++;   /* skip cmd */
        while (*s == ' ' || *s == '\t') s++;          /* skip whitespace */
        for (ik = 0; s[ik] && s[ik] != ' ' && s[ik] != '\t' && ik < 31; ik++)
            ta[ik] = s[ik];
        ta[ik] = 0;
        if (ta[0] == '/')
            pids[i] = fs_resolve(ta);
    }

    /* ---- create pipes ---- */
    for (i = 0; i < nseg - 1; i++) {
        if (sys_pipe(pipes[i]) < 0) {
            puts("pipe failed");
            for (j = 0; j < i; j++) {
                sys_close(pipes[j][0]);
                sys_close(pipes[j][1]);
            }
            return;
        }
    }

    /* ---- spawn each segment ---- */
    for (i = 0; i < nseg; i++) {
        char *s = segs[i];
        char tcmd[16], targ[32];
        int pid, in, out;

        while (*s == ' ' || *s == '\t') s++;

        for (j = 0; s[j] && s[j] != ' ' && s[j] != '\t' && j < 15; j++)
            tcmd[j] = s[j];
        tcmd[j] = 0;
        while (s[j] == ' ' || s[j] == '\t') j++;
        for (k = 0; s[j + k] && s[j + k] != ' ' && s[j + k] != '\t' && k < 31; k++)
            targ[k] = s[j + k];
        targ[k] = 0;

        {   int saved_ino = pids[i];   /* pre-resolved inode from above */
            pid = proc_spawn(sh_pipe_segment_task, cur_pid, "pip");
            pids[i] = pid;
            if (pid >= 0) {
                in  = (i > 0)       ? pipes[i-1][0] : 0;
                out = (i < nseg-1)  ? pipes[i][1]   : 1;
                sh_seg_ino[pid] = saved_ino;  /* transfer pre-resolved inode */
                /* copy into the child's per-pid slot */
                for (k = 0; tcmd[k]; k++)
                    sh_seg_cmd[pid][k] = tcmd[k];
                sh_seg_cmd[pid][k] = 0;
                for (k = 0; targ[k]; k++)
                    sh_seg_arg[pid][k] = targ[k];
                sh_seg_arg[pid][k] = 0;
                sh_seg_stdin[pid]  = in;
                sh_seg_stdout[pid] = out;
                p_priority[pid] = 100 - i;  /* writer-before-reader order */
            } else {
                puts("spawn failed");
                break;
            }
        }
    }

    /* ---- wait for all children (yields; priority ordering ensures
     *      segment 0 runs first, then segment 1, etc.) ---- */
    for (i = 0; i < nseg; i++)
        if (pids[i] >= 0)
            proc_wait(pids[i]);

    /* ---- parent cleans up any remaining pipe FDs ---- */
    for (i = 0; i < nseg - 1; i++) {
        if (fd_type[pipes[i][0]] != FD_FREE) sys_close(pipes[i][0]);
        if (fd_type[pipes[i][1]] != FD_FREE) sys_close(pipes[i][1]);
    }
}

/* ================================================================
 *  TEXT EDITOR
 * ================================================================ */
#define EDIT_BUF_SIZE 2048
#define EDIT_MAX_LINES 128

static char edit_buf[EDIT_BUF_SIZE];
static int  edit_len;
static int  edit_lines[EDIT_MAX_LINES];
static int  edit_nlines;
static char edit_path[32];

/* Rebuild the line-index from the text buffer.
 * edit_lines[i] = offset of line i in edit_buf.
 * A line is text up to (but not including) the next '\n'. */
static void edit_reindex(void)
{
    int i;
    edit_nlines = 0;
    if (edit_len > 0)
        edit_lines[edit_nlines++] = 0;
    for (i = 0; i < edit_len && edit_nlines < EDIT_MAX_LINES; i++) {
        if (edit_buf[i] == '\n' && i + 1 < edit_len)
            edit_lines[edit_nlines++] = i + 1;
    }
}

/* Print all lines with 1-based line numbers. */
static void edit_print(void)
{
    int i, j, start, end;
    if (edit_nlines == 0) {
        puts("(empty)");
        return;
    }
    for (i = 0; i < edit_nlines; i++) {
        print_string(" "); print_int(i + 1); print_string("| ");
        start = edit_lines[i];
        end = (i + 1 < edit_nlines) ? edit_lines[i + 1] - 1 : edit_len;
        /* strip trailing newline from display */
        if (end > start && edit_buf[end - 1] == '\n') end--;
        for (j = start; j < end; j++)
            putchar(edit_buf[j]);
        putchar('\n');
    }
}

/* Load file contents into the edit buffer.  If the file doesn't exist
 * we start with an empty buffer (new file). */
static int edit_load(char *path)
{
    int fd, n, total;
    total = 0;
    fd = sys_open(path);
    if (fd >= 0) {
        while (total < EDIT_BUF_SIZE - 1) {
            n = sys_read(fd, &edit_buf[total], EDIT_BUF_SIZE - 1 - total);
            if (n <= 0) break;
            total += n;
        }
        sys_close(fd);
    }
    edit_buf[total] = 0;
    edit_len = total;
    edit_reindex();
    return 0;
}

/* Save the edit buffer to the file.  Deletes the old file first,
 * then creates a fresh inode and writes the buffer. */
static int edit_save(void)
{
    int fd, i, written, parent, ino;
    char *p, *name, *slash;
    char tmp[32];

    if (edit_path[0] == 0) { puts("no file"); return -1; }

    /* delete old file if it exists (ignore failure) */
    fs_unlink(edit_path);

    /* parse path: split into parent directory + basename */
    for (i = 0; edit_path[i] && i < 31; i++) tmp[i] = edit_path[i];
    tmp[i] = 0;
    p = tmp; name = tmp; slash = tmp;
    while (*p) { if (*p == '/') slash = p; p++; }
    if (*name == '/' && slash == name) {
        parent = 0; name++;
    } else if (*name == '/') {
        *slash = 0; parent = fs_resolve(name); *slash = '/'; name = slash + 1;
    } else {
        parent = 0;
    }

    if (name[0] == 0) { puts("invalid path"); return -1; }
    if (parent < 0 || fs_type[parent] != 2) { puts("parent not a dir"); return -1; }

    ino = fs_ialloc(1, name);
    if (ino < 0) { puts("inode alloc failed"); return -1; }
    fs_dir_add(parent, name, ino);

    fd = fd_alloc();
    if (fd < 0) { puts("fd alloc failed"); return -1; }
    fd_type[fd] = FD_FILE; fd_ino[fd] = ino; fd_offset[fd] = 0;
    written = sys_write(fd, edit_buf, edit_len);
    sys_close(fd);
    printf("saved %d bytes to %s\n", written, edit_path);
    return 0;
}

/* Append a line of text to the end of the buffer. */
static void edit_append(char *text)
{
    int tlen, i;
    tlen = strlen(text);
    /* ensure there's room: possibly need an extra '\n' separator + text + '\n' */
    if (edit_len + tlen + 2 >= EDIT_BUF_SIZE) {
        puts("buffer full");
        return;
    }
    if (edit_nlines >= EDIT_MAX_LINES) {
        puts("too many lines");
        return;
    }
    /* if the buffer doesn't end with a newline, add one to finish the last line */
    if (edit_len > 0 && edit_buf[edit_len - 1] != '\n')
        edit_buf[edit_len++] = '\n';
    for (i = 0; i < tlen; i++)
        edit_buf[edit_len++] = text[i];
    edit_buf[edit_len++] = '\n';
    edit_reindex();
}

/* Insert a line BEFORE line N (1-based).  The old line N becomes line N+1. */
static void edit_insert(int line, char *text)
{
    int tlen, offset, i, shift;
    if (line < 1 || line > edit_nlines) {
        puts("invalid line number");
        return;
    }
    if (edit_nlines >= EDIT_MAX_LINES) {
        puts("too many lines");
        return;
    }
    tlen = strlen(text);
    if (edit_len + tlen + 1 >= EDIT_BUF_SIZE) {
        puts("buffer full");
        return;
    }
    offset = edit_lines[line - 1];
    shift = tlen + 1;   /* text + newline */
    /* shift existing text right to make room */
    for (i = edit_len - 1; i >= offset; i--)
        edit_buf[i + shift] = edit_buf[i];
    /* insert new text + newline */
    for (i = 0; i < tlen; i++)
        edit_buf[offset + i] = text[i];
    edit_buf[offset + tlen] = '\n';
    edit_len += shift;
    edit_reindex();
}

/* Delete line N (1-based). */
static void edit_delete(int line)
{
    int start, end, len, i;
    if (line < 1 || line > edit_nlines) {
        puts("invalid line number");
        return;
    }
    start = edit_lines[line - 1];
    end = (line < edit_nlines) ? edit_lines[line] : edit_len;
    len = end - start;
    /* shift remaining text left */
    for (i = end; i < edit_len; i++)
        edit_buf[i - len] = edit_buf[i];
    edit_len -= len;
    edit_reindex();
}

/* Show help. */
static void edit_help(void)
{
    puts("Commands:");
    puts(" p           print all lines");
    puts(" a <text>    append a line");
    puts(" i <N> <txt> insert before line N");
    puts(" d <N>       delete line N");
    puts(" w           save to file");
    puts(" q           quit editor");
    puts(" h           this help");
}

/* Parse a positive integer, advance *next past it. */
static int edit_parse_num(char *s, char **next)
{
    int n;
    n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    if (next) *next = s;
    return n;
}

/* Editor entry point.  Called from sh_exec_cmd("edit", path).
 * Enters an interactive loop until the user quits. */
void editor_task(char *path)
{
    char line[96];
    char cmd;
    int  lnum;
    char *rest;
    int  i;

    /* copy path to static buffer */
    for (i = 0; path[i] && i < 31; i++)
        edit_path[i] = path[i];
    edit_path[i] = 0;

    if (path[0] == 0) {
        puts("usage: edit <file>");
        return;
    }

    edit_len = 0;
    edit_nlines = 0;
    edit_load(path);

    printf("=== EDITOR: %s ===\n", edit_path);
    edit_print();

    while (1) {
        putchar('>'); putchar(' ');
        sh_readln(line, 96);
        putchar('\n');

        if (line[0] == 0) continue;

        cmd = line[0];
        rest = line + 1;
        while (*rest == ' ' || *rest == '\t') rest++;

        if (cmd == 'q') {
            puts("bye.");
            break;
        } else if (cmd == 'h') {
            edit_help();
        } else if (cmd == 'p') {
            edit_print();
        } else if (cmd == 'w') {
            edit_save();
        } else if (cmd == 'a') {
            if (rest[0] == 0)
                puts("usage: a <text>");
            else
                edit_append(rest);
        } else if (cmd == 'i') {
            lnum = edit_parse_num(rest, &rest);
            while (*rest == ' ' || *rest == '\t') rest++;
            if (lnum < 1 || rest[0] == 0)
                puts("usage: i <N> <text>");
            else
                edit_insert(lnum, rest);
        } else if (cmd == 'd') {
            lnum = edit_parse_num(rest, &rest);
            if (lnum < 1)
                puts("usage: d <N>");
            else
                edit_delete(lnum);
        } else {
            printf("? '%c'. h for help.\n", cmd);
        }
    }
}

/* ================================================================
 *  DEMO: pipe a message through to the terminal
 * ================================================================ */
void pipe_demo_task(void)
{
    int fds[2], pid, buf[64], n, i;
    char *msg = "hello through a pipe!";

    if (sys_pipe(fds) < 0) { puts("pipe fail"); proc_exit(1); }

    /* write to pipe */
    for (i = 0; msg[i]; i++) buf[i] = msg[i];
    sys_write(fds[1], buf, i);
    sys_close(fds[1]);

    /* read from pipe */
    n = sys_read(fds[0], buf, 64);
    sys_close(fds[0]);

    for (i = 0; i < n; i++) putchar(buf[i]);
    putchar('\n');

    proc_exit(0);
}

/* ================================================================
 *  PRIORITY DEMO: 3 tasks at priorities 0, 1, 2 each print a char
 * ================================================================ */
void prio_worker_task(void)
{
    /* pid is passed via the stack frame (see proc_spawn).
     * We grab the priority and print a distinct marker. */
    int my_prio, i;
    char marker;
    my_prio = p_priority[cur_pid];
    marker = '0' + my_prio;   /* '0', '1', or '2' */
    for (i = 0; i < 8; i++) {
        putchar(marker);
        proc_sleep(10);        /* let others run */
    }
    proc_exit(0);
}

void prio_demo_task(void)
{
    int lo, mid, hi;
    /* spawn 3 workers at different priorities */
    lo  = proc_spawn(prio_worker_task, cur_pid, "wk0");
    mid = proc_spawn(prio_worker_task, cur_pid, "wk1");
    hi  = proc_spawn(prio_worker_task, cur_pid, "wk2");
    p_priority[lo]  = 0;
    p_priority[mid] = 5;
    p_priority[hi]  = 9;
    printf("prio demo: lo=%d(p=0) mid=%d(p=5) hi=%d(p=9)\n", lo, mid, hi);
    putchar('[');
    proc_wait(-1);  /* wait for all 3 (they all exit) */
    proc_wait(-1);
    proc_wait(-1);
    puts("] done");
    proc_exit(0);
}

/* ================================================================
 *  FS DEMO: populate initial filesystem
 * ================================================================ */
void fs_populate(void)
{
    int ino;

    ino = fs_ialloc(1, "motd");
    fs_dir_add(0, "motd", ino);
    fs_write_str(ino, "Welcome to RISKY OS v3!");

    ino = fs_ialloc(1, "secrets");
    fs_dir_add(0, "secrets", ino);
    fs_write_str(ino, "the answer is 42");

    /* create /etc directory */
    ino = fs_ialloc(2, "etc");
    fs_dir_add(0, "etc", ino);

    /* create /etc/passwd */
    {
        int eino = fs_ialloc(1, "passwd");
        fs_dir_add(ino, "passwd", eino);
        fs_write_str(eino, "root:x:0:0:root:/:/bin/sh\n");
    }

    /* create /bin directory */
    ino = fs_ialloc(2, "bin");
    fs_dir_add(0, "bin", ino);
}

/* ================================================================
 *  MAIN
 * ================================================================ */
int main(void)
{
    int i, alive, code;

    for (i = 0; i < MAX_PROCS; i++) p_state[i] = S_FREE;
    fd_init();
    pipe_init();
    fs_init_simple();
    fs_populate();
    sys_tick = 0;

    p_state[0] = S_READY; p_ppid[0] = -1; p_exit[0] = 0;
    p_priority[0] = 0; p_sleep_until[0] = 0;
    p_name[0][0] = 'i'; p_name[0][1] = 'n'; p_name[0][2] = 'i'; p_name[0][3] = 't';
    cur_pid = 0;

    puts("RISKY OS v3 — booting");

    i = proc_spawn(shell_task, 0, "sh");

    while (1) {
        alive = 0;
        for (i = 1; i < MAX_PROCS; i++)
            if (p_state[i] == S_READY || p_state[i] == S_BLOCKED
                || p_state[i] == S_SLEEP) alive = 1;
        if (!alive) {
            for (i = 1; i < MAX_PROCS; i++)
                if (p_state[i] == S_ZOMBIE) {
                    printf("[%d exited: %d]\n", i, p_exit[i]);
                    p_state[i] = S_FREE;
                }
            break;
        }
        code = proc_wait(-1);
        for (i = 1; i < MAX_PROCS; i++)
            if (p_state[i] == S_ZOMBIE) {
                printf("[%d exited: %d]\n", i, code);
                p_state[i] = S_FREE;
            }
    }

    puts("Halting.");
    __asm__("\n\tjmp __halt");
    return 0;
}
