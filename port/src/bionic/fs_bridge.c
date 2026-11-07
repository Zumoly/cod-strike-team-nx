#include "port/bionic_fs.h"
#include "port/vfs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Guest errno lives apart from the host's so guest code never sees newlib numbers.
static __thread int t_errno;

int *b_errno(void)
{
    return &t_errno;
}

// Linux numbers for the values file, socket and threading code can produce.
int b_errno_from_native(int e)
{
    switch (e) {
    case 0: return 0;
    case EPERM: return 1;
    case ENOENT: return 2;
    case EINTR: return 4;
    case EIO: return 5;
    case EBADF: return 9;
    case EAGAIN: return 11;
    case ENOMEM: return 12;
    case EACCES: return 13;
    case EBUSY: return 16;
    case EEXIST: return 17;
    case EXDEV: return 18;
    case ENOTDIR: return 20;
    case EISDIR: return 21;
    case EINVAL: return 22;
    case EMFILE: return 24;
    case EFBIG: return 27;
    case ENOSPC: return 28;
    case ESPIPE: return 29;
    case EROFS: return 30;
    case EPIPE: return 32;
    case ERANGE: return 34;
    case EDEADLK: return 35;
    case ENAMETOOLONG: return 36;
    case ENOSYS: return 38;
    case ENOTEMPTY: return 39;
    case ETIMEDOUT: return 110;
    case ECONNREFUSED: return 111;
    default: return 5; // EIO: never leak a newlib-only number
    }
}

#define FAIL_OP(op, fd_or_zero, native) fail_named(op, (int)(fd_or_zero), native)

static int fail(int native)
{
    t_errno = b_errno_from_native(native);
    return -1;
}

// Horizon's filesystem differs from Linux in ways the guest cannot be told about, so
// failures on open files are always worth a line in the log.
static int fail_named(const char *op, int fd, int native)
{
    fprintf(stderr, "fs: %s on fd %d failed, errno %d\n", op, fd, native);
    return fail(native);
}

#ifdef __SWITCH__
extern uint32_t fsdevGetLastResult(void);
#define FS_RESULT() ((unsigned)fsdevGetLastResult())
#else
#define FS_RESULT() 0u
#endif

// A missing file is an ordinary answer.  Anything else here means Horizon refused.
static int fail_path(const char *op, const char *path, int native)
{
    if (native != ENOENT && native != EEXIST)
        fprintf(stderr, "fs: %s %s failed, errno %d, fs result 0x%x\n", op, path, native, FS_RESULT());
    return fail(native);
}

// ---- files this process holds open ---------------------------------------------
//
// Horizon lets one handle at a time write a file, and then no other handle can open it.  libnx's
// stat() opens the file to read its size, so stat, access and truncate by name all fail on a file
// open for writing.  Code that reaches a file by name while holding it must use the handle it has.
//
// libnx also reports inode 0 for every file.  SQLite keys its file state on the inode, so it saw
// its journal and database as one file.  Each file gets an inode from its path, case-blind as on FAT.

#define MAX_TRACKED 128

static struct {
    int fd_plus_1;
    uint32_t ino;
    int writable;
    int endless_random;
} g_files[MAX_TRACKED];
static atomic_flag g_files_lock = ATOMIC_FLAG_INIT;

static void files_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&g_files_lock, memory_order_acquire)) {
    }
}

static void files_unlock(void)
{
    atomic_flag_clear_explicit(&g_files_lock, memory_order_release);
}

static uint32_t path_ino(const char *host)
{
    uint32_t h = 2166136261u;
    for (; *host; host++)
        h = (h ^ (uint32_t)tolower((unsigned char)*host)) * 16777619u;
    return h ? h : 1;
}

void b_fs_forget(int fd)
{
    files_lock();
    for (int i = 0; i < MAX_TRACKED; i++)
        if (g_files[i].fd_plus_1 == fd + 1)
            g_files[i].fd_plus_1 = 0;
    files_unlock();
}

static void track(int fd, const char *host, int writable)
{
    uint32_t ino = path_ino(host);
    b_fs_forget(fd); // a number fresh from open: any entry holding it is stale
    files_lock();
    for (int i = 0; i < MAX_TRACKED; i++)
        if (!g_files[i].fd_plus_1) {
            g_files[i].fd_plus_1 = fd + 1;
            g_files[i].ino = ino;
            g_files[i].writable = writable;
            g_files[i].endless_random = 0;
            break;
        }
    files_unlock();
}

static int writer_of(const char *host)
{
    uint32_t ino = path_ino(host);
    int fd = -1;
    files_lock();
    for (int i = 0; i < MAX_TRACKED && fd < 0; i++)
        if (g_files[i].fd_plus_1 && g_files[i].writable && g_files[i].ino == ino)
            fd = g_files[i].fd_plus_1 - 1;
    files_unlock();
    return fd;
}

static uint32_t ino_of(int fd)
{
    uint32_t ino = 0x80000000u | (uint32_t)fd; // untracked: stdio and the directory placeholder
    files_lock();
    for (int i = 0; i < MAX_TRACKED; i++)
        if (g_files[i].fd_plus_1 == fd + 1) {
            ino = g_files[i].ino;
            break;
        }
    files_unlock();
    return ino;
}

// /dev/urandom never ends, and Mono's generator reads in a loop until it has every byte it wants.
// A stand-in file reaches end of file, read returns 0 for ever, and the loop never finishes: that
// hung level loading and, by flooding the filesystem service, the whole console.
static int is_endless_random(int fd)
{
    int found = 0;
    files_lock();
    for (int i = 0; i < MAX_TRACKED; i++)
        if (g_files[i].fd_plus_1 == fd + 1) {
            found = g_files[i].endless_random;
            break;
        }
    files_unlock();
    return found;
}

static void mark_endless_random(int fd)
{
    files_lock();
    for (int i = 0; i < MAX_TRACKED; i++)
        if (g_files[i].fd_plus_1 == fd + 1)
            g_files[i].endless_random = 1;
    files_unlock();
}

static void fill_random(uint8_t *out, uint32_t n)
{
    static _Atomic uint64_t state;
    extern int64_t b_monotonic_ns(void);
    uint64_t x = atomic_fetch_add_explicit(&state, 0x9E3779B97F4A7C15ull, memory_order_relaxed);
    x ^= (uint64_t)b_monotonic_ns() * 0xBF58476D1CE4E5B9ull;
    for (uint32_t i = 0; i < n; i++) {
        if ((i & 7) == 0) {
            x += 0x9E3779B97F4A7C15ull;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
            x ^= x >> 31;
        }
        out[i] = (uint8_t)(x >> ((i & 7) * 8));
    }
}

static int resolve(const char *guest, char *host, int must_exist)
{
    int r = vfs_resolve(guest, host, VFS_PATH_MAX, must_exist);
    if (r < 0)
        t_errno = b_errno_from_native(-r);
    return r;
}

int b_flags_to_native(int f)
{
    int n;
    switch (f & 3) {
    case BIONIC_O_WRONLY: n = O_WRONLY; break;
    case BIONIC_O_RDWR: n = O_RDWR; break;
    default: n = O_RDONLY; break;
    }
    if (f & BIONIC_O_CREAT) n |= O_CREAT;
    if (f & BIONIC_O_EXCL) n |= O_EXCL;
    if (f & BIONIC_O_TRUNC) n |= O_TRUNC;
    if (f & BIONIC_O_APPEND) n |= O_APPEND;
    if (f & BIONIC_O_NONBLOCK) n |= O_NONBLOCK;
#ifdef O_DIRECTORY
    if (f & BIONIC_O_DIRECTORY) n |= O_DIRECTORY;
#endif
    return n;
}

int b_open(const char *path, int flags, ...)
{
    uint32_t mode = 0;
    if (flags & BIONIC_O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, uint32_t);
        va_end(ap);
    }
    char host[VFS_PATH_MAX];
    if (resolve(path, host, !(flags & BIONIC_O_CREAT)) < 0) {
        fprintf(stderr, "fs: open %s flags 0x%x: path refused, errno %d\n", path, (unsigned)flags, *b_errno());
        return -1;
    }
    int fd = open(host, b_flags_to_native(flags), mode);
    if (fd < 0) {
        int e = errno;
        // SQLite opens directories to fsync them.  Horizon cannot, and that sync means
        // nothing here, so any readable descriptor keeps it happy.
        struct stat st;
        if (stat(host, &st) == 0 && S_ISDIR(st.st_mode)) {
            char placeholder[VFS_PATH_MAX];
            if (vfs_resolve("/proc/cpuinfo", placeholder, sizeof placeholder, 1) == 0 &&
                (fd = open(placeholder, O_RDONLY)) >= 0)
                return fd;
        }
        fprintf(stderr, "fs: open %s as %s flags 0x%x failed, errno %d, fs result 0x%x\n", path, host,
                (unsigned)flags, e, FS_RESULT());
        return fail(e);
    }
    track(fd, host, (flags & 3) != 0);
    if (strcmp(path, "/dev/urandom") == 0 || strcmp(path, "/dev/random") == 0)
        mark_endless_random(fd);
    return fd;
}

int b_close(int fd)
{
    b_fs_forget(fd);
    return close(fd) < 0 ? fail(errno) : 0;
}

int32_t b_read(int fd, void *buf, uint32_t n)
{
    if (is_endless_random(fd)) {
        fill_random(buf, n);
        return (int32_t)n;
    }
    ssize_t r = read(fd, buf, n);
    if (r >= 0)
        return (int32_t)r;
    // Horizon refuses reads that start past end of file, where Linux returns 0.  SQLite
    // depends on that: it reads beyond the end to detect pages not written yet.
    int e = errno;
    struct stat st;
    off_t at = lseek(fd, 0, SEEK_CUR);
    if (at >= 0 && fstat(fd, &st) == 0 && at >= st.st_size)
        return 0;
    return FAIL_OP("read", fd, e);
}

int32_t b_write(int fd, const void *buf, uint32_t n)
{
    ssize_t r = write(fd, buf, n);
    return r < 0 ? FAIL_OP("write", fd, errno) : (int32_t)r;
}

int32_t b_lseek(int fd, int32_t offset, int whence)
{
    off_t r = lseek(fd, (off_t)offset, whence);
    if (r < 0)
        return FAIL_OP("lseek", fd, errno);
    if (r > INT32_MAX)
        return fail(EFBIG); // would alias a negative error return
    return (int32_t)r;
}

FILE *b_fopen(const char *path, const char *mode)
{
    char host[VFS_PATH_MAX];
    if (resolve(path, host, mode[0] == 'r') < 0)
        return NULL;
    FILE *f = fopen(host, mode);
    if (!f) {
        fail_path("fopen", path, errno);
        return NULL;
    }
    track(fileno(f), host, mode[0] != 'r' || strchr(mode, '+') != NULL);
    return f;
}

static void to_bionic_stat(const struct stat *s, uint32_t ino, struct bionic_stat *o)
{
    memset(o, 0, sizeof *o);
    o->st_dev = 1;
    o->st_ino = ino;
    o->__st_ino = ino;
    o->st_mode = (uint32_t)s->st_mode;
    o->st_nlink = 1;
    o->st_size = (int64_t)s->st_size;
    o->st_blksize = 4096;
    o->st_blocks = ((uint64_t)s->st_size + 511) / 512;
    o->st_atime_s = (uint32_t)s->st_atime;
    o->st_mtime_s = (uint32_t)s->st_mtime;
    o->st_ctime_s = (uint32_t)s->st_ctime;
}

int b_stat(const char *path, struct bionic_stat *out)
{
    char host[VFS_PATH_MAX];
    struct stat st;
    if (resolve(path, host, 1) < 0)
        return -1;
    int writer = writer_of(host);
    if (writer >= 0 && fstat(writer, &st) == 0) {
        st.st_mode = (st.st_mode & ~(mode_t)S_IFMT) | S_IFREG;
    } else if (stat(host, &st) < 0) {
        return fail_path("stat", path, errno);
    }
    to_bionic_stat(&st, path_ino(host), out);
    return 0;
}

int b_lstat(const char *path, struct bionic_stat *out)
{
    return b_stat(path, out); // FAT has no symlinks
}

int b_access(const char *path, int mode)
{
    (void)mode; // no permission model on FAT: existence is the only answer
    struct bionic_stat st;
    return b_stat(path, &st);
}

int b_fstat(int fd, struct bionic_stat *out)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return FAIL_OP("fstat", fd, errno);
    to_bionic_stat(&st, ino_of(fd), out);
    return 0;
}

struct bionic_dir {
    DIR *dir;
    struct bionic_dirent entry;
};

bionic_DIR *b_opendir(const char *path)
{
    char host[VFS_PATH_MAX];
    if (resolve(path, host, 1) < 0)
        return NULL;
    DIR *d = opendir(host);
    if (!d) {
        fail(errno);
        return NULL;
    }
    bionic_DIR *b = calloc(1, sizeof *b);
    if (!b) {
        closedir(d);
        fail(ENOMEM);
        return NULL;
    }
    b->dir = d;
    return b;
}

struct bionic_dirent *b_readdir(bionic_DIR *b)
{
    struct dirent *e = readdir(b->dir);
    if (!e)
        return NULL;
    struct bionic_dirent *o = &b->entry;
    memset(o, 0, sizeof *o);
    o->d_ino = (uint64_t)e->d_ino;
    o->d_reclen = sizeof *o;
    o->d_type = e->d_type;
    size_t n = strlen(e->d_name);
    memcpy(o->d_name, e->d_name, n < sizeof o->d_name ? n : sizeof o->d_name - 1);
    return o;
}

int b_closedir(bionic_DIR *b)
{
    closedir(b->dir);
    free(b);
    return 0;
}

#define WRAP_PATH(name, must_exist, call)                                                  \
    int name                                                                               \
    {                                                                                      \
        char host[VFS_PATH_MAX];                                                           \
        if (resolve(path, host, must_exist) < 0)                                           \
            return -1;                                                                     \
        return (call) < 0 ? fail_path(#call, path, errno) : 0;                             \
    }

WRAP_PATH(b_mkdir(const char *path, uint32_t mode), 0, mkdir(host, mode))
WRAP_PATH(b_rmdir(const char *path), 1, rmdir(host))
WRAP_PATH(b_unlink(const char *path), 1, unlink(host))
WRAP_PATH(b_chdir(const char *path), 1, chdir(host))

int b_truncate(const char *path, int32_t size)
{
    char host[VFS_PATH_MAX];
    if (resolve(path, host, 1) < 0)
        return -1;
    int writer = writer_of(host);
    int r = writer >= 0 ? ftruncate(writer, size) : truncate(host, size);
    return r < 0 ? fail_path("truncate", path, errno) : 0;
}

int b_rename(const char *from, const char *to)
{
    char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
    if (resolve(from, a, 1) < 0 || resolve(to, b, 0) < 0)
        return -1;
    unlink(b); // Horizon will not replace an existing target, and POSIX rename does
    return rename(a, b) < 0 ? fail_path("rename", from, errno) : 0;
}
