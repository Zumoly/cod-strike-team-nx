// Sampled live bytes per call site for every malloc made through the port's own code (the link
// wraps malloc, calloc, realloc, memalign and free).  Run 15 passed wave 5 but newlib's in-use total
// still grew ~0.2 MB/s faster than the game's own blocks, and a 1 MB Unity allocation failed in a
// fragmented 1 GB heap.  One block in 8, chosen by address so free finds it without a lookup, is
// recorded with its caller; live bytes per caller times 8 estimate who holds the heap.
#include "port/so_loader.h"

#include <malloc.h>
#include <reent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__real_memalign(size_t, size_t);
void __real_free(void *);
void *_malloc_r(struct _reent *, size_t);
void *_calloc_r(struct _reent *, size_t, size_t);
void *_realloc_r(struct _reent *, void *, size_t);

#define SLOTS 131072 // power of two
#define SITES 2048
#define SAMPLE 8

static struct {
    uintptr_t p;
    uint32_t caller, size;
} g_blocks[SLOTS];
static struct {
    uint32_t caller;
    int32_t count;
    int64_t bytes;
} g_sites[SITES];
static uint32_t g_lock;

static void lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE))
#ifdef __SWITCH__
        svcSleepThread(0);
#else
        ;
#endif
}

static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

static int sampled(const void *p) { return p && (((uintptr_t)p >> 4) * 2654435761u >> 29) == 0; }

static void site_add(uint32_t caller, int64_t bytes, int count)
{
    unsigned i = (caller >> 1) * 2654435761u % SITES;
    for (int n = 0; n < 64; n++, i = (i + 1) % SITES)
        if (g_sites[i].caller == caller || g_sites[i].caller == 0) {
            g_sites[i].caller = caller;
            g_sites[i].bytes += bytes;
            g_sites[i].count += count;
            return;
        }
}

static void record(void *p, size_t size, uint32_t caller)
{
    if (!sampled(p))
        return;
    lock();
    unsigned i = ((uintptr_t)p >> 4) * 2654435761u & (SLOTS - 1);
    for (int n = 0; n < 256; n++, i = (i + 1) & (SLOTS - 1))
        if (g_blocks[i].p == 0 || g_blocks[i].p == 1) {
            g_blocks[i].p = (uintptr_t)p;
            g_blocks[i].caller = caller;
            g_blocks[i].size = (uint32_t)size;
            site_add(caller, (int64_t)size, 1);
            break;
        }
    unlock();
}

static void forget(void *p)
{
    if (!sampled(p))
        return;
    lock();
    unsigned i = ((uintptr_t)p >> 4) * 2654435761u & (SLOTS - 1);
    for (int n = 0; n < 256 && g_blocks[i].p != 0; n++, i = (i + 1) & (SLOTS - 1))
        if (g_blocks[i].p == (uintptr_t)p) {
            site_add(g_blocks[i].caller, -(int64_t)g_blocks[i].size, -1);
            g_blocks[i].p = 1; // tombstone
            break;
        }
    unlock();
}

// For heap_guard.c, whose game allocations call newlib directly.
void alloc_track_record(void *p, size_t size, uint32_t caller) { record(p, size, caller); }
void alloc_track_forget(void *p) { forget(p); }

#define CALLER() ((uint32_t)(uintptr_t)__builtin_return_address(0))

void *__wrap_malloc(size_t n)
{
    void *p = _malloc_r(_REENT, n); // not devkitARM's malloc: see heap_guard.c
    record(p, n, CALLER());
    return p;
}

void *__wrap_calloc(size_t n, size_t m)
{
    void *p = _calloc_r(_REENT, n, m);
    record(p, n * m, CALLER());
    return p;
}

void *__wrap_memalign(size_t a, size_t n)
{
    void *p = __real_memalign(a, n);
    record(p, n, CALLER());
    return p;
}

void *__wrap_realloc(void *old, size_t n)
{
    uint32_t caller = CALLER();
    forget(old);
    void *p = _realloc_r(_REENT, old, n);
    if (!p && n && old)
        record(old, malloc_usable_size(old), caller); // still live
    else
        record(p, n, caller);
    return p;
}

void __wrap_free(void *p)
{
    forget(p);
    __real_free(p);
}

// The watchdog's 30 s report: the top sites by estimated live bytes.
void alloc_track_report(void (*put)(const char *))
{
    struct {
        uint32_t caller;
        int32_t count;
        int64_t bytes;
    } top[12] = {{0}}, s, t;
    lock();
    for (int i = 0; i < SITES; i++) {
        if (!g_sites[i].caller)
            continue;
        s.caller = g_sites[i].caller;
        s.count = g_sites[i].count;
        s.bytes = g_sites[i].bytes;
        for (int k = 0; k < 12; k++)
            if (s.bytes > top[k].bytes) {
                t = top[k];
                top[k] = s;
                s = t;
            }
    }
    unlock();
    char line[200], where[96];
    put("port mallocs (sampled 1 in 8, scaled):\n");
    for (int k = 0; k < 12 && top[k].caller; k++) {
        so_describe(top[k].caller, where, sizeof where);
        snprintf(line, sizeof line, "  %7u KB in %7d blocks from %s\n", (unsigned)(top[k].bytes * SAMPLE >> 10),
                 (int)top[k].count * SAMPLE, where);
        put(line);
    }
}
