// Binding table: Android symbol name -> host implementation.
// P = same ABI as newlib, bind directly.  M = bind to a bridge.  Symbols absent from
// here fall through to other game libraries, then to a named thunk.
#include "port/bionic_fs.h"
#include "port/bionic_thread.h"
#include "port/gl_shim.h"
#include "port/so_loader.h"

#include <ctype.h>
#include <fnmatch.h>
#include <locale.h>
#include <malloc.h>
#include <math.h>
#include <miniz/miniz.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#define DECL(n) extern char n[]
DECL(b_fclose); DECL(b_fflush); DECL(b_fread); DECL(b_fwrite); DECL(b_fputs); DECL(b_fputc); DECL(b_fgets);
DECL(b_fseek); DECL(b_ftell); DECL(b_getc); DECL(b_putc); DECL(b_ungetc); DECL(b_clearerr); DECL(b_setvbuf);
DECL(b_getwc); DECL(b_putwc); DECL(b_ungetwc); DECL(b_vfprintf); DECL(b_fprintf); DECL(b_fscanf);
DECL(b_time); DECL(b_gettimeofday); DECL(b_clock_gettime); DECL(b_clock_getres); DECL(b_clock);
DECL(b_nanosleep); DECL(b_usleep); DECL(b_sleep); DECL(b_gmtime); DECL(b_gmtime_r); DECL(b_localtime);
DECL(b_localtime_r); DECL(b_mktime); DECL(b_difftime); DECL(b_mmap); DECL(b_munmap); DECL(b_mprotect);
DECL(b_madvise); DECL(b_sysconf); DECL(b_sigaction); DECL(b_bsd_signal); DECL(b_sigprocmask);
DECL(b_cxa_atexit); DECL(b_cxa_finalize); DECL(b_cxa_guard_acquire); DECL(b_cxa_guard_release);
DECL(b_cxa_pure_virtual); DECL(b_stack_chk_fail); DECL(b_log_vprint); DECL(b_log_print); DECL(b_log_write);
DECL(b_system_property_get); DECL(b_fpclassifyf); DECL(b_basename); DECL(b_strerror_r); DECL(b_getcwd);
DECL(b_getpid); DECL(b_gettid); DECL(b_id_zero); DECL(b_getdtablesize); DECL(b_uname); DECL(b_gethostname);
DECL(b_getrusage); DECL(b_fcntl); DECL(b_flock); DECL(b_fsync); DECL(b_ftruncate); DECL(b_isatty);
DECL(b_chmod); DECL(b_fchmod); DECL(b_fchown); DECL(b_utime); DECL(b_remove); DECL(b_readlink); DECL(b_syscall);
DECL(b_exit); DECL(b_select); DECL(b_poll); DECL(b_enosys); DECL(b_no_network); DECL(b_null);
DECL(b_getaddrinfo); DECL(b_gai_strerror); DECL(b_inet_addr); DECL(b_setjmp); DECL(b_longjmp);
DECL(b_SHA1Init); DECL(b_SHA1Update); DECL(b_SHA1Final);
DECL(b_sF); DECL(b_stack_chk_guard); DECL(b_page_size); DECL(b_ctype_); DECL(b_tolower_tab_);
DECL(b_toupper_tab_);

void gl_unimplemented(void);
DECL(b_window_from_surface);
DECL(b_input_queue); DECL(b_looper_poll); DECL(b_queue_get_event); DECL(b_queue_pre_dispatch); DECL(b_queue_finish);
DECL(b_queue_attach); DECL(b_queue_detach); DECL(b_watchdog); DECL(b_forward_to_dalvik);
DECL(b_event_type); DECL(b_event_source); DECL(b_event_device); DECL(b_key_code); DECL(b_key_meta); DECL(b_key_action);
DECL(b_motion_action); DECL(b_motion_time); DECL(b_motion_count); DECL(b_motion_pointer_id); DECL(b_motion_x);
DECL(b_motion_y); DECL(b_motion_history); DECL(b_motion_hist); DECL(b_motion_hist_time); DECL(b_get_axis_value);

int port_inflate(mz_streamp s, int flush);
int port_deflateInit2_(mz_streamp s, int level, int method, int bits, int mem, int strategy, const char *v, int sz);
int port_inflateInit2_(mz_streamp s, int bits, const char *v, int sz);
int port_inflateInit_(mz_streamp s, const char *v, int sz);

struct import {
    const char *name;
    void *fn;
};

#define P(n) {#n, (void *)n}
#define M(n, f) {#n, (void *)f}

// The game's heap calls go through src/bionic/heap_guard.c (on the console and in the harness).
#if defined(__SWITCH__) || defined(PORT_QEMU)
void *hg_malloc(size_t n);
void *hg_calloc(size_t n, size_t m);
void *hg_memalign(size_t align, size_t n);
void *hg_realloc(void *p, size_t n);
void hg_free(void *p);
#define HEAP(n) {#n, (void *)hg_##n}
#else
#define HEAP(n) P(n)
#endif

static struct import g_table[] = {
    // Direct: memory, strings, conversion.
    P(memchr), P(memcmp), P(memcpy), P(memmem), P(memmove), P(memset), P(strcasecmp), P(strcasestr), P(strcat),
    P(strchr), P(strcmp), P(strcoll), P(strcpy), P(strcspn), P(strdup), P(strlcat), P(strlcpy), P(strlen),
    P(strncasecmp), P(strncat), P(strncmp), P(strncpy), P(strpbrk), P(strrchr), P(strsep), P(strspn), P(strstr),
    P(strtod), P(strtok), P(strtok_r), P(strtol), P(strtoll), P(strtoul), P(strtoull), P(strxfrm), P(strerror),
    P(atoi), P(atol), P(bsearch), P(qsort), P(div), P(lrand48), P(srand48),
    P(isalnum), P(isalpha), P(isspace), P(isxdigit), P(tolower), P(towlower), P(towupper),
    // Direct: heap and environment.
    HEAP(calloc), HEAP(free), HEAP(malloc), HEAP(memalign), HEAP(realloc), P(getenv), P(setenv), P(unsetenv), M(environ, &environ),
    // Direct: formatted output into buffers.
    P(printf), P(putchar), P(puts), P(snprintf), P(sprintf), P(sscanf), P(vasprintf), P(vprintf), P(vsnprintf),
    P(vsprintf), P(perror), P(setlocale), P(strftime), P(fnmatch),
    // Direct: wide characters.
    P(btowc), P(iswctype), P(mbrtowc), P(wcrtomb), P(wcscoll), P(wcsftime), P(wcslen), P(wcsxfrm), P(wctob),
    P(wctype), P(wmemchr), P(wmemcmp), P(wmemcpy), P(wmemmove), P(wmemset),
    // Direct: math.  softfp matches the libraries' calling convention.
    P(acos), P(acosf), P(asin), P(asinf), P(atan), P(atan2), P(atan2f), P(atanf), P(ceil), P(ceilf), P(cos),
    P(cosf), P(cosh), P(exp), P(expf), P(floor), P(floorf), P(fmod), P(fmodf), P(frexp), P(ldexp), P(log),
    P(log10), P(logf), P(modff), P(pow), P(powf), P(rint), P(round), P(sin), P(sinf), P(sinh), P(sqrt), P(sqrtf),
    P(tan), P(tanf), P(tanh), P(trunc),
    // zlib: miniz's stream struct has the same layout.
    M(crc32, mz_crc32), M(deflate, mz_deflate), M(deflateEnd, mz_deflateEnd), M(deflateReset, mz_deflateReset),
    M(inflate, port_inflate), M(inflateEnd, mz_inflateEnd), M(inflateReset, mz_inflateReset),
    M(deflateInit2_, port_deflateInit2_), M(inflateInit2_, port_inflateInit2_), M(inflateInit_, port_inflateInit_),
    // Data symbols.
    M(__sF, b_sF), M(__stack_chk_guard, b_stack_chk_guard), M(__page_size, b_page_size), M(_ctype_, b_ctype_),
    M(_tolower_tab_, b_tolower_tab_), M(_toupper_tab_, b_toupper_tab_),
    // Files.
    M(open, b_open), M(close, b_close), M(read, b_read), M(write, b_write), M(lseek, b_lseek), M(fopen, b_fopen),
    M(stat, b_stat), M(lstat, b_lstat), M(fstat, b_fstat), M(opendir, b_opendir), M(readdir, b_readdir),
    M(closedir, b_closedir), M(mkdir, b_mkdir), M(rmdir, b_rmdir), M(unlink, b_unlink), M(rename, b_rename),
    M(truncate, b_truncate), M(chdir, b_chdir), M(remove, b_remove), M(__errno, b_errno),
    M(fsync, b_fsync), M(ftruncate, b_ftruncate), M(fcntl, b_fcntl), M(flock, b_flock), M(isatty, b_isatty),
    M(chmod, b_chmod), M(fchmod, b_fchmod), M(fchown, b_fchown), M(utime, b_utime), M(utimes, b_utime),
    M(readlink, b_readlink), M(getcwd, b_getcwd), M(basename, b_basename),
    // Streams.
    M(fclose, b_fclose), M(fflush, b_fflush), M(fread, b_fread), M(fwrite, b_fwrite), M(fputs, b_fputs),
    M(fputc, b_fputc), M(fgets, b_fgets), M(fseek, b_fseek), M(ftell, b_ftell), M(getc, b_getc), M(putc, b_putc),
    M(ungetc, b_ungetc), M(clearerr, b_clearerr), M(setvbuf, b_setvbuf), M(getwc, b_getwc), M(putwc, b_putwc),
    M(ungetwc, b_ungetwc), M(vfprintf, b_vfprintf), M(fprintf, b_fprintf), M(fscanf, b_fscanf),
    // Time.
    M(time, b_time), M(gettimeofday, b_gettimeofday), M(clock_gettime, b_clock_gettime),
    M(clock_getres, b_clock_getres), M(clock, b_clock), M(nanosleep, b_nanosleep), M(usleep, b_usleep),
    M(sleep, b_sleep), M(gmtime, b_gmtime), M(gmtime_r, b_gmtime_r), M(localtime, b_localtime),
    M(localtime_r, b_localtime_r), M(mktime, b_mktime), M(difftime, b_difftime),
    // Memory, signals, control flow.
    M(mmap, b_mmap), M(munmap, b_munmap), M(mprotect, b_mprotect), M(madvise, b_madvise), M(sysconf, b_sysconf),
    M(sigaction, b_sigaction), M(bsd_signal, b_bsd_signal), M(sigprocmask, b_sigprocmask),
    M(setjmp, b_setjmp), M(longjmp, b_longjmp), M(sigsetjmp, b_setjmp), M(siglongjmp, b_longjmp),
    // Threads.
    M(pthread_attr_destroy, b_attr_destroy), M(pthread_attr_getdetachstate, b_attr_getdetachstate),
    M(pthread_attr_getstack, b_attr_getstack), M(pthread_attr_init, b_attr_init),
    M(pthread_attr_setdetachstate, b_attr_setdetachstate), M(pthread_attr_setschedparam, b_attr_setschedparam),
    M(pthread_attr_setschedpolicy, b_attr_setschedpolicy), M(pthread_attr_setstacksize, b_attr_setstacksize),
    M(pthread_cond_broadcast, b_cond_broadcast), M(pthread_cond_destroy, b_cond_destroy),
    M(pthread_cond_init, b_cond_init), M(pthread_cond_signal, b_cond_signal),
    M(pthread_cond_timedwait, b_cond_timedwait), M(pthread_cond_wait, b_cond_wait),
    M(pthread_create, b_pthread_create), M(pthread_detach, b_pthread_detach), M(pthread_equal, b_pthread_equal),
    M(pthread_exit, b_pthread_exit), M(pthread_getattr_np, b_pthread_getattr_np),
    M(pthread_getschedparam, b_pthread_getschedparam), M(pthread_getspecific, b_getspecific),
    M(pthread_join, b_pthread_join), M(pthread_key_create, b_key_create), M(pthread_key_delete, b_key_delete),
    M(pthread_kill, b_pthread_kill), M(pthread_mutex_destroy, b_mutex_destroy), M(pthread_mutex_init, b_mutex_init),
    M(pthread_mutex_lock, b_mutex_lock), M(pthread_mutex_trylock, b_mutex_trylock),
    M(pthread_mutex_unlock, b_mutex_unlock), M(pthread_mutexattr_destroy, b_mutexattr_destroy),
    M(pthread_mutexattr_init, b_mutexattr_init), M(pthread_mutexattr_settype, b_mutexattr_settype),
    M(pthread_once, b_once), M(pthread_self, b_pthread_self), M(pthread_setschedparam, b_pthread_setschedparam),
    M(pthread_setspecific, b_setspecific), M(pthread_sigmask, b_pthread_sigmask),
    M(__pthread_cleanup_push, b_cleanup_push), M(__pthread_cleanup_pop, b_cleanup_pop), M(sched_yield, b_sched_yield),
    M(sem_destroy, b_sem_destroy), M(sem_getvalue, b_sem_getvalue), M(sem_init, b_sem_init), M(sem_post, b_sem_post),
    M(sem_timedwait, b_sem_timedwait), M(sem_trywait, b_sem_trywait), M(sem_wait, b_sem_wait),
    // Loader services.
    M(dlopen, b_dlopen), M(dlsym, b_dlsym), M(dlclose, b_dlclose), M(dlerror, b_dlerror),
    M(__gnu_Unwind_Find_exidx, b_unwind_find_exidx),
    // C++ runtime and hardening.
    M(__cxa_atexit, b_cxa_atexit), M(__aeabi_atexit, b_cxa_atexit), M(__cxa_finalize, b_cxa_finalize),
    M(__cxa_guard_acquire, b_cxa_guard_acquire), M(__cxa_guard_release, b_cxa_guard_release),
    M(__cxa_pure_virtual, b_cxa_pure_virtual), M(__stack_chk_fail, b_stack_chk_fail),
    // Android and process identity.
    M(__android_log_print, b_log_print), M(__android_log_vprint, b_log_vprint), M(__android_log_write, b_log_write),
    M(__system_property_get, b_system_property_get), M(__fpclassifyf, b_fpclassifyf), M(strerror_r, b_strerror_r),
    M(SHA1Init, b_SHA1Init), M(SHA1Update, b_SHA1Update), M(SHA1Final, b_SHA1Final),
    M(getpid, b_getpid), M(gettid, b_gettid), M(getuid, b_id_zero), M(geteuid, b_id_zero), M(getgid, b_id_zero),
    M(getegid, b_id_zero), M(getpriority, b_id_zero), M(setpriority, b_id_zero), M(prctl, b_id_zero),
    M(setitimer, b_id_zero), M(setresuid, b_id_zero), M(getresuid, b_id_zero), M(getdtablesize, b_getdtablesize),
    M(uname, b_uname), M(gethostname, b_gethostname), M(getrusage, b_getrusage), M(syscall, b_syscall),
    M(exit, b_exit), M(_exit, b_exit), M(openlog, b_id_zero), M(closelog, b_id_zero), M(syslog, b_id_zero),
    M(select, b_select), M(poll, b_poll),
    M(ANativeWindow_fromSurface, b_window_from_surface), M(ANativeWindow_setBuffersGeometry, b_id_zero),
    M(ALooper_prepare, b_null), M(ALooper_forThread, b_null), M(ALooper_pollAll, b_looper_poll),
    M(_Z29ANativeActivity_getInputQueuev, b_input_queue), M(_Z28ANativeActivity_feedWatchdogd, b_watchdog),
    M(_Z42ANativeActivity_forwardMotionEventToDalvikP11AInputEventP7_JNIEnv, b_forward_to_dalvik),
    M(AInputQueue_getEvent, b_queue_get_event), M(AInputQueue_preDispatchEvent, b_queue_pre_dispatch),
    M(AInputQueue_finishEvent, b_queue_finish), M(AInputQueue_attachLooper, b_queue_attach),
    M(AInputQueue_detachLooper, b_queue_detach), M(AInputEvent_getType, b_event_type),
    M(AInputEvent_getSource, b_event_source), M(AInputEvent_getDeviceId, b_event_device),
    M(AKeyEvent_getKeyCode, b_key_code), M(AKeyEvent_getMetaState, b_key_meta), M(AKeyEvent_getAction, b_key_action),
    M(AMotionEvent_getAction, b_motion_action), M(AMotionEvent_getEventTime, b_motion_time),
    M(AMotionEvent_getPointerCount, b_motion_count), M(AMotionEvent_getPointerId, b_motion_pointer_id),
    M(AMotionEvent_getX, b_motion_x), M(AMotionEvent_getY, b_motion_y), M(AMotionEvent_getHistorySize, b_motion_history),
    M(AMotionEvent_getHistoricalX, b_motion_hist), M(AMotionEvent_getHistoricalY, b_motion_hist),
    M(AMotionEvent_getHistoricalEventTime, b_motion_hist_time), M(_MotionEvent_getAxisValue, b_get_axis_value),
    M(ASensorManager_getInstance, b_null), M(ASensorManager_getSensorList, b_id_zero),
    M(ASensorManager_getDefaultSensor, b_null), M(ASensorManager_createEventQueue, b_null),
    P(abort), M(atexit, b_id_zero), M(access, b_access), M(sched_get_priority_min, b_id_zero),
    M(sched_get_priority_max, b_id_zero),
    // Absent by design: processes.
    M(fork, b_enosys), M(execl, b_enosys), M(execv, b_enosys), M(execve, b_enosys), M(waitpid, b_enosys),
    M(system, b_enosys), M(popen, b_null), M(pclose, b_enosys), M(ptrace, b_enosys), M(pipe, b_enosys),
    M(kill, b_enosys), M(tkill, b_tkill), M(tgkill, b_enosys), M(raise, b_enosys), M(sigsuspend, b_enosys),
    M(mremap, b_enosys), M(ioctl, b_enosys), M(dup2, b_enosys), M(mkstemp, b_enosys), M(sendfile, b_enosys),
    M(epoll_create, b_enosys), M(epoll_ctl, b_enosys), M(epoll_wait, b_enosys), M(fdopen, b_null),
    M(getpwnam, b_null), M(getpwuid, b_null), M(getgrgid, b_null), M(getgrnam, b_null),
    // Absent by design: network.
    M(socket, b_no_network), M(accept, b_no_network), M(bind, b_no_network), M(connect, b_no_network),
    M(listen, b_no_network), M(recv, b_no_network), M(recvfrom, b_no_network), M(recvmsg, b_no_network),
    M(send, b_no_network), M(sendmsg, b_no_network), M(sendto, b_no_network), M(setsockopt, b_no_network),
    M(getsockopt, b_no_network), M(getsockname, b_no_network), M(getpeername, b_no_network),
    M(shutdown, b_no_network), M(writev, b_enosys), M(getaddrinfo, b_getaddrinfo), M(freeaddrinfo, b_id_zero),
    M(gai_strerror, b_gai_strerror), M(gethostbyname, b_null), M(gethostbyaddr, b_null), M(getservbyname, b_null),
    M(inet_addr, b_inet_addr), M(inet_aton, b_id_zero), M(inet_ntoa, b_null), M(inet_ntop, b_null),
    M(inet_pton, b_id_zero), M(__get_h_errno, b_errno),
};

#define TABLE_SIZE (sizeof g_table / sizeof *g_table)

static int g_sorted;

static int by_name(const void *a, const void *b)
{
    return strcmp(((const struct import *)a)->name, ((const struct import *)b)->name);
}

void *so_import_lookup(const char *name)
{
    if (!g_sorted) {
        qsort(g_table, TABLE_SIZE, sizeof *g_table, by_name);
        g_sorted = 1;
    }
    struct import key = {name, NULL};
    const struct import *e = bsearch(&key, g_table, TABLE_SIZE, sizeof *g_table, by_name);
    if (e)
        return e->fn;
    // A shim still on its weak stub reports as unresolved, so its call stops by name.
    if (strncmp(name, "gl", 2) == 0 || strncmp(name, "egl", 3) == 0)
        for (unsigned i = 0; i < gl_import_count; i++)
            if (strcmp(gl_imports[i].name, name) == 0)
                return gl_imports[i].fn == gl_unimplemented ? NULL : (void *)gl_imports[i].fn;
    return NULL;
}

// Makes miniz behave like zlib for callers that watch the input side.
//
// miniz decodes eagerly: it can consume all input while decoded bytes still wait inside
// it.  zlib stops consuming once the output is full.  Unity's zip reader treats "no
// input left" as end of file, so with miniz it lost the tail of deflated files.  Holding
// back the last input byte until pending output is delivered restores zlib's contract.
//
// libpng asks for Z_PARTIAL_FLUSH, which miniz refuses.  For inflate it gives the same
// output as Z_SYNC_FLUSH.
int port_inflate(mz_streamp s, int flush)
{
    int mode = flush == MZ_PARTIAL_FLUSH ? MZ_SYNC_FLUSH : flush;
    unsigned start_in = s->avail_in, start_out = s->avail_out;
    int r;
    for (;;) {
        unsigned out_before = s->avail_out, in_before = s->avail_in;
        if (s->avail_in > 1) {
            s->avail_in--;
            r = mz_inflate(s, mode);
            s->avail_in++;
        } else if (s->avail_in == 1) {
            s->avail_in = 0; // deliver what is pending before giving up the last byte
            r = mz_inflate(s, mode);
            s->avail_in = 1;
            if (r == MZ_BUF_ERROR)
                r = MZ_OK;
            if (r == MZ_OK && s->avail_out > 0 && s->avail_out == out_before)
                r = mz_inflate(s, mode);
        } else {
            r = mz_inflate(s, mode);
        }
        if (r != MZ_OK || s->avail_out == 0)
            break;
        // miniz can return early after copying out pending bytes.
        if (s->avail_out == out_before && s->avail_in == in_before)
            break;
    }
    // As in zlib, this error only means the whole call achieved nothing.
    if (r == MZ_BUF_ERROR && (s->avail_in != start_in || s->avail_out != start_out))
        r = MZ_OK;
    if (r < 0 && r != MZ_BUF_ERROR)
        fprintf(stderr, "inflate: error %d (flush %d, %u bytes in, %u out)\n", r, flush, s->avail_in, s->avail_out);
    return r;
}

// Init entry points differ from miniz's: version and size arguments are dropped.
int port_deflateInit2_(mz_streamp s, int level, int method, int bits, int mem, int strategy, const char *v, int sz)
{
    (void)v;
    (void)sz;
    return mz_deflateInit2(s, level, method, bits, mem, strategy);
}

int port_inflateInit2_(mz_streamp s, int bits, const char *v, int sz)
{
    (void)v;
    (void)sz;
    return mz_inflateInit2(s, bits);
}

int port_inflateInit_(mz_streamp s, const char *v, int sz)
{
    (void)v;
    (void)sz;
    return mz_inflateInit(s);
}
