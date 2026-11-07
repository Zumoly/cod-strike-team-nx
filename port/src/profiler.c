// Finds where a thread's time goes by sampling its program counter.  Each report lists the share of
// samples per module and the busiest 1 KB blocks of code, as module+offset.
#include "port/bionic_thread.h"
#include "port/profiler.h"
#include "port/so_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUCKETS 1024
#define BUCKET_SHIFT 10
#define SHOWN_BLOCKS 16
#define MAX_MODULES 16
#define NAME_SIZE 40

struct bucket {
    uint32_t base, count;
};

static struct bucket g_buckets[BUCKETS];
static uint32_t g_samples, g_dropped;

void profiler_sample(b_pthread_t t)
{
    uint32_t regs[16];
    if (!b_gc_pause_begin())
        return; // the collector has the thread stopped, and only it may restart it
    b_thread_pause(t, regs);
    b_thread_unpause(t);
    b_gc_pause_end();

    uint32_t base = regs[15] >> BUCKET_SHIFT;
    uint32_t at = (base * 2654435761u) % BUCKETS;
    for (uint32_t probe = 0; probe < BUCKETS; probe++) {
        struct bucket *b = &g_buckets[(at + probe) % BUCKETS];
        if (b->count == 0 || b->base == base) {
            b->base = base;
            b->count++;
            g_samples++;
            return;
        }
    }
    g_dropped++;
}

static int by_count(const void *a, const void *b)
{
    uint32_t x = ((const struct bucket *)a)->count, y = ((const struct bucket *)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void module_of(uint32_t addr, char *name, char *where, size_t size)
{
    so_describe(addr, where, size);
    const char *plus = strchr(where, '+');
    size_t n = plus ? (size_t)(plus - where) : 0;
    if (n >= NAME_SIZE)
        n = NAME_SIZE - 1;
    memcpy(name, where, n);
    name[n] = 0;
    if (!plus)
        strcpy(name, "port");
}

void profiler_report(void (*put)(const char *))
{
    static struct bucket sorted[BUCKETS];
    static struct {
        char name[NAME_SIZE];
        uint32_t count;
    } modules[MAX_MODULES];
    char line[160], where[96], name[NAME_SIZE];
    if (!g_samples)
        return;
    memcpy(sorted, g_buckets, sizeof sorted);
    qsort(sorted, BUCKETS, sizeof *sorted, by_count);

    int used = 0;
    memset(modules, 0, sizeof modules);
    for (int i = 0; i < BUCKETS && sorted[i].count; i++) {
        module_of(sorted[i].base << BUCKET_SHIFT, name, where, sizeof where);
        int m = 0;
        while (m < used && strcmp(modules[m].name, name) != 0)
            m++;
        if (m == used) {
            if (used == MAX_MODULES)
                continue;
            strcpy(modules[used++].name, name);
        }
        modules[m].count += sorted[i].count;
    }
    snprintf(line, sizeof line, "--- profile: %u samples, code anchor %p\n", (unsigned)g_samples, (void *)profiler_sample);
    put(line);
    for (int m = 0; m < used; m++) {
        snprintf(line, sizeof line, "  %3u%% %.39s\n", (unsigned)(modules[m].count * 100u / g_samples), modules[m].name);
        put(line);
    }
    for (int i = 0; i < SHOWN_BLOCKS && sorted[i].count; i++) {
        so_describe(sorted[i].base << BUCKET_SHIFT, where, sizeof where);
        snprintf(line, sizeof line, "  %3u.%u%% %s\n", (unsigned)(sorted[i].count * 100u / g_samples),
                 (unsigned)(sorted[i].count * 1000u / g_samples % 10u), where);
        put(line);
    }
    memset(g_buckets, 0, sizeof g_buckets);
    g_samples = g_dropped = 0;
}
