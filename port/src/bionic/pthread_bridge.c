#include "port/bionic_thread.h"
#include "port/bionic_fs.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#elif !defined(PORT_QEMU)
#include <pthread.h>
#include <sched.h>
#endif

// Overridable so tests can prove they detect a missing release barrier.
#ifndef UNLOCK_ORDER
#define UNLOCK_ORDER memory_order_release
#endif

#define STATE_MASK 3u
#define TYPE_MASK 0xC000u
#define TYPE_SHIFT 14
#define COUNT_ONE 4u
#define COUNT_MASK 0x1FFCu
#define OWNER_SHIFT 16

#define DEFAULT_STACK (1024 * 1024) // Android's default; libnx would otherwise pick less

struct b_thread {
    void *(*fn)(void *);
    void *arg;
    void *ret;
    _Atomic uint32_t done;
    int detached;
    int main_thread;
    uint32_t tid;
    int32_t policy, priority;
    struct b_cleanup *cleanups;
    void *specific[B_KEYS_MAX];
    void *_Atomic blocked_sp;
#ifdef __SWITCH__
    Thread nx;
    Thread *nx_ptr; // &nx, or libnx's own record for a thread it created
#elif defined(PORT_QEMU)
    void *stack;
    uint32_t stack_size;
    int linux_tid;
    _Atomic uint32_t paused;
    uint32_t regs[16];
#else
    pthread_t host;
#endif
};

static __thread struct b_thread *t_self;
static _Atomic uint32_t g_next_tid = 1;

static uint32_t self_tid(void)
{
    return b_pthread_self()->tid;
}

uint32_t b_thread_id(void)
{
    return self_tid();
}

// ---- mutex ---------------------------------------------------------------

int b_mutexattr_init(b_mutexattr_t *a) { *a = B_MUTEX_NORMAL; return 0; }
int b_mutexattr_destroy(b_mutexattr_t *a) { (void)a; return 0; }

int b_mutexattr_settype(b_mutexattr_t *a, int type)
{
    if (type < B_MUTEX_NORMAL || type > B_MUTEX_ERRORCHECK)
        return B_EINVAL;
    *a = type;
    return 0;
}

int b_mutex_init(b_mutex_t *m, const b_mutexattr_t *attr)
{
    uint32_t type = attr ? (uint32_t)*attr : B_MUTEX_NORMAL;
    atomic_store_explicit(&m->value, type << TYPE_SHIFT, memory_order_relaxed);
    return 0;
}

int b_mutex_destroy(b_mutex_t *m)
{
    return (atomic_load_explicit(&m->value, memory_order_relaxed) & STATE_MASK) ? B_EBUSY : 0;
}

int b_mutex_trylock(b_mutex_t *m)
{
    uint32_t v = atomic_load_explicit(&m->value, memory_order_relaxed);
    uint32_t type = v & TYPE_MASK;
    uint32_t owner = 0;
    if (type) {
        owner = self_tid() << OWNER_SHIFT;
        if ((v & STATE_MASK) && (v >> OWNER_SHIFT) == (owner >> OWNER_SHIFT)) {
            if (type != (B_MUTEX_RECURSIVE << TYPE_SHIFT))
                return B_EBUSY;
            if ((v & COUNT_MASK) == COUNT_MASK)
                return B_EAGAIN;
            atomic_fetch_add_explicit(&m->value, COUNT_ONE, memory_order_relaxed);
            return 0;
        }
    }
    uint32_t expected = type;
    return atomic_compare_exchange_strong_explicit(&m->value, &expected, type | owner | 1, memory_order_acquire,
                                                   memory_order_relaxed)
               ? 0
               : B_EBUSY;
}

static void lock_slow_normal(b_mutex_t *m, uint32_t type)
{
    // State 2 means "may have waiters", so an unlock knows it must wake.
    while (atomic_exchange_explicit(&m->value, type | 2, memory_order_acquire) != type)
        futex_wait(&m->value, type | 2, -1);
}

static void lock_slow_owned(b_mutex_t *m, uint32_t type, uint32_t owner)
{
    for (;;) {
        uint32_t v = atomic_load_explicit(&m->value, memory_order_relaxed);
        if ((v & STATE_MASK) == 0) {
            if (atomic_compare_exchange_weak_explicit(&m->value, &v, type | owner | 2, memory_order_acquire,
                                                      memory_order_relaxed))
                return;
            continue;
        }
        if ((v & STATE_MASK) == 1) {
            uint32_t contended = (v & ~STATE_MASK) | 2;
            if (!atomic_compare_exchange_weak_explicit(&m->value, &v, contended, memory_order_relaxed,
                                                       memory_order_relaxed))
                continue;
            v = contended;
        }
        futex_wait(&m->value, v, -1);
    }
}

int b_mutex_lock(b_mutex_t *m)
{
    uint32_t v = atomic_load_explicit(&m->value, memory_order_relaxed);
    uint32_t type = v & TYPE_MASK;
    if (type == 0) {
        uint32_t expected = 0;
        if (!atomic_compare_exchange_strong_explicit(&m->value, &expected, 1, memory_order_acquire,
                                                     memory_order_relaxed))
            lock_slow_normal(m, 0);
        return 0;
    }

    uint32_t owner = self_tid() << OWNER_SHIFT;
    if ((v & STATE_MASK) && (v >> OWNER_SHIFT) == (owner >> OWNER_SHIFT)) {
        if (type != (B_MUTEX_RECURSIVE << TYPE_SHIFT))
            return B_EDEADLK;
        if ((v & COUNT_MASK) == COUNT_MASK)
            return B_EAGAIN;
        atomic_fetch_add_explicit(&m->value, COUNT_ONE, memory_order_relaxed);
        return 0;
    }
    uint32_t expected = type;
    if (!atomic_compare_exchange_strong_explicit(&m->value, &expected, type | owner | 1, memory_order_acquire,
                                                 memory_order_relaxed))
        lock_slow_owned(m, type, owner);
    return 0;
}

int b_mutex_unlock(b_mutex_t *m)
{
    uint32_t v = atomic_load_explicit(&m->value, memory_order_relaxed);
    uint32_t type = v & TYPE_MASK;
    if (type) {
        if ((v & STATE_MASK) == 0 || (v >> OWNER_SHIFT) != self_tid())
            return B_EPERM;
        if (v & COUNT_MASK) {
            atomic_fetch_sub_explicit(&m->value, COUNT_ONE, memory_order_relaxed);
            return 0;
        }
    }
    uint32_t old = atomic_exchange_explicit(&m->value, type, UNLOCK_ORDER);
    if ((old & STATE_MASK) == 2)
        futex_wake(&m->value, 1);
    return 0;
}

// ---- condition variable --------------------------------------------------

int b_cond_init(b_cond_t *c, const void *attr)
{
    (void)attr;
    atomic_store_explicit(&c->value, 0, memory_order_relaxed);
    return 0;
}

int b_cond_destroy(b_cond_t *c) { (void)c; return 0; }

static int cond_wake(b_cond_t *c, int count)
{
    atomic_fetch_add_explicit(&c->value, 2, memory_order_release); // bit 0 is bionic's shared flag
    futex_wake(&c->value, count);
    return 0;
}

int b_cond_signal(b_cond_t *c) { return cond_wake(c, 1); }
int b_cond_broadcast(b_cond_t *c) { return cond_wake(c, INT_MAX); }

// A signal between unlock and sleep changes the sequence, so futex_wait returns
// immediately: wakeups cannot be lost.  Spurious returns are permitted by POSIX.
static int cond_wait_ns(b_cond_t *c, b_mutex_t *m, int64_t timeout_ns)
{
    uint32_t seq = atomic_load_explicit(&c->value, memory_order_acquire);
    int r = b_mutex_unlock(m);
    if (r)
        return r;
    int status = futex_wait(&c->value, seq, timeout_ns);
    b_mutex_lock(m);
    return status;
}

int b_cond_wait(b_cond_t *c, b_mutex_t *m)
{
    return cond_wait_ns(c, m, -1);
}

static int64_t remaining_ns(const struct b_timespec *abstime)
{
    int64_t target = (int64_t)abstime->tv_sec * 1000000000 + abstime->tv_nsec;
    int64_t left = target - b_realtime_ns();
    return left < 0 ? 0 : left;
}

int b_cond_timedwait(b_cond_t *c, b_mutex_t *m, const struct b_timespec *abstime)
{
    if (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000)
        return B_EINVAL;
    return cond_wait_ns(c, m, remaining_ns(abstime));
}

// ---- semaphore -----------------------------------------------------------

int b_sem_init(b_sem_t *s, int shared, uint32_t value)
{
    (void)shared;
    atomic_store_explicit(&s->count, value, memory_order_relaxed);
    return 0;
}

int b_sem_destroy(b_sem_t *s) { (void)s; return 0; }

int b_sem_post(b_sem_t *s)
{
    atomic_fetch_add_explicit(&s->count, 1, memory_order_release);
    futex_wake(&s->count, 1);
    return 0;
}

static int sem_try(b_sem_t *s)
{
    uint32_t c = atomic_load_explicit(&s->count, memory_order_relaxed);
    while (c > 0)
        if (atomic_compare_exchange_weak_explicit(&s->count, &c, c - 1, memory_order_acquire, memory_order_relaxed))
            return 1;
    return 0;
}

int b_sem_wait(b_sem_t *s)
{
    while (!sem_try(s))
        futex_wait(&s->count, 0, -1);
    return 0;
}

int b_sem_trywait(b_sem_t *s)
{
    if (sem_try(s))
        return 0;
    *b_errno() = B_EAGAIN;
    return -1;
}

int b_sem_timedwait(b_sem_t *s, const struct b_timespec *abstime)
{
    while (!sem_try(s)) {
        int64_t left = remaining_ns(abstime);
        if (left == 0 || futex_wait(&s->count, 0, left) == B_ETIMEDOUT) {
            if (sem_try(s))
                return 0;
            *b_errno() = B_ETIMEDOUT;
            return -1;
        }
    }
    return 0;
}

int b_sem_getvalue(b_sem_t *s, int *out)
{
    *out = (int)atomic_load_explicit(&s->count, memory_order_relaxed);
    return 0;
}

// ---- once ----------------------------------------------------------------

enum { ONCE_INIT, ONCE_RUNNING, ONCE_DONE };

int b_once(b_once_t *once, void (*fn)(void))
{
    _Atomic uint32_t *word = (_Atomic uint32_t *)once;
    int32_t state = atomic_load_explicit(once, memory_order_acquire);
    if (state == ONCE_DONE)
        return 0;
    int32_t expected = ONCE_INIT;
    if (atomic_compare_exchange_strong_explicit(once, &expected, ONCE_RUNNING, memory_order_acquire,
                                                memory_order_acquire)) {
        fn();
        atomic_store_explicit(once, ONCE_DONE, memory_order_release);
        futex_wake(word, INT_MAX);
        return 0;
    }
    while (atomic_load_explicit(once, memory_order_acquire) != ONCE_DONE)
        futex_wait(word, ONCE_RUNNING, -1);
    return 0;
}

// ---- thread-specific data ------------------------------------------------

static b_mutex_t g_key_lock;
static uint8_t g_key_used[B_KEYS_MAX];
static void (*g_key_dtor[B_KEYS_MAX])(void *);

int b_key_create(b_key_t *key, void (*destructor)(void *))
{
    b_mutex_lock(&g_key_lock);
    for (int i = 0; i < B_KEYS_MAX; i++) {
        if (!g_key_used[i]) {
            g_key_used[i] = 1;
            g_key_dtor[i] = destructor;
            b_mutex_unlock(&g_key_lock);
            *key = i;
            return 0;
        }
    }
    b_mutex_unlock(&g_key_lock);
    return B_EAGAIN;
}

int b_key_delete(b_key_t key)
{
    if (key < 0 || key >= B_KEYS_MAX)
        return B_EINVAL;
    b_mutex_lock(&g_key_lock);
    g_key_used[key] = 0;
    g_key_dtor[key] = NULL;
    b_mutex_unlock(&g_key_lock);
    return 0;
}

void *b_getspecific(b_key_t key)
{
    return (key >= 0 && key < B_KEYS_MAX) ? b_pthread_self()->specific[key] : NULL;
}

int b_setspecific(b_key_t key, const void *value)
{
    if (key < 0 || key >= B_KEYS_MAX)
        return B_EINVAL;
    b_pthread_self()->specific[key] = (void *)value;
    return 0;
}

static void run_key_destructors(struct b_thread *t)
{
    for (int pass = 0; pass < 4; pass++) { // PTHREAD_DESTRUCTOR_ITERATIONS
        int ran = 0;
        for (int i = 0; i < B_KEYS_MAX; i++) {
            void *v = t->specific[i];
            b_mutex_lock(&g_key_lock);
            void (*dtor)(void *) = g_key_dtor[i];
            b_mutex_unlock(&g_key_lock);
            if (v && dtor) {
                t->specific[i] = NULL;
                dtor(v);
                ran = 1;
            }
        }
        if (!ran)
            break;
    }
}

// ---- attributes ----------------------------------------------------------

int b_attr_init(b_attr_t *a)
{
    memset(a, 0, sizeof *a);
    a->stack_size = DEFAULT_STACK;
    a->guard_size = 4096;
    return 0;
}

int b_attr_destroy(b_attr_t *a) { (void)a; return 0; }

int b_attr_setdetachstate(b_attr_t *a, int detached)
{
    if (detached)
        a->flags |= B_PTHREAD_DETACHED;
    else
        a->flags &= ~(uint32_t)B_PTHREAD_DETACHED;
    return 0;
}

int b_attr_getdetachstate(const b_attr_t *a, int *detached)
{
    *detached = (a->flags & B_PTHREAD_DETACHED) ? 1 : 0;
    return 0;
}

// bionic accepts anything from 8 KB, and FMOD asks for that little on its stream threads.
// Refusing it fails the stream, which silences music while sound effects keep playing.  The code
// under the engine here wants more room than a phone's libc, so small requests are raised.
#define SMALL_STACK (32 * 1024)

int b_attr_setstacksize(b_attr_t *a, uint32_t size)
{
    a->stack_size = size < 16 * 1024 ? SMALL_STACK : size;
    return 0;
}

int b_attr_getstack(const b_attr_t *a, void **base, uint32_t *size)
{
    *base = a->stack_base;
    *size = a->stack_size;
    return 0;
}

int b_attr_setschedpolicy(b_attr_t *a, int policy) { a->sched_policy = policy; return 0; }

int b_attr_setschedparam(b_attr_t *a, const struct b_sched_param *p)
{
    a->sched_priority = p->sched_priority;
    return 0;
}

// ---- threads -------------------------------------------------------------

#define THREAD_TABLE 1024
static struct b_thread *_Atomic g_threads[THREAD_TABLE];

static void assign_tid(struct b_thread *t)
{
    // Owner field is 16 bits; zero must stay "no owner".
    uint32_t id = atomic_fetch_add_explicit(&g_next_tid, 1, memory_order_relaxed) & 0xFFFF;
    t->tid = id ? id : 1;
    atomic_store(&g_threads[t->tid % THREAD_TABLE], t);
}

b_pthread_t b_thread_by_id(uint32_t id)
{
    struct b_thread *t = atomic_load(&g_threads[id % THREAD_TABLE]);
    return (t && t->tid == id) ? t : NULL;
}

void *b_thread_blocked_sp(b_pthread_t t)
{
    return atomic_load(&t->blocked_sp);
}

void b_blocking_enter(void *spill)
{
    atomic_store(&b_pthread_self()->blocked_sp, spill);
}

void b_blocking_exit(void)
{
    atomic_store(&b_pthread_self()->blocked_sp, NULL);
}

static struct b_thread *new_thread(void)
{
    struct b_thread *t = calloc(1, sizeof *t);
    if (t)
        assign_tid(t);
    return t;
}

// Runs on threads that did not come from b_pthread_create, such as libnx's main.
// The first adoption must not allocate: malloc's lock can be a b_mutex, and taking
// a recursive one asks for the current thread, which would re-enter malloc.
b_pthread_t b_pthread_self(void)
{
    static struct b_thread first;
    static _Atomic uint32_t first_used;

    if (!t_self) {
        if (atomic_exchange_explicit(&first_used, 1, memory_order_relaxed) == 0) {
            t_self = &first;
            assign_tid(t_self);
        } else {
            t_self = new_thread();
            if (!t_self)
                abort();
        }
        t_self->main_thread = 1;
    }
    return t_self;
}

void b_cleanup_push(struct b_cleanup *c, void (*routine)(void *), void *arg)
{
    struct b_thread *t = b_pthread_self();
    c->routine = routine;
    c->arg = arg;
    c->prev = t->cleanups;
    t->cleanups = c;
}

void b_cleanup_pop(struct b_cleanup *c, int execute)
{
    b_pthread_self()->cleanups = c->prev;
    if (execute)
        c->routine(c->arg);
}

static void thread_finish(struct b_thread *t, void *ret)
{
    t->ret = ret;
    while (t->cleanups) {
        struct b_cleanup *c = t->cleanups;
        t->cleanups = c->prev;
        c->routine(c->arg);
    }
    run_key_destructors(t);
    // Before the struct can be freed: a late tkill must not find this thread.
    struct b_thread *expected = t;
    atomic_compare_exchange_strong(&g_threads[t->tid % THREAD_TABLE], &expected, NULL);
    atomic_store_explicit(&t->done, 1, memory_order_release);
    futex_wake(&t->done, INT_MAX);
}

#ifdef __SWITCH__

// Core 3 belongs to the system.  Horizon does not balance threads the way Linux
// does: a thread stays on its preferred core unless its affinity mask allows
// another idle core, so an unset mask would pile every Unity thread onto core 0.
// Android preempts, and Unity relies on it: its threads spin waiting for each other.  Horizon only
// time-slices one priority: every 10 ms the kernel rotates priority 59 on cores 0-2 and moves
// threads between them.  At any other priority a spinning thread keeps its core for ever.  Above
// 59 it also outranks the system services sharing those cores, which freezes the whole console.
#define GUEST_PRIORITY 59
#define CORE_MASK 0x7u
static _Atomic uint32_t g_next_core = 1;

void b_thread_init(void)
{
    b_pthread_self()->nx_ptr = threadGetSelf();
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, CORE_MASK);
    svcSetThreadPriority(CUR_THREAD_HANDLE, GUEST_PRIORITY);
}

static void thread_entry(void *arg)
{
    struct b_thread *t = arg;
    t_self = t;
    thread_finish(t, t->fn(t->arg));
}

// A thread can sit in the table before it has a native thread, or after it has lost one.
int b_thread_has_handle(b_pthread_t t)
{
    return t->nx_ptr != NULL && t->nx_ptr->handle != 0;
}

// The kernel writes its full context, 64-bit slots even for a 32-bit thread: r13 is
// sp and r14 is lr, pc has its own slot.  libnx32's ThreadContext is too small for it.
struct kernel_context {
    u64 r[29];
    u64 fp, lr, sp, pc;
    u8 rest[0x320 - 29 * 8 - 4 * 8];
};
_Static_assert(sizeof(struct kernel_context) == 0x320, "kernel ThreadContext size");

void b_thread_pause(b_pthread_t t, uint32_t regs[16])
{
    static struct kernel_context ctx; // one suspender at a time: Boehm holds its lock
    memset(&ctx, 0, sizeof ctx);
    threadPause(t->nx_ptr);
    Result rc = svcGetThreadContext3((ThreadContext *)&ctx, t->nx_ptr->handle);
    if (R_FAILED(rc))
        fprintf(stderr, "b_thread_pause: context rc=0x%x\n", rc);
    for (int i = 0; i < 15; i++)
        regs[i] = (uint32_t)ctx.r[i];
    regs[15] = (uint32_t)ctx.pc;
}

void b_thread_unpause(b_pthread_t t)
{
    threadResume(t->nx_ptr);
}

int b_pthread_create(b_pthread_t *out, const b_attr_t *attr, void *(*fn)(void *), void *arg)
{
    struct b_thread *t = new_thread();
    if (!t)
        return B_EAGAIN;
    t->fn = fn;
    t->arg = arg;
    uint32_t stack = attr ? attr->stack_size : DEFAULT_STACK;
    stack = (stack + 0xFFF) & ~0xFFFu;
    if (attr) {
        t->detached = (attr->flags & B_PTHREAD_DETACHED) != 0;
        t->policy = attr->sched_policy;
        t->priority = attr->sched_priority;
    }

    // Workers use cores 1 and 2 first, keeping the main loop's core quieter.
    uint32_t core = 1 + (atomic_fetch_add_explicit(&g_next_core, 1, memory_order_relaxed) & 1);
    t->nx_ptr = &t->nx;
    Result rc = threadCreate(&t->nx, thread_entry, t, NULL, stack, GUEST_PRIORITY, (int)core);
    if (R_FAILED(rc)) {
        free(t);
        return B_EAGAIN;
    }
    svcSetThreadCoreMask(t->nx.handle, (s32)core, CORE_MASK);
    rc = threadStart(&t->nx);
    if (R_FAILED(rc)) {
        threadClose(&t->nx);
        free(t);
        return B_EAGAIN;
    }
    *out = t;
    return 0;
}

static void host_join(struct b_thread *t)
{
    threadWaitForExit(&t->nx);
    threadClose(&t->nx);
}

void b_pthread_exit(void *ret)
{
    struct b_thread *t = b_pthread_self();
    thread_finish(t, ret);
    threadExit();
}

int b_sched_yield(void)
{
    // A plain yield never leaves the core; spinning guests need this to make
    // progress when the thread they wait for is queued elsewhere.
    svcSleepThread(YieldType_ToAnyThread);
    return 0;
}

int b_pthread_getattr_np(b_pthread_t t, b_attr_t *a)
{
    b_attr_init(a);
    if (t->main_thread) {
        // The stack region containing a local variable of this thread.
        MemoryInfo mi;
        u32 page;
        svcQueryMemory(&mi, &page, (u32)(uintptr_t)&mi);
        a->stack_base = (void *)(uintptr_t)mi.addr;
        a->stack_size = (uint32_t)mi.size;
    } else {
        a->stack_base = t->nx.stack_mirror;
        a->stack_size = t->nx.stack_sz;
    }
    return 0;
}

#elif defined(PORT_QEMU)

// Emulated ARM Linux: clone() with the harness providing TLS (tests/qemu/sys.c).
int qemu_thread_start(void (*entry)(void *), void *arg, void *stack, uint32_t stack_size);
void qemu_thread_exit(void) __attribute__((noreturn));
void qemu_yield(void);
int qemu_gettid(void);
void qemu_kick(int linux_tid);

void b_thread_init(void)
{
    b_pthread_self()->linux_tid = qemu_gettid();
}

static void thread_entry(void *arg)
{
    struct b_thread *t = arg;
    t_self = t;
    t->linux_tid = qemu_gettid();
    thread_finish(t, t->fn(t->arg));
    qemu_thread_exit();
}

// Signal handler in the harness: stands in for the kernel pausing a thread.
void qemu_on_pause(const uint32_t *regs)
{
    struct b_thread *t = b_pthread_self();
    memcpy(t->regs, regs, sizeof t->regs);
    atomic_store(&t->paused, 1);
    futex_wake(&t->paused, INT_MAX);
    while (atomic_load(&t->paused) == 1)
        futex_wait(&t->paused, 1, -1);
}

void b_thread_pause(b_pthread_t t, uint32_t regs[16])
{
    while (t->linux_tid == 0) // created but not yet running
        qemu_yield();
    qemu_kick(t->linux_tid);
    while (atomic_load(&t->paused) != 1)
        futex_wait(&t->paused, 0, 1000000);
    memcpy(regs, t->regs, sizeof t->regs);
}

void b_thread_unpause(b_pthread_t t)
{
    atomic_store(&t->paused, 0);
    futex_wake(&t->paused, INT_MAX);
}

int b_pthread_create(b_pthread_t *out, const b_attr_t *attr, void *(*fn)(void *), void *arg)
{
    struct b_thread *t = new_thread();
    if (!t)
        return B_EAGAIN;
    t->fn = fn;
    t->arg = arg;
    t->stack_size = attr ? attr->stack_size : DEFAULT_STACK;
    t->stack = attr && attr->stack_base ? attr->stack_base : malloc(t->stack_size);
    if (attr) {
        t->detached = (attr->flags & B_PTHREAD_DETACHED) != 0;
        t->policy = attr->sched_policy;
        t->priority = attr->sched_priority;
    }
    if (!t->stack || qemu_thread_start(thread_entry, t, t->stack, t->stack_size) != 0) {
        free(t);
        return B_EAGAIN;
    }
    *out = t;
    return 0;
}

static void host_join(struct b_thread *t)
{
    (void)t;
}

void b_pthread_exit(void *ret)
{
    thread_finish(b_pthread_self(), ret);
    qemu_thread_exit();
}

int b_sched_yield(void)
{
    qemu_yield();
    return 0;
}

int b_pthread_getattr_np(b_pthread_t t, b_attr_t *a)
{
    b_attr_init(a);
    if (t->stack) {
        a->stack_base = t->stack;
        a->stack_size = t->stack_size;
    } else {
        // Main thread: the harness runs it on a known 8 MB stack.
        extern char qemu_main_stack[];
        a->stack_base = qemu_main_stack;
        a->stack_size = 8 * 1024 * 1024;
    }
    return 0;
}

#else // host

void b_thread_init(void)
{
    b_pthread_self();
}

void b_thread_pause(b_pthread_t t, uint32_t regs[16])
{
    (void)t;
    memset(regs, 0, 16 * sizeof(uint32_t));
}

void b_thread_unpause(b_pthread_t t)
{
    (void)t;
}

static void *thread_entry(void *arg)
{
    struct b_thread *t = arg;
    t_self = t;
    thread_finish(t, t->fn(t->arg));
    return NULL;
}

int b_pthread_create(b_pthread_t *out, const b_attr_t *attr, void *(*fn)(void *), void *arg)
{
    struct b_thread *t = new_thread();
    if (!t)
        return B_EAGAIN;
    t->fn = fn;
    t->arg = arg;
    if (attr) {
        t->detached = (attr->flags & B_PTHREAD_DETACHED) != 0;
        t->policy = attr->sched_policy;
        t->priority = attr->sched_priority;
    }
    if (pthread_create(&t->host, NULL, thread_entry, t)) {
        free(t);
        return B_EAGAIN;
    }
    *out = t;
    return 0;
}

static void host_join(struct b_thread *t)
{
    pthread_join(t->host, NULL);
}

void b_pthread_exit(void *ret)
{
    thread_finish(b_pthread_self(), ret);
    pthread_exit(NULL);
}

int b_sched_yield(void)
{
    return sched_yield();
}

int b_pthread_getattr_np(b_pthread_t t, b_attr_t *a)
{
    (void)t;
    return b_attr_init(a);
}

#endif

int b_pthread_join(b_pthread_t t, void **ret)
{
    if (t == b_pthread_self())
        return B_EDEADLK;
    if (t->detached)
        return B_EINVAL;
    while (!atomic_load_explicit(&t->done, memory_order_acquire))
        futex_wait(&t->done, 0, -1);
    host_join(t);
    if (ret)
        *ret = t->ret;
    free(t);
    return 0;
}

// The struct of a detached thread is kept: it may still be finishing, and a
// join-less free needs a reaper that the workload does not justify.
int b_pthread_detach(b_pthread_t t)
{
    t->detached = 1;
    return 0;
}

int b_pthread_equal(b_pthread_t a, b_pthread_t b) { return a == b; }

// No Horizon equivalent yet.  sig==0 is the only form that works: signal 30/24 from
// Boehm's stop-the-world need the suspend bridge described in docs/PORTING.md.
int b_pthread_kill(b_pthread_t t, int sig)
{
    (void)t;
    return sig == 0 ? 0 : B_ENOSYS;
}

int b_pthread_sigmask(int how, const void *set, void *old)
{
    (void)how;
    (void)set;
    if (old)
        memset(old, 0, 4); // bionic sigset_t
    return 0;
}

int b_pthread_getschedparam(b_pthread_t t, int *policy, struct b_sched_param *p)
{
    *policy = t->policy;
    p->sched_priority = t->priority;
    return 0;
}

int b_pthread_setschedparam(b_pthread_t t, int policy, const struct b_sched_param *p)
{
    t->policy = policy;
    t->priority = p->sched_priority;
    return 0;
}
