// Android bionic threading ABI on top of a futex primitive.
//
// libunity/libmono were compiled against bionic, where these objects are single
// words that callers allocate inline and zero-initialize statically.  newlib's
// types are larger, so a pass-through would corrupt the caller's memory.  The state
// therefore lives in the 4-byte word, using bionic's bit layout:
//   mutex: [31:16] owner, [15:14] type, [12:2] recursion count, [1:0] state
// Return values are Linux errno numbers because the caller compares against them.
#ifndef PORT_BIONIC_THREAD_H
#define PORT_BIONIC_THREAD_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define B_EPERM 1
#define B_ESRCH 3
#define B_EAGAIN 11
#define B_ENOMEM 12
#define B_EBUSY 16
#define B_EINVAL 22
#define B_EDEADLK 35
#define B_ENOSYS 38
#define B_ETIMEDOUT 110

#define B_MUTEX_NORMAL 0
#define B_MUTEX_RECURSIVE 1
#define B_MUTEX_ERRORCHECK 2
#define B_MUTEX_RECURSIVE_INIT 0x4000 // PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#define B_MUTEX_ERRORCHECK_INIT 0x8000

#define B_KEYS_MAX 128
#define B_PTHREAD_DETACHED 1

typedef struct { _Atomic uint32_t value; } b_mutex_t;
typedef struct { _Atomic uint32_t value; } b_cond_t;
typedef struct { _Atomic uint32_t count; } b_sem_t;
typedef _Atomic int32_t b_once_t;
typedef int32_t b_key_t;
typedef int32_t b_mutexattr_t;

typedef struct {
    uint32_t flags;
    void *stack_base;
    uint32_t stack_size;
    uint32_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
} b_attr_t;

struct b_timespec { // bionic time_t and long are both 32-bit
    int32_t tv_sec;
    int32_t tv_nsec;
};

struct b_sched_param {
    int32_t sched_priority;
};

struct b_cleanup {
    struct b_cleanup *prev;
    void (*routine)(void *);
    void *arg;
};

#if UINTPTR_MAX == 0xFFFFFFFFu
_Static_assert(sizeof(b_attr_t) == 24, "bionic pthread_attr_t size");
_Static_assert(sizeof(struct b_cleanup) == 12, "bionic cleanup size");
#endif
_Static_assert(sizeof(b_mutex_t) == 4, "bionic pthread_mutex_t size");
_Static_assert(sizeof(b_cond_t) == 4, "bionic pthread_cond_t size");
_Static_assert(sizeof(b_sem_t) == 4, "bionic sem_t size");
_Static_assert(sizeof(struct b_timespec) == 8, "bionic timespec size");

typedef struct b_thread *b_pthread_t;

// Futex layer: Horizon address arbitration, or Linux futex in host tests.
int futex_wait(_Atomic uint32_t *word, uint32_t expected, int64_t timeout_ns); // 0 or B_ETIMEDOUT
void futex_wake(_Atomic uint32_t *word, int count);
int64_t b_realtime_ns(void);
int64_t b_monotonic_ns(void);

void b_thread_init(void); // call once on the main thread before any guest code
uint32_t b_thread_id(void);

// Stop-the-world support (src/bionic/gc_bridge.c).  regs: r0-r12, sp, lr, pc.
b_pthread_t b_thread_by_id(uint32_t id);
void b_thread_pause(b_pthread_t t, uint32_t regs[16]);
int b_thread_has_handle(b_pthread_t t);
void b_thread_unpause(b_pthread_t t);
void *b_thread_blocked_sp(b_pthread_t t);
void b_blocking_enter(void *spill);
void b_blocking_exit(void);
int b_tkill(int id, int sig);

int b_mutex_init(b_mutex_t *m, const b_mutexattr_t *attr);
int b_mutex_destroy(b_mutex_t *m);
int b_mutex_lock(b_mutex_t *m);
int b_mutex_trylock(b_mutex_t *m);
int b_mutex_unlock(b_mutex_t *m);
int b_mutexattr_init(b_mutexattr_t *a);
int b_mutexattr_destroy(b_mutexattr_t *a);
int b_mutexattr_settype(b_mutexattr_t *a, int type);

int b_cond_init(b_cond_t *c, const void *attr);
int b_cond_destroy(b_cond_t *c);
int b_cond_signal(b_cond_t *c);
int b_cond_broadcast(b_cond_t *c);
int b_cond_wait(b_cond_t *c, b_mutex_t *m);
int b_cond_timedwait(b_cond_t *c, b_mutex_t *m, const struct b_timespec *abstime);

// These report failure through b_errno() and return -1, like POSIX.
int b_sem_init(b_sem_t *s, int shared, uint32_t value);
int b_sem_destroy(b_sem_t *s);
int b_sem_post(b_sem_t *s);
int b_sem_wait(b_sem_t *s);
int b_sem_trywait(b_sem_t *s);
int b_sem_timedwait(b_sem_t *s, const struct b_timespec *abstime);
int b_sem_getvalue(b_sem_t *s, int *out);

int b_once(b_once_t *once, void (*fn)(void));
int b_key_create(b_key_t *key, void (*destructor)(void *));
int b_key_delete(b_key_t key);
void *b_getspecific(b_key_t key);
int b_setspecific(b_key_t key, const void *value);

int b_attr_init(b_attr_t *a);
int b_attr_destroy(b_attr_t *a);
int b_attr_setdetachstate(b_attr_t *a, int detached);
int b_attr_getdetachstate(const b_attr_t *a, int *detached);
int b_attr_setstacksize(b_attr_t *a, uint32_t size);
int b_attr_getstack(const b_attr_t *a, void **base, uint32_t *size);
int b_attr_setschedpolicy(b_attr_t *a, int policy);
int b_attr_setschedparam(b_attr_t *a, const struct b_sched_param *p);

int b_pthread_create(b_pthread_t *out, const b_attr_t *attr, void *(*fn)(void *), void *arg);
int b_pthread_join(b_pthread_t t, void **ret);
int b_pthread_detach(b_pthread_t t);
void b_pthread_exit(void *ret) __attribute__((noreturn));
b_pthread_t b_pthread_self(void);
int b_pthread_equal(b_pthread_t a, b_pthread_t b);
int b_pthread_kill(b_pthread_t t, int sig);
int b_pthread_sigmask(int how, const void *set, void *old);
int b_pthread_getattr_np(b_pthread_t t, b_attr_t *a);
int b_pthread_getschedparam(b_pthread_t t, int *policy, struct b_sched_param *p);
int b_pthread_setschedparam(b_pthread_t t, int policy, const struct b_sched_param *p);
void b_cleanup_push(struct b_cleanup *c, void (*routine)(void *), void *arg);
void b_cleanup_pop(struct b_cleanup *c, int execute);
int b_sched_yield(void);

// Brackets a pause from outside the collector.  On 0 the collector is busy: do not pause.
int b_gc_pause_begin(void);
void b_gc_pause_end(void);

#endif
