// bionic C library surface that cannot bind straight to newlib: 32-bit time types,
// the __sF stream array, ctype tables, Linux constants, signals, and calls with no
// meaning on the console.  File and thread functions live in fs_bridge.c and
// pthread_bridge.c.
#include "port/bionic_fs.h"
#include "port/bionic_thread.h"
#include "port/so_loader.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define ENOSYS_L 38
#define EAFNOSUPPORT_L 97
#define ENOENT_L 2

// ---- backend hooks ---------------------------------------------------------
// mmap needs real pages under emulation and heap-backed regions on Horizon.
void *b_mem_map(size_t size, int prot);
int b_mem_unmap(void *addr, size_t size);
int b_mem_protect(void *addr, size_t size, int prot);
void b_sleep_ns(int64_t ns);

// ---- data symbols ----------------------------------------------------------

#define BIONIC_FILE_SIZE 84
char b_sF[BIONIC_FILE_SIZE * 3];
uint32_t b_stack_chk_guard = 0x00FF0A0Du; // NUL and newline defeat string overruns
uint32_t b_page_size = 4096;

// bionic's ctype symbols are pointers to tables with a leading EOF slot.
static char g_ctype_table[1 + 256];
static short g_tolower_table[1 + 256];
static short g_toupper_table[1 + 256];
const char *b_ctype_ = g_ctype_table;
const short *b_tolower_tab_ = g_tolower_table;
const short *b_toupper_tab_ = g_toupper_table;

void b_libc_init(void)
{
    for (int c = 0; c < 256; c++) {
        // Bits are shared by BSD and newlib: U=1 L=2 N=4 S=8 P=0x10 C=0x20 X=0x40 B=0x80
        char v = 0;
        if (c < 128) {
            if (isupper(c)) v |= 0x01;
            if (islower(c)) v |= 0x02;
            if (isdigit(c)) v |= 0x04;
            if (isspace(c)) v |= 0x08;
            if (ispunct(c)) v |= 0x10;
            if (iscntrl(c)) v |= 0x20;
            if (isxdigit(c)) v |= 0x40;
            if (c == ' ') v |= (char)0x80;
        }
        g_ctype_table[c + 1] = v;
        g_tolower_table[c + 1] = (short)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        g_toupper_table[c + 1] = (short)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    g_tolower_table[0] = g_toupper_table[0] = -1;
}

// ---- stdio: only the three standard streams differ ---------------------------

static FILE *F(FILE *f)
{
    char *p = (char *)f;
    if (p >= b_sF && p < b_sF + sizeof b_sF) {
        size_t i = (size_t)(p - b_sF) / BIONIC_FILE_SIZE;
        return i == 0 ? stdin : i == 1 ? stdout : stderr;
    }
    return f;
}

int b_fclose(FILE *f)
{
    b_fs_forget(fileno(F(f)));
    return fclose(F(f));
}
int b_fflush(FILE *f) { return fflush(f ? F(f) : NULL); }
size_t b_fread(void *p, size_t s, size_t n, FILE *f) { return fread(p, s, n, F(f)); }
size_t b_fwrite(const void *p, size_t s, size_t n, FILE *f) { return fwrite(p, s, n, F(f)); }
int b_fputs(const char *s, FILE *f) { return fputs(s, F(f)); }
int b_fputc(int c, FILE *f) { return fputc(c, F(f)); }
char *b_fgets(char *s, int n, FILE *f) { return fgets(s, n, F(f)); }
int b_fseek(FILE *f, long o, int w) { return fseek(F(f), o, w); }
long b_ftell(FILE *f) { return ftell(F(f)); }
int b_getc(FILE *f) { return getc(F(f)); }
int b_putc(int c, FILE *f) { return putc(c, F(f)); }
int b_ungetc(int c, FILE *f) { return ungetc(c, F(f)); }
void b_clearerr(FILE *f) { clearerr(F(f)); }
int b_setvbuf(FILE *f, char *b, int m, size_t s) { return setvbuf(F(f), b, m, s); }
wint_t b_getwc(FILE *f) { return getwc(F(f)); }
wint_t b_putwc(wchar_t c, FILE *f) { return putwc(c, F(f)); }
wint_t b_ungetwc(wint_t c, FILE *f) { return ungetwc(c, F(f)); }

int b_vfprintf(FILE *f, const char *fmt, va_list ap) { return vfprintf(F(f), fmt, ap); }

int b_fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(F(f), fmt, ap);
    va_end(ap);
    return r;
}

int b_fscanf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(F(f), fmt, ap);
    va_end(ap);
    return r;
}

// ---- time: bionic time_t and tv_sec are 32-bit -------------------------------

struct b_timeval {
    int32_t tv_sec, tv_usec;
};

int32_t b_time(int32_t *out)
{
    int32_t t = (int32_t)(b_realtime_ns() / 1000000000);
    if (out)
        *out = t;
    return t;
}

int b_gettimeofday(struct b_timeval *tv, void *tz)
{
    (void)tz;
    int64_t ns = b_realtime_ns();
    tv->tv_sec = (int32_t)(ns / 1000000000);
    tv->tv_usec = (int32_t)((ns % 1000000000) / 1000);
    return 0;
}

int b_clock_gettime(int clk, struct b_timespec *ts)
{
    int64_t ns = clk == 0 ? b_realtime_ns() : b_monotonic_ns();
    ts->tv_sec = (int32_t)(ns / 1000000000);
    ts->tv_nsec = (int32_t)(ns % 1000000000);
    return 0;
}

int b_clock_getres(int clk, struct b_timespec *ts)
{
    (void)clk;
    if (ts) {
        ts->tv_sec = 0;
        ts->tv_nsec = 1;
    }
    return 0;
}

long b_clock(void)
{
    return (long)(b_monotonic_ns() / 1000);
}

int b_nanosleep(const struct b_timespec *req, struct b_timespec *rem)
{
    b_sleep_ns((int64_t)req->tv_sec * 1000000000 + req->tv_nsec);
    if (rem)
        rem->tv_sec = rem->tv_nsec = 0;
    return 0;
}

int b_usleep(uint32_t us) { b_sleep_ns((int64_t)us * 1000); return 0; }
uint32_t b_sleep(uint32_t s) { b_sleep_ns((int64_t)s * 1000000000); return 0; }

struct tm *b_gmtime(const int32_t *t)
{
    time_t v = *t;
    return gmtime(&v);
}

struct tm *b_gmtime_r(const int32_t *t, struct tm *out)
{
    time_t v = *t;
    return gmtime_r(&v, out);
}

struct tm *b_localtime(const int32_t *t)
{
    time_t v = *t;
    return localtime(&v);
}

struct tm *b_localtime_r(const int32_t *t, struct tm *out)
{
    time_t v = *t;
    return localtime_r(&v, out);
}

int32_t b_mktime(struct tm *t) { return (int32_t)mktime(t); }
double b_difftime(int32_t a, int32_t b) { return (double)a - (double)b; }

// ---- memory ----------------------------------------------------------------

#define PROT_EXEC_L 4
#define MAP_FIXED_L 0x10
#define MAP_FAILED_P ((void *)-1)

void *b_mmap(void *addr, uint32_t len, int prot, int flags, int fd, int32_t offset)
{
    if ((flags & MAP_FIXED_L) && addr) {
        // Boehm remaps heap blocks inaccessible to release them.  Keeping the pages is
        // safe: it only ever maps them back through mprotect or another fixed mapping.
        if (prot == 0)
            return addr;
        *b_errno() = 22;
        return MAP_FAILED_P;
    }
    // A read-only file mapping still has to be filled, so it starts writable.
    void *p = b_mem_map(len, fd >= 0 ? prot | 3 : prot);
    if (!p) {
        *b_errno() = 12;
        return MAP_FAILED_P;
    }
    if (fd >= 0) {
        // No demand paging: file mappings are read up front.
        off_t saved = lseek(fd, 0, SEEK_CUR);
        lseek(fd, offset, SEEK_SET);
        ssize_t got = read(fd, p, len);
        lseek(fd, saved, SEEK_SET);
        if (got < 0) {
            b_mem_unmap(p, len);
            *b_errno() = 5;
            return MAP_FAILED_P;
        }
        if (b_mem_protect(p, len, prot) != 0) {
            b_mem_unmap(p, len);
            *b_errno() = 12;
            return MAP_FAILED_P;
        }
    }
    return p;
}

int b_munmap(void *addr, uint32_t len) { return b_mem_unmap(addr, len); }
int b_mprotect(void *addr, uint32_t len, int prot) { return b_mem_protect(addr, len, prot); }
int b_madvise(void *a, uint32_t l, int adv) { (void)a; (void)l; (void)adv; return 0; }

// ---- sysconf ---------------------------------------------------------------

long b_sysconf(int name)
{
    switch (name) {
    case 0x06: return 100;        // _SC_CLK_TCK
    case 0x27:
    case 0x28: return 4096;       // _SC_PAGESIZE, _SC_PAGE_SIZE
    case 0x60:
    case 0x61: return 3;          // _SC_NPROCESSORS_CONF, _SC_NPROCESSORS_ONLN
    case 0x62: return 0xC0000;    // _SC_PHYS_PAGES: 3 GB
    case 0x63: return 0x80000;    // _SC_AVPHYS_PAGES: 2 GB
    case 0x04: return 1024;       // _SC_OPEN_MAX
    default:
        *b_errno() = 22;
        return -1;
    }
}

// ---- signals ---------------------------------------------------------------
// Horizon has none.  Handlers are recorded so the fault path can dispatch them.

struct b_sigaction {
    void (*handler)(int);
    uint32_t mask;
    uint32_t flags;
    void (*restorer)(void);
};
_Static_assert(sizeof(struct b_sigaction) == 16, "bionic sigaction size");

#define SIG_MAX 65
struct b_sigaction g_sigactions[SIG_MAX];

__attribute__((weak)) void b_sigaction_notify(int sig, const struct b_sigaction *act)
{
    (void)sig;
    (void)act;
}

int b_sigaction(int sig, const struct b_sigaction *act, struct b_sigaction *old)
{
    if (sig <= 0 || sig >= SIG_MAX) {
        *b_errno() = 22;
        return -1;
    }
    if (old)
        *old = g_sigactions[sig];
    if (act) {
        g_sigactions[sig] = *act;
        b_sigaction_notify(sig, act);
    }
    return 0;
}

void *b_bsd_signal(int sig, void (*handler)(int))
{
    if (sig <= 0 || sig >= SIG_MAX)
        return (void *)-1;
    void *old = (void *)g_sigactions[sig].handler;
    g_sigactions[sig].handler = handler;
    return old;
}

int b_sigprocmask(int how, const void *set, void *old) { return b_pthread_sigmask(how, set, old); }

// ---- C++ runtime -----------------------------------------------------------

int b_cxa_atexit(void *fn, void *arg, void *dso)
{
    (void)fn;
    (void)arg;
    (void)dso;
    return 0; // the game never exits through libc, so destructors never run
}

void b_cxa_finalize(void *dso) { (void)dso; }

static b_mutex_t g_guard_lock = {B_MUTEX_RECURSIVE_INIT};

int b_cxa_guard_acquire(uint32_t *guard)
{
    b_mutex_lock(&g_guard_lock);
    if (*guard & 1) {
        b_mutex_unlock(&g_guard_lock);
        return 0;
    }
    return 1; // lock stays held until release: other threads wait for the constructor
}

void b_cxa_guard_release(uint32_t *guard)
{
    *guard = 1;
    b_mutex_unlock(&g_guard_lock);
}

void b_cxa_guard_abort(uint32_t *guard)
{
    (void)guard;
    b_mutex_unlock(&g_guard_lock);
}

void b_cxa_pure_virtual(void)
{
    fprintf(stderr, "pure virtual call\n");
    abort();
}

void b_stack_chk_fail(void)
{
    fprintf(stderr, "stack corruption detected\n");
    abort();
}

// ---- Android and process -----------------------------------------------------

int b_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    fprintf(stderr, "[%d %s] ", prio, tag ? tag : "");
    int r = vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    return r;
}

int b_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = b_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

int b_log_write(int prio, const char *tag, const char *text)
{
    fprintf(stderr, "[%d %s] %s\n", prio, tag ? tag : "", text);
    return 0;
}

int b_system_property_get(const char *name, char *value)
{
    (void)name;
    value[0] = '\0';
    return 0;
}

int b_fpclassifyf(float f)
{
    switch (fpclassify(f)) {
    case FP_INFINITE: return 0x01;
    case FP_NAN: return 0x02;
    case FP_NORMAL: return 0x04;
    case FP_SUBNORMAL: return 0x08;
    default: return 0x10;
    }
}

const char *b_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

int b_strerror_r(int e, char *buf, size_t n)
{
    snprintf(buf, n, "error %d", e);
    return 0;
}

char *b_getcwd(char *buf, size_t size)
{
    static const char cwd[] = "/data/data/com.activision.callofduty.striketeam";
    if (!buf) {
        buf = malloc(sizeof cwd);
        if (!buf)
            return NULL;
    } else if (size < sizeof cwd) {
        *b_errno() = 34;
        return NULL;
    }
    memcpy(buf, cwd, sizeof cwd);
    return buf;
}

int b_getpid(void) { return 1000; }
int b_gettid(void) { return 1000 + (int)b_thread_id(); }
int b_id_zero(void) { return 0; }
int b_getdtablesize(void) { return 1024; }

int b_uname(char *buf)
{
    // Six 65-byte fields, as in struct utsname.
    static const char *const fields[6] = {"Linux", "switch", "3.4.0", "#1", "armv7l", ""};
    memset(buf, 0, 65 * 6);
    for (int i = 0; i < 6; i++)
        strcpy(buf + i * 65, fields[i]);
    return 0;
}

int b_gethostname(char *name, size_t size)
{
    snprintf(name, size, "switch");
    return 0;
}

int b_getrusage(int who, void *out)
{
    (void)who;
    memset(out, 0, 72);
    return 0;
}

#define F_GETFL_L 3
#define F_GETLK_L 5
#define F_GETLK64_L 12
#define F_UNLCK_L 2
#define O_RDWR_L 2

// One process owns every file, so locks never conflict.  SQLite reads l_type back after
// F_GETLK: leaving it unset would make every database look locked.  The other commands
// only flip FD_CLOEXEC and O_NONBLOCK, which mean nothing here.
int b_fcntl(int fd, int cmd, ...)
{
    (void)fd;
    if (cmd == F_GETLK_L || cmd == F_GETLK64_L) {
        va_list ap;
        va_start(ap, cmd);
        short *flock = va_arg(ap, short *); // struct flock starts with short l_type
        va_end(ap);
        *flock = F_UNLCK_L;
        return 0;
    }
    return cmd == F_GETFL_L ? O_RDWR_L : 0;
}
int b_flock(int fd, int op) { (void)fd; (void)op; return 0; }
// Horizon holds written data until a flush, so a crash could lose a database's commits.
int b_fsync(int fd)
{
    fsync(fd);
    return 0;
}
int b_ftruncate(int fd, int32_t size)
{
    if (ftruncate(fd, size) == 0)
        return 0;
    fprintf(stderr, "fs: ftruncate fd %d to %d failed, errno %d\n", fd, (int)size, errno);
    *b_errno() = b_errno_from_native(errno);
    return -1;
}
int b_isatty(int fd) { (void)fd; return 0; }
int b_chmod(const char *p, uint32_t m) { (void)p; (void)m; return 0; }
int b_fchmod(int f, uint32_t m) { (void)f; (void)m; return 0; }
int b_fchown(int f, int u, int g) { (void)f; (void)u; (void)g; return 0; }
int b_utime(const char *p, const void *t) { (void)p; (void)t; return 0; }
int b_remove(const char *p) { return b_unlink(p); }

int b_readlink(const char *p, char *b, size_t n)
{
    (void)p;
    (void)b;
    (void)n;
    *b_errno() = ENOENT_L;
    return -1;
}

int b_syscall(int n, ...)
{
    (void)n; // only inotify_init/add_watch/rm_watch reach here; Mono handles ENOSYS
    *b_errno() = ENOSYS_L;
    return -1;
}

void b_exit(int code)
{
    fflush(NULL);
    fprintf(stderr, "guest exit(%d)\n", code);
    exit(code);
}

// Timed sleep is the only behaviour callers can rely on with no descriptors ready.
int b_select(int n, void *r, void *w, void *e, struct b_timeval *tv)
{
    (void)n;
    (void)r;
    (void)w;
    (void)e;
    if (tv)
        b_sleep_ns((int64_t)tv->tv_sec * 1000000000 + (int64_t)tv->tv_usec * 1000);
    return 0;
}

int b_poll(void *fds, uint32_t n, int timeout_ms)
{
    (void)fds;
    (void)n;
    if (timeout_ms > 0)
        b_sleep_ns((int64_t)timeout_ms * 1000000);
    return 0;
}

// There is one window, and input reaches Unity through natives, not an Android queue.
void *b_window_from_surface(void *env, void *surface)
{
    (void)env;
    (void)surface;
    return (void *)1;
}

// ---- deliberately absent -----------------------------------------------------
// No processes, and the servers this game talked to are not part of the port.

int b_enosys(void)
{
    *b_errno() = ENOSYS_L;
    return -1;
}

int b_no_network(void)
{
    *b_errno() = EAFNOSUPPORT_L;
    return -1;
}

void *b_null(void)
{
    return NULL;
}

int b_getaddrinfo(const char *n, const char *s, const void *h, void **res)
{
    (void)n;
    (void)s;
    (void)h;
    if (res)
        *res = NULL;
    return 4; // EAI_FAIL
}

const char *b_gai_strerror(int e)
{
    (void)e;
    return "network is disabled";
}

uint32_t b_inet_addr(const char *s) { (void)s; return 0xFFFFFFFFu; }
