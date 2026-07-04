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

#define SAVE_SP    0x7FF0
#define SAVE_R15   0x7FF1
#define NEXT_SP    0x7FF2
#define NEXT_R15   0x7FF3

int p_sp[MAX_PROCS], p_r15[MAX_PROCS];
int p_state[MAX_PROCS];      /* 0=free 1=ready 2=blocked 3=zombie */
int p_ppid[MAX_PROCS];
int p_exit[MAX_PROCS];
int p_wait4[MAX_PROCS];
int p_msgs[MAX_PROCS][MSG_Q];
int p_msgcnt[MAX_PROCS];
int p_msgnext[MAX_PROCS];
int p_name[MAX_PROCS][4];
int cur_pid;

static int stk_pool[MAX_PROCS][STK_WORDS];

int sched_next(int cur)
{ int n = cur; do { n = (n+1) % MAX_PROCS; } while (p_state[n] != 1); return n; }

void __sched(void)
{
    int *s = (int *) SAVE_SP;
    p_sp[cur_pid]  = s[0];
    p_r15[cur_pid] = s[1];
    do { cur_pid = (cur_pid+1) % MAX_PROCS; } while (p_state[cur_pid] != 1);
    s[2] = p_sp[cur_pid];
    s[3] = p_r15[cur_pid];
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
        if (p_state[pid] == 0) break;
    if (pid == MAX_PROCS) return -1;
    stack = stk_pool[pid];
    sp = stack + STK_WORDS - 4;
    sp[1] = (int) fn; sp[0] = 0; sp[-1] = (int) pid;
    p_sp[pid] = (int) sp; p_r15[pid] = (int)(sp + 1);
    p_state[pid] = 1; p_ppid[pid] = ppid; p_exit[pid] = 0;
    p_wait4[pid] = -1; p_msgcnt[pid] = 0; p_msgnext[pid] = 0;
    for (i = 0; name[i] && i < 4; i++) p_name[pid][i] = name[i];
    for (; i < 4; i++) p_name[pid][i] = 0;
    return pid;
}

void proc_exit(int code)
{
    int pp = p_ppid[cur_pid];
    p_exit[cur_pid] = code; p_state[cur_pid] = 3;
    if (pp >= 0 && p_state[pp] == 2 && (p_wait4[pp] == cur_pid || p_wait4[pp] == -1))
        p_state[pp] = 1;
    while (1) yield();
}

int proc_wait(int pid)
{
    int i;
    for (i = 0; i < MAX_PROCS; i++)
        if (p_state[i] == 3 && p_ppid[i] == cur_pid && (pid == -1 || pid == i))
        { int c = p_exit[i]; p_state[i] = 0; return c; }
    p_wait4[cur_pid] = pid; p_state[cur_pid] = 2; yield();
    for (i = 0; i < MAX_PROCS; i++)
        if (p_state[i] == 3 && p_ppid[i] == cur_pid && (pid == -1 || pid == i))
        { int c = p_exit[i]; p_state[i] = 0; return c; }
    return -1;
}

int send_msg(int dst, int msg)
{
    int w;
    if (p_state[dst] != 1 && p_state[dst] != 2) return -1;
    if (p_msgcnt[dst] >= MSG_Q) return -2;
    w = (p_msgnext[dst] + p_msgcnt[dst]) % MSG_Q;
    p_msgs[dst][w] = msg; p_msgcnt[dst]++;
    if (p_state[dst] == 2 && p_wait4[dst] == -2) p_state[dst] = 1;
    yield(); return 0;
}

int recv_msg(int *msg)
{
    while (p_msgcnt[cur_pid] == 0)
    { p_wait4[cur_pid] = -2; p_state[cur_pid] = 2; yield(); }
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

int fd_type[MAX_FD];
int fd_ino[MAX_FD];        /* for FD_FILE: inode number */
int fd_offset[MAX_FD];     /* for FD_FILE: current read/write offset */
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
int pipe_readers[MAX_PIPES];   /* FD count of read ends */
int pipe_writers[MAX_PIPES];   /* FD count of write ends */

void pipe_init(void)
{
    int i;
    for (i = 0; i < MAX_PIPES; i++) pipe_used[i] = 0;
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

    fd_type[rfd] = FD_PIPE_RD; fd_pipe[rfd] = i;
    fd_type[wfd] = FD_PIPE_WR; fd_pipe[wfd] = i;

    fds[0] = rfd; fds[1] = wfd;
    return 0;
}

int pipe_write(int pi, int *buf, int words)
{
    int i, written, pos;
    written = 0;
    while (written < words) {
        if (pipe_count[pi] >= PIPE_BUF) break;
        pos = (pipe_head[pi] + pipe_count[pi]) % PIPE_BUF;
        pipe_buf[pi][pos] = buf[written++];
        pipe_count[pi]++;
    }
    return written;
}

int pipe_read(int pi, int *buf, int words)
{
    int i, nread = 0;
    while (nread < words && pipe_count[pi] > 0) {
        buf[nread++] = pipe_buf[pi][pipe_tail[pi]];
        pipe_tail[pi] = (pipe_tail[pi] + 1) % PIPE_BUF;
        pipe_count[pi]--;
    }
    return nread;
}

void pipe_close_read(int pi)
{
    pipe_readers[pi]--;
    if (pipe_readers[pi] <= 0 && pipe_writers[pi] <= 0)
        pipe_used[pi] = 0;
}

void pipe_close_write(int pi)
{
    pipe_writers[pi]--;
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

/* inode (in page 0 RAM) */
int  fs_type[FS_INODES];     /* 0=free 1=file 2=dir */
int  fs_size[FS_INODES];
int  fs_blocks[FS_INODES][FS_DIRECT];  /* page:addr encoded */
char fs_name[FS_INODES][FS_NAMELEN];

/* block allocator: track free blocks across pages 1-7.
 * Simple: pages 1-7, blocks numbered 0..7167 (7*64K/256 = 7*256 = 1792,
 * but we store per-page bitmaps).  Keep it simple: next free block counter. */
int fs_next_page;
int fs_next_addr;

/* File data in page 0, starting at 0x8000.
 * Each inode gets up to FS_DIRECT blocks of FS_BLKSZ words.
 * Block pointer is a byte offset from 0x8000. */
#define FS_DATA_BASE 0x8000

int fs_alloc_block_simple(void)
{
    static int next_blk = FS_BLKSZ;    /* 256 = first block; 0 = unallocated */
    int blk = next_blk;
    next_blk += FS_BLKSZ;
    if (next_blk > 0x7000) return -1;  /* 28K of file data in page 0 */
    return blk;
}

void fs_init_simple(void)
{
    int i, j;
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
    int cur_blocks, need_blocks, i, blk;
    if (ino < 0 || ino >= FS_INODES || fs_type[ino] == 0) return -1;
    cur_blocks = (fs_size[ino] + FS_BLKSZ - 1) / FS_BLKSZ;
    need_blocks = (newsize + FS_BLKSZ - 1) / FS_BLKSZ;
    for (i = cur_blocks; i < need_blocks && i < FS_DIRECT; i++) {
        blk = fs_alloc_block_simple();
        if (blk < 0) return -1;
        fs_blocks[ino][i] = blk;
    }
    if (newsize > fs_size[ino]) fs_size[ino] = newsize;
    return 0;
}

int fs_writei(int ino, int offset, int *buf, int words)
{
    int i, blk_idx, blk_off, blk, addr, *dst;
    if (fs_grow(ino, offset + words) < 0) return -1;
    for (i = 0; i < words; i++) {
        blk_idx = (offset + i) / FS_BLKSZ;
        blk_off = (offset + i) % FS_BLKSZ;
        if (blk_idx >= FS_DIRECT) break;
        blk = fs_blocks[ino][blk_idx];
        addr = FS_DATA_BASE + blk + blk_off;
        dst = (int *) addr;
        *dst = buf[i];
    }
    return i;
}

/* Write string to inode — simple version */
int fs_write_str(int ino, char *s)
{
    int i, blk_idx, blk, addr;
    int *dst;
    for (i = 0; s[i]; i++) {
        if (i >= fs_size[ino]) {
            /* grow */
            blk_idx = i / FS_BLKSZ;
            if (blk_idx >= FS_DIRECT) return i;
            if (fs_blocks[ino][blk_idx] == 0) {
                int nb = fs_alloc_block_simple();
                if (nb < 0) return i;
                fs_blocks[ino][blk_idx] = nb;
            }
            fs_size[ino] = i + 1;
        }
        blk_idx = i / FS_BLKSZ;
        blk = fs_blocks[ino][blk_idx];
        addr = FS_DATA_BASE + blk + (i % FS_BLKSZ);
        dst = (int *) addr;
        *dst = s[i];
    }
    return i;
}

int fs_readi(int ino, int offset, int *buf, int words)
{
    int i, blk_idx, blk, addr, *src;
    if (ino < 0 || ino >= FS_INODES || fs_type[ino] == 0) return -1;
    for (i = 0; i < words && offset + i < fs_size[ino]; i++) {
        blk_idx = (offset + i) / FS_BLKSZ;
        if (blk_idx >= FS_DIRECT) break;
        blk = fs_blocks[ino][blk_idx];
        if (blk == 0) break;
        addr = FS_DATA_BASE + blk + ((offset + i) % FS_BLKSZ);
        src = (int *) addr;
        buf[i] = *src;
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

/* ================================================================
 *  SHELL
 * ================================================================ */
int scmp(char *a, char *b) { while (*a && *a == *b) { a++; b++; } return *a - *b; }

void sh_readln(char *buf, int max)
{
    int c, i, tmp[1];
    i = 0;
    while (i < max - 1) {
        if (sys_read(0, tmp, 1) <= 0) break;
        c = tmp[0];
        if (c == '\n') break;
        buf[i++] = c;
        putchar(c);  /* echo */
    }
    buf[i] = 0;
}

void sh_cat(char *path)
{
    int fd, buf[64], n, i;
    fd = sys_open(path);
    if (fd < 0) { puts("not found"); return; }
    while ((n = sys_read(fd, buf, 64)) > 0)
        for (i = 0; i < n; i++) putchar(buf[i]);
    putchar('\n');
    sys_close(fd);
}

void sh_ps(void)
{
    int i; char *sts = "?RBZ";
    puts("PID PPID ST NAME");
    for (i = 0; i < MAX_PROCS; i++) if (p_state[i]) {
        printf(" %d   %d   %c  ", i, p_ppid[i], sts[p_state[i]]);
        { int j; for (j = 0; j < 4 && p_name[i][j]; j++) putchar(p_name[i][j]); }
        putchar('\n');
    }
}

void shell_task(void)
{
    char line[64], cmd[32], arg[64];
    int c, i, fd, fds[2], pid, buf[256], n;

    puts("RISKY OS v3 shell.  Type 'help'.");

    while (1) {
        putchar('$'); putchar(' ');
        sh_readln(line, 64);
        if (line[0] == 0) {
            putchar('\n');
            continue;
        }
        putchar('\n');

        /* parse cmd and arg */
        for (i = 0; line[i] == ' ' || line[i] == '\t'; i++);
        { int j; for (j = 0; line[i] && line[i] != ' ' && line[i] != '\t' && j < 31; i++, j++)
            cmd[j] = line[i]; cmd[j] = 0; }
        while (line[i] == ' ' || line[i] == '\t') i++;
        { int j; for (j = 0; line[i] && j < 63; i++, j++)
            arg[j] = line[i]; arg[j] = 0; }

        if (cmd[0] == 0) { yield(); continue; }

        if (scmp(cmd, "help") == 0) {
            puts("help ps ls cat mkfile mkdir pipe write exit");
        } else if (scmp(cmd, "ps") == 0) {
            sh_ps();
        } else if (scmp(cmd, "ls") == 0) {
            if (arg[0]) { fs_dir_list(fs_resolve(arg)); putchar('\n'); }
            else { fs_dir_list(0); putchar('\n'); }
        } else if (scmp(cmd, "cat") == 0) {
            sh_cat(arg);
        } else if (scmp(cmd, "mkfile") == 0 || scmp(cmd, "mkdir") == 0) {
            /* Parse path: split into parent dir and name.
             * e.g. "/logs/boot" → parent = fs_resolve("/logs"), name = "boot" */
            int parent, type, ino;
            char *path, *name, *slash;
            path = arg;
            /* find last '/' */
            name = path;
            slash = path;
            while (*path) { if (*path == '/') slash = path; path++; }
            if (slash == name) { parent = 0; name++; }      /* "/name" */
            else { *slash = 0; parent = fs_resolve(arg); *slash = '/'; name = slash + 1; }
            type = (cmd[0] == 'm' && cmd[2] == 'f') ? 1 : 2;  /* mkfile→1, mkdir→2 */
            ino = fs_ialloc(type, name);
            if (ino >= 0 && parent >= 0 && fs_type[parent] == 2) {
                fs_dir_add(parent, name, ino);
                printf("created %s %s (ino %d)\n", type==2?"dir":"file", name, ino);
            } else {
                puts("create failed");
            }
        } else if (scmp(cmd, "write") == 0) {
            /* write <path> <text> */
            char *path, *text;
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
        } else if (scmp(cmd, "pipe") == 0) {
            if (sys_pipe(fds) == 0)
                printf("pipe: rd=%d wr=%d\n", fds[0], fds[1]);
            else
                puts("pipe failed");
        } else if (scmp(cmd, "echo") == 0) {
            puts(arg);
        } else if (scmp(cmd, "spawn") == 0) {
            pid = proc_spawn(shell_task, cur_pid, "sh2");
            printf("spawned %d\n", pid);
        } else if (scmp(cmd, "exit") == 0) {
            puts("bye.");
            proc_exit(0);
        } else {
            printf("?: %s\n", cmd);
        }
        yield();
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

    for (i = 0; i < MAX_PROCS; i++) p_state[i] = 0;
    fd_init();
    pipe_init();
    fs_init_simple();
    fs_populate();

    p_state[0] = 1; p_ppid[0] = -1; p_exit[0] = 0;
    p_name[0][0] = 'i'; p_name[0][1] = 'n'; p_name[0][2] = 'i'; p_name[0][3] = 't';
    cur_pid = 0;

    puts("RISKY OS v3 — booting");

    i = proc_spawn(shell_task, 0, "sh");

    while (1) {
        alive = 0;
        for (i = 1; i < MAX_PROCS; i++)
            if (p_state[i] == 1 || p_state[i] == 2) alive = 1;
        if (!alive) {
            for (i = 1; i < MAX_PROCS; i++)
                if (p_state[i] == 3) {
                    printf("[%d exited: %d]\n", i, p_exit[i]);
                    p_state[i] = 0;
                }
            break;
        }
        code = proc_wait(-1);
        for (i = 1; i < MAX_PROCS; i++)
            if (p_state[i] == 3) {
                printf("[%d exited: %d]\n", i, code);
                p_state[i] = 0;
            }
    }

    puts("Halting.");
    __asm__("\n\tjmp __halt");
    return 0;
}
