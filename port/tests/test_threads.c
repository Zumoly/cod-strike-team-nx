// Host stress test of the bionic threading bridge using real threads.
// Built plain at -O2 and under ThreadSanitizer.  A plain data race on `counter`
// is intentional: the mutex is what makes it correct, so TSan reports it if an
// acquire/release is ever missing.
#include "port/bionic_fs.h"
#include "port/bionic_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c)                                                                 \
    do {                                                                         \
        int ok_ = (c);                                                           \
        if (!ok_ || g_verbose)                                                   \
            printf("%s %d: %s\n", ok_ ? "ok  " : "FAIL", __LINE__, #c);          \
        if (!ok_)                                                                \
            g_fail++;                                                            \
    } while (0)
static const int g_verbose = 1;

#define THREADS 8
#define ITERS 100000

static b_mutex_t g_mutex;
static long g_counter;

static void *inc_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        b_mutex_lock(&g_mutex);
        g_counter++;
        b_mutex_unlock(&g_mutex);
    }
    return NULL;
}

static void run_contention(int type)
{
    b_mutexattr_t attr;
    b_mutexattr_init(&attr);
    b_mutexattr_settype(&attr, type);
    b_mutex_init(&g_mutex, &attr);
    g_counter = 0;
    b_pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        CHECK(b_pthread_create(&t[i], NULL, inc_worker, NULL) == 0);
    for (int i = 0; i < THREADS; i++)
        CHECK(b_pthread_join(t[i], NULL) == 0);
    CHECK(g_counter == (long)THREADS * ITERS);
    CHECK(b_mutex_destroy(&g_mutex) == 0);
}

// Bounded queue: the classic lost-wakeup detector.
#define QCAP 4
#define ITEMS 50000
static b_mutex_t q_lock;
static b_cond_t q_not_empty, q_not_full;
static int q_buf[QCAP], q_head, q_len;
static long q_sum;

static void *producer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= ITEMS; i++) {
        b_mutex_lock(&q_lock);
        while (q_len == QCAP)
            b_cond_wait(&q_not_full, &q_lock);
        q_buf[(q_head + q_len++) % QCAP] = i;
        b_cond_signal(&q_not_empty);
        b_mutex_unlock(&q_lock);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITEMS; i++) {
        b_mutex_lock(&q_lock);
        while (q_len == 0)
            b_cond_wait(&q_not_empty, &q_lock);
        q_sum += q_buf[q_head];
        q_head = (q_head + 1) % QCAP;
        q_len--;
        b_cond_signal(&q_not_full);
        b_mutex_unlock(&q_lock);
    }
    return NULL;
}

// Semaphore ping-pong: the handshake Boehm's stop-the-world depends on.
static b_sem_t s_a, s_b;
static long s_shared;

static void *ponger(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITEMS; i++) {
        b_sem_wait(&s_a);
        s_shared++;
        b_sem_post(&s_b);
    }
    return NULL;
}

static b_once_t g_once;
static _Atomic int g_once_runs;
static long g_once_value;

static void once_fn(void)
{
    usleep(1000);
    g_once_value = 42;
    g_once_runs++;
}

static void *once_worker(void *arg)
{
    (void)arg;
    b_once(&g_once, once_fn);
    return (void *)g_once_value;
}

static _Atomic int g_dtor_calls;
static void key_dtor(void *v)
{
    (void)v;
    g_dtor_calls++;
}

static b_key_t g_key;
static void *key_worker(void *arg)
{
    b_setspecific(g_key, arg);
    return b_getspecific(g_key);
}

static int g_order[3], g_order_n;
static void cleanup_fn(void *v) { g_order[g_order_n++] = (int)(long)v; }

static void *exit_worker(void *arg)
{
    struct b_cleanup a, b;
    b_cleanup_push(&a, cleanup_fn, (void *)1);
    b_cleanup_push(&b, cleanup_fn, (void *)2);
    b_pthread_exit(arg); // runs b then a; code below must not execute
    b_cleanup_pop(&b, 0);
    b_cleanup_pop(&a, 0);
    return NULL;
}

static struct b_timespec in_ms(int ms)
{
    int64_t t = b_realtime_ns() + (int64_t)ms * 1000000;
    struct b_timespec ts = {(int32_t)(t / 1000000000), (int32_t)(t % 1000000000)};
    return ts;
}

int main(void)
{
    b_thread_init();

    run_contention(B_MUTEX_NORMAL);
    run_contention(B_MUTEX_RECURSIVE);
    run_contention(B_MUTEX_ERRORCHECK);

    // Statically zero-initialized and bionic's recursive initializer value.
    static b_mutex_t zero_mutex;
    CHECK(b_mutex_lock(&zero_mutex) == 0);
    CHECK(b_mutex_trylock(&zero_mutex) == B_EBUSY);
    CHECK(b_mutex_unlock(&zero_mutex) == 0);

    b_mutex_t rec = {B_MUTEX_RECURSIVE_INIT};
    CHECK(b_mutex_lock(&rec) == 0);
    CHECK(b_mutex_lock(&rec) == 0);
    CHECK(b_mutex_trylock(&rec) == 0);
    CHECK(b_mutex_unlock(&rec) == 0);
    CHECK(b_mutex_unlock(&rec) == 0);
    CHECK(b_mutex_unlock(&rec) == 0);
    CHECK(b_mutex_unlock(&rec) == B_EPERM);
    CHECK(atomic_load(&rec.value) == B_MUTEX_RECURSIVE_INIT);

    b_mutex_t chk = {B_MUTEX_ERRORCHECK_INIT};
    CHECK(b_mutex_lock(&chk) == 0);
    CHECK(b_mutex_lock(&chk) == B_EDEADLK);
    CHECK(b_mutex_unlock(&chk) == 0);

    b_pthread_t p[2], c[2];
    q_sum = 0;
    for (int i = 0; i < 2; i++) {
        CHECK(b_pthread_create(&p[i], NULL, producer, NULL) == 0);
        CHECK(b_pthread_create(&c[i], NULL, consumer, NULL) == 0);
    }
    for (int i = 0; i < 2; i++) {
        CHECK(b_pthread_join(p[i], NULL) == 0);
        CHECK(b_pthread_join(c[i], NULL) == 0);
    }
    CHECK(q_sum == 2L * ITEMS * (ITEMS + 1) / 2);

    // Broadcast reaches every waiter; timed waits report and respect the deadline.
    b_mutex_t m = {0};
    b_cond_t cv = {0};
    b_mutex_lock(&m);
    struct b_timespec dl = in_ms(30);
    int64_t t0 = b_monotonic_ns();
    int r = b_cond_timedwait(&cv, &m, &dl);
    int64_t elapsed = b_monotonic_ns() - t0;
    b_mutex_unlock(&m);
    CHECK(r == B_ETIMEDOUT);
    CHECK(elapsed >= 25000000);
    CHECK(elapsed < 500000000);

    b_sem_init(&s_a, 0, 0);
    b_sem_init(&s_b, 0, 0);
    b_pthread_t pong;
    CHECK(b_pthread_create(&pong, NULL, ponger, NULL) == 0);
    for (int i = 0; i < ITEMS; i++) {
        b_sem_post(&s_a);
        b_sem_wait(&s_b);
    }
    CHECK(b_pthread_join(pong, NULL) == 0);
    CHECK(s_shared == ITEMS);

    b_sem_t sem;
    b_sem_init(&sem, 0, 1);
    int sv;
    CHECK(b_sem_trywait(&sem) == 0);
    CHECK(b_sem_trywait(&sem) == -1 && *b_errno() == B_EAGAIN);
    dl = in_ms(20);
    CHECK(b_sem_timedwait(&sem, &dl) == -1 && *b_errno() == B_ETIMEDOUT);
    b_sem_post(&sem);
    CHECK(b_sem_getvalue(&sem, &sv) == 0 && sv == 1);

    b_pthread_t o[THREADS];
    for (int i = 0; i < THREADS; i++)
        CHECK(b_pthread_create(&o[i], NULL, once_worker, NULL) == 0);
    for (int i = 0; i < THREADS; i++) {
        void *v;
        CHECK(b_pthread_join(o[i], &v) == 0);
        CHECK((long)v == 42);
    }
    CHECK(g_once_runs == 1);

    CHECK(b_key_create(&g_key, key_dtor) == 0);
    b_pthread_t k[THREADS];
    for (int i = 0; i < THREADS; i++)
        CHECK(b_pthread_create(&k[i], NULL, key_worker, (void *)(long)(i + 1)) == 0);
    for (int i = 0; i < THREADS; i++) {
        void *v;
        CHECK(b_pthread_join(k[i], &v) == 0);
        CHECK((long)v == i + 1);
    }
    CHECK(g_dtor_calls == THREADS);
    CHECK(b_getspecific(g_key) == NULL);
    CHECK(b_key_delete(g_key) == 0);

    b_pthread_t e;
    void *ret;
    CHECK(b_pthread_create(&e, NULL, exit_worker, (void *)7) == 0);
    CHECK(b_pthread_join(e, &ret) == 0);
    CHECK((long)ret == 7);
    CHECK(g_order_n == 2 && g_order[0] == 2 && g_order[1] == 1);

    CHECK(b_pthread_join(b_pthread_self(), NULL) == B_EDEADLK);
    CHECK(b_pthread_kill(b_pthread_self(), 0) == 0);

    printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail != 0;
}
