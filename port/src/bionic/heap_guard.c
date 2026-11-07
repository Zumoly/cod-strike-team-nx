// Heap guard for the game's libraries (their malloc, calloc, realloc, memalign and free).
//
// Survival crashed at wave 4 or 5 on every long run, inside newlib's malloc or Unity's allocator,
// with the heap's free lists already damaged.  Something writes where it should not.  Each block
// handed to the game here carries:
//   * a header before it: magic, size, the real block, and the caller's return address;
//   * a 16-byte guard pattern after it, checked when the block is freed or reallocated: a broken
//     guard means the block was written past its end, and the log names who allocated it;
//   * on free, a stay in a quarantine filled with a pattern, checked when it leaves: a changed
//     byte means something wrote to the block after freeing it.
// Both also keep those stray writes away from newlib's own bookkeeping.  Port code keeps using
// newlib directly; a pointer without this header (the port's strdup, vasprintf, JNI buffers) is
// passed to newlib's free unchanged.
//
// sd:/switch/cod_st/no_heap_guard turns all of it off.
#include "port/bionic_thread.h"
#include "port/so_loader.h"

#include <malloc.h>
#include <reent.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#define MAGIC_LIVE 0x47554152u // "GUAR"
#define MAGIC_FREE 0x46524545u // "FREE"
#define GUARD_BYTES 16
#define GUARD_FILL 0xA7
#define FREED_FILL 0xD9
#define HEADER 16
#define QUARANTINE_SLOTS 4096
#define QUARANTINE_MAX_BLOCK (64 * 1024)     // larger blocks are checked and freed at once
#define QUARANTINE_MAX_BYTES (8 * 1024 * 1024) // the heap is only ~960 MB; second map loads came close
#define MAX_REPORTS 60

struct header {
    uint32_t magic;
    uint32_t size;
    void *real;
    uint32_t caller; // the game's return address, for the report
};
_Static_assert(sizeof(struct header) == HEADER, "header layout");

static int g_enabled = -1;
static atomic_flag g_lock = ATOMIC_FLAG_INIT; // held for a few stores only

static void lock(void)
{
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire))
        b_sched_yield();
}

static void unlock(void) { atomic_flag_clear_explicit(&g_lock, memory_order_release); }
static void *g_slots[QUARANTINE_SLOTS];
static unsigned g_head, g_count;
static size_t g_held;
static unsigned g_reports;
uint32_t heap_guard_overruns, heap_guard_late_writes, heap_guard_bad_frees;

// Off unless sd:/switch/cod_st/heap_guard exists: the corruption it was written to find was GL ids
// reused while live (run 14), and its aligned blocks cost ~60 MB and fragmented the heap.
static int enabled(void)
{
    if (g_enabled < 0)
        g_enabled = access("sdmc:/switch/cod_st/heap_guard", F_OK) == 0;
    return g_enabled;
}

// devkitARM's malloc turns every request of 32 bytes or more into memalign(32), which in newlib's
// allocator splits off a leading fragment each time: by wave 5 the heap had 115 MB free in pieces
// and a 1 MB Unity allocation failed.  The game gets newlib's plain allocator instead, with the
// 8-byte alignment Android 4's dlmalloc gave it.
void *_malloc_r(struct _reent *, size_t);
void *_calloc_r(struct _reent *, size_t, size_t);
void *_realloc_r(struct _reent *, void *, size_t);
void _free_r(struct _reent *, void *);
// src/bionic/alloc_track.c, in the console build only.
void alloc_track_record(void *p, size_t size, uint32_t caller) __attribute__((weak));
void alloc_track_forget(void *p) __attribute__((weak));

static void *tracked(void *p, size_t n, uint32_t caller)
{
    if (alloc_track_record)
        alloc_track_record(p, n, caller);
    return p;
}

#define plain_malloc(n) tracked(_malloc_r(_REENT, (n)), (n), caller)
#define plain_calloc(n, m) tracked(_calloc_r(_REENT, (n), (m)), (n) * (m), caller)
static void *plain_realloc_impl(void *p, size_t n, uint32_t caller)
{
    if (p && alloc_track_forget)
        alloc_track_forget(p);
    void *q = _realloc_r(_REENT, p, n);
    return tracked(q ? q : p, q ? n : (p ? malloc_usable_size(p) : 0), caller);
}
#define plain_realloc(p, n) plain_realloc_impl((p), (n), caller)

static struct header *header_of(void *user) { return (struct header *)((uint8_t *)user - HEADER); }

// newlib's heap on the console; anything outside it was never malloc's to give or take back.
#ifdef __SWITCH__
extern char *fake_heap_start, *fake_heap_end;
static int in_heap(const void *p)
{
    return (uintptr_t)p >= (uintptr_t)fake_heap_start + 64 && (uintptr_t)p < (uintptr_t)fake_heap_end;
}
#else
static int in_heap(const void *p) { return p != NULL; }
#endif

// The header of a page-aligned block sits on the page before it, which for a foreign pointer may not
// be mapped (the loader maps libraries' code out of heap staging blocks, leaving holes).
static int previous_page_readable(const void *p)
{
#ifdef __SWITCH__
    MemoryInfo mi;
    u32 page;
    if (R_FAILED(svcQueryMemory(&mi, &page, (u32)((uintptr_t)p - HEADER))))
        return 0;
    return (mi.perm & Perm_R) != 0 && mi.type != MemType_Unmapped;
#else
    (void)p;
    return 1;
#endif
}

static int is_guarded(void *user, uint32_t magic)
{
    // Every block made here is 16-byte aligned; anything else is not one, and reading before it
    // could leave mapped memory (the first build of this file faulted there at start-up).
    if (!user || ((uintptr_t)user & 15) || !in_heap(user))
        return 0;
    if (((uintptr_t)user & 0xFFF) == 0 && !previous_page_readable(user))
        return 0;
    const struct header *h = header_of(user);
    return h->magic == magic && h->real != NULL && (uintptr_t)h->real < (uintptr_t)user &&
           (uintptr_t)user - (uintptr_t)h->real < 4096 + HEADER;
}

static void report(const char *what, const struct header *h, void *user, uint32_t offset, uint8_t found)
{
    if (g_reports++ >= MAX_REPORTS)
        return;
    char where[96];
    so_describe(h->caller, where, sizeof where);
    fprintf(stderr, "heap_guard: %s: block %p, %u bytes, allocated from %s; first changed byte at +%u is 0x%02x\n",
            what, user, (unsigned)h->size, where, (unsigned)offset, found);
}

// Live bytes per allocating call site, to name what keeps growing (run 12: the heap rose ~0.5 MB/s
// in a survival game until malloc failed).  Open addressing; a full table just stops adding sites.
#define SITES 4096
static struct site {
    uint32_t caller;
    int32_t count;
    int64_t bytes;
} g_sites[SITES];
static int64_t g_live_bytes;

static void account(uint32_t caller, int64_t bytes, int count)
{
    lock();
    g_live_bytes += bytes;
    unsigned i = (caller >> 2) * 2654435761u % SITES;
    for (int n = 0; n < 64; n++, i = (i + 1) % SITES) {
        if (g_sites[i].caller == caller || g_sites[i].caller == 0) {
            g_sites[i].caller = caller;
            g_sites[i].bytes += bytes;
            g_sites[i].count += count;
            break;
        }
    }
    unlock();
}

// Called by the watchdog every minute.
void heap_guard_report(void (*put)(const char *))
{
    char line[200];
    struct mallinfo mi = mallinfo();
    snprintf(line, sizeof line, "heap: in use %u MB, free in heap %u MB, game blocks %u MB, quarantine %u MB\n",
             (unsigned)(mi.uordblks >> 20), (unsigned)(mi.fordblks >> 20), (unsigned)(g_live_bytes >> 20),
             (unsigned)(g_held >> 20));
    put(line);
    if (!enabled())
        return;
    struct site top[10] = {{0}};
    lock();
    for (int i = 0; i < SITES; i++) {
        if (!g_sites[i].caller)
            continue;
        struct site s = g_sites[i];
        for (int k = 0; k < 10; k++)
            if (s.bytes > top[k].bytes) {
                struct site t = top[k];
                top[k] = s;
                s = t;
            }
    }
    unlock();
    for (int k = 0; k < 10 && top[k].caller; k++) {
        char where[96];
        so_describe(top[k].caller, where, sizeof where);
        snprintf(line, sizeof line, "  %6u KB in %6d blocks from %s\n", (unsigned)(top[k].bytes >> 10),
                 (int)top[k].count, where);
        put(line);
    }
}

static void *guarded_alloc(size_t size, size_t align, uint32_t caller)
{
    if (size > 0x7FFF0000u)
        return NULL;
    if (align > 4096) // is_guarded expects the header within a page of the real block
        return memalign(align, size);
    if (align < HEADER)
        align = HEADER;
    size_t lead = align; // room for the header, keeping the user block aligned
    uint8_t *real = memalign(align, lead + size + GUARD_BYTES);
    if (!real)
        return NULL;
    uint8_t *user = real + lead;
    struct header *h = header_of(user);
    h->magic = MAGIC_LIVE;
    h->size = (uint32_t)size;
    h->real = real;
    h->caller = caller;
    memset(user + size, GUARD_FILL, GUARD_BYTES);
    account(caller, (int64_t)size, 1);
    return user;
}

// Returns 1 if the guard after the block is intact.
static int check_guard(struct header *h, uint8_t *user)
{
    const uint8_t *g = user + h->size;
    for (int i = 0; i < GUARD_BYTES; i++)
        if (g[i] != GUARD_FILL) {
            heap_guard_overruns++;
            report("written past its end", h, user, h->size + (uint32_t)i, g[i]);
            return 0;
        }
    return 1;
}

static void release(void *user)
{
    struct header *h = header_of(user);
    if (h->size <= QUARANTINE_MAX_BLOCK) {
        const uint8_t *p = user;
        for (uint32_t i = 0; i < h->size; i++)
            if (p[i] != FREED_FILL) {
                heap_guard_late_writes++;
                report("written after it was freed", h, user, i, p[i]);
                break;
            }
    }
    h->magic = 0;
    free(h->real);
}

static void guarded_free(void *user, uint32_t caller)
{
    struct header *h = header_of(user);
    check_guard(h, user);
    account(h->caller, -(int64_t)h->size, -1);
    if (h->size > QUARANTINE_MAX_BLOCK) {
        h->magic = 0;
        free(h->real);
        return;
    }
    memset(user, FREED_FILL, h->size);
    h->magic = MAGIC_FREE;
    h->caller = caller; // from here on, the report names who freed it
    lock();
    void *evict = NULL;
    if (g_count == QUARANTINE_SLOTS || g_held + h->size > QUARANTINE_MAX_BYTES) {
        evict = g_slots[g_head];
        g_held -= header_of(evict)->size;
        g_head = (g_head + 1) % QUARANTINE_SLOTS;
        g_count--;
    }
    g_slots[(g_head + g_count) % QUARANTINE_SLOTS] = user;
    g_count++;
    g_held += h->size;
    unlock();
    if (evict)
        release(evict);
}

static void plain_free(void *p, uint32_t caller)
{
    if (!in_heap(p)) {
        // newlib would take the bytes before it for a chunk header and link a foreign block into its
        // free lists: the heap damage this file was written to find.  Left alone instead.
        heap_guard_bad_frees++;
        static unsigned shown;
        if (shown++ < 20) {
            char where[96];
            so_describe(caller, where, sizeof where);
            fprintf(stderr, "heap_guard: free of %p, outside the heap, from %s: ignored\n", p, where);
        }
        return;
    }
    if (is_guarded(p, MAGIC_FREE)) {
        // Freed twice: the first free put it in quarantine, so this one must not reach newlib.
        heap_guard_bad_frees++;
        if (g_reports++ < MAX_REPORTS) {
            char where[96];
            so_describe(caller, where, sizeof where);
            fprintf(stderr, "heap_guard: block %p freed twice, again from %s\n", p, where);
        }
        return;
    }
    free(p);
}

// ---- what the game's imports point at ----------------------------------------------------

void *hg_malloc(size_t n)
{
    uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
    return enabled() ? guarded_alloc(n, 8, caller) : plain_malloc(n);
}

void *hg_calloc(size_t n, size_t m)
{
    uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
    if (!enabled())
        return plain_calloc(n, m);
    if (m && n > 0xFFFFFFFFu / m)
        return NULL;
    void *p = guarded_alloc(n * m, 8, caller);
    if (p)
        memset(p, 0, n * m);
    return p;
}

void *hg_memalign(size_t align, size_t n)
{
    uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
    return enabled() ? guarded_alloc(n, align, caller) : memalign(align, n);
}

void hg_free(void *p)
{
    uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
    if (!p)
        return;
    if (enabled() && is_guarded(p, MAGIC_LIVE))
        guarded_free(p, caller);
    else
        plain_free(p, caller);
}

void *hg_realloc(void *p, size_t n)
{
    uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
    if (!enabled() || (p && !is_guarded(p, MAGIC_LIVE)))
        return plain_realloc(p, n);
    if (!p)
        return guarded_alloc(n, 8, caller);
    if (n == 0) {
        guarded_free(p, caller);
        return NULL;
    }
    struct header *h = header_of(p);
    check_guard(h, p);
    void *q = guarded_alloc(n, 8, h->caller);
    if (!q)
        return NULL;
    memcpy(q, p, h->size < n ? h->size : n);
    guarded_free(p, caller);
    return q;
}
