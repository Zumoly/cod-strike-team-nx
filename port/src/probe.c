// One-shot hardware probe: each kernel behaviour the port depends on is tested here
// and written to the log.  Each result is synced first, because fflush alone left
// logs empty after a crash: everything before the failure must stay readable.
#include "port/bionic_thread.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>

#define KB 1024u
#define FAULT_NONE 0xFFFFFFFFu

static FILE *g_log;

#define LOG(...)                       \
    do {                               \
        fprintf(g_log, __VA_ARGS__);   \
        fputc('\n', g_log);            \
        fflush(g_log);                 \
        fsync(fileno(g_log));          \
    } while (0)

// ---- futex and core placement -----------------------------------------------

static b_mutex_t g_mutex;
static long g_counter;
static _Atomic uint32_t g_core_seen;

// A wrong WaitForAddress ABI does not crash: the call returns at once and locks
// quietly become spin loops.  Only blocking for the requested time proves it.
static void probe_futex(void)
{
    static _Atomic uint32_t word = 5;
    u32 fw = hosversionGet();
    LOG("futex: firmware %u.%u.%u", HOSVER_MAJOR(fw), HOSVER_MINOR(fw), HOSVER_MICRO(fw));

    int64_t t0 = b_monotonic_ns();
    int r = futex_wait(&word, 5, 20 * 1000 * 1000);
    int64_t blocked = b_monotonic_ns() - t0;
    LOG("futex: equal value r=%d (want %d) blocked=%lld us (want about 20000)", r, B_ETIMEDOUT,
        (long long)(blocked / 1000));

    t0 = b_monotonic_ns();
    r = futex_wait(&word, 6, 20 * 1000 * 1000);
    blocked = b_monotonic_ns() - t0;
    LOG("futex: other value r=%d (want 0) blocked=%lld us (want near 0)", r, (long long)(blocked / 1000));
}

static void *counter_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 200000; i++) {
        b_mutex_lock(&g_mutex);
        g_counter++;
        b_mutex_unlock(&g_mutex);
        if ((i & 0x3FFF) == 0)
            atomic_fetch_or(&g_core_seen, 1u << svcGetCurrentProcessorNumber());
    }
    return NULL;
}

static void probe_threads(void)
{
    b_pthread_t t[6];
    u64 start = armGetSystemTick();
    int created = 0;
    for (int i = 0; i < 6; i++)
        if (b_pthread_create(&t[i], NULL, counter_worker, NULL) == 0)
            created++;
    for (int i = 0; i < created; i++)
        b_pthread_join(t[i], NULL);
    LOG("threads: created=%d counter=%ld (want %ld) cores=0x%x ms=%llu", created, g_counter, created * 200000L,
        (unsigned)g_core_seen, (unsigned long long)(armTicksToNs(armGetSystemTick() - start) / 1000000));
}

// ---- stop-the-world prerequisites -------------------------------------------

static volatile int g_spin_run = 1;
static b_sem_t g_block_sem;

static void *spin_worker(void *arg)
{
    (void)arg;
    while (g_spin_run)
        ;
    return NULL;
}

static void *block_worker(void *arg)
{
    (void)arg;
    b_sem_wait(&g_block_sem);
    return NULL;
}

// Uses libnx threads directly because the probe needs the handle, which b_thread hides.
static void dump_thread(const char *name, void (*entry)(void *))
{
    Thread t;
    Result rc = threadCreate(&t, entry, NULL, NULL, 64 * KB, 0x2C, 1);
    if (R_FAILED(rc)) {
        LOG("suspend[%s]: create rc=0x%x", name, rc);
        return;
    }
    threadStart(&t);
    svcSleepThread(50 * 1000 * 1000);
    rc = threadPause(&t);
    LOG("suspend[%s]: pause rc=0x%x", name, rc);
    // Oversized on purpose: the kernel's 32-bit context layout is what is being measured.
    static u8 ctx_buf[0x400] __attribute__((aligned(16)));
    memset(ctx_buf, 0, sizeof ctx_buf);
    rc = threadDumpContext((ThreadContext *)ctx_buf, &t);
    LOG("suspend[%s]: dump rc=0x%x sizeof=%zu stack=%p+0x%zx", name, rc, sizeof(ThreadContext), t.stack_mirror,
        t.stack_sz);
    // Raw words: which registers the kernel fills for a thread inside an SVC is
    // the question, and it decides how the GC suspend bridge finds roots.
    const u32 *w = (const u32 *)ctx_buf;
    for (int i = 0; i < 40; i += 8)
        LOG("  ctx[%02d] %08x %08x %08x %08x %08x %08x %08x %08x", i, (unsigned)w[i], (unsigned)w[i + 1],
            (unsigned)w[i + 2], (unsigned)w[i + 3], (unsigned)w[i + 4], (unsigned)w[i + 5], (unsigned)w[i + 6],
            (unsigned)w[i + 7]);
    threadResume(&t);
    g_spin_run = 0;
    b_sem_post(&g_block_sem);
    threadWaitForExit(&t);
    threadClose(&t);
}

static void spin_entry(void *arg) { spin_worker(arg); }
static void block_entry(void *arg) { block_worker(arg); }

// ---- user exceptions, the replacement for SIGSEGV -----------------------------

static volatile u32 g_fault_far = FAULT_NONE;
static volatile u32 g_fault_pc = FAULT_NONE;
static volatile u32 g_fault_desc;
static volatile int g_fault_armed;

// Resumes only the probe's own deliberate read of NULL+4.
int probe_fault_hook(ThreadExceptionInfo32 *info)
{
    if (!g_fault_armed || info->far != 0x4)
        return 0;
    g_fault_armed = 0;
    g_fault_desc = info->esr;
    g_fault_pc = info->pc;
    g_fault_far = info->far;
    info->pc += 4;
    return 1;
}

static void __attribute__((noinline)) probe_fault(void)
{
    LOG("exception: reading NULL+4 now; if the log ends here, delivery failed");
    g_fault_armed = 1;
    volatile uintptr_t address = 4; // through a variable so GCC cannot fold the fault away
    (void)*(volatile u32 *)address;
    LOG("exception: resumed esr=0x%x pc=0x%x far=0x%x", (unsigned)g_fault_desc, (unsigned)g_fault_pc,
        (unsigned)g_fault_far);
}

// ---- address space ----------------------------------------------------------

static void probe_memory(void)
{
    static const struct {
        const char *name;
        u32 id;
    } items[] = {
        {"total", InfoType_TotalMemorySize},
        {"used", InfoType_UsedMemorySize},
        {"heap_base", InfoType_HeapRegionAddress},
        {"heap_size", InfoType_HeapRegionSize},
        {"aslr_base", InfoType_AslrRegionAddress},
        {"aslr_size", InfoType_AslrRegionSize},
        {"stack_base", InfoType_StackRegionAddress},
        {"stack_size", InfoType_StackRegionSize},
    };
    for (size_t i = 0; i < sizeof items / sizeof *items; i++) {
        u64 v = 0;
        Result rc = svcGetInfo(&v, items[i].id, CUR_PROCESS_HANDLE, 0);
        LOG("mem: %s=0x%llx rc=0x%x", items[i].name, (unsigned long long)v, rc);
    }
    size_t best = 0;
    // Largest single allocation decides whether Unity and Mono's heaps can fit.  Anything
    // above the 1 GiB heap wraps sbrk's address check and falsely succeeds.
    for (size_t mb = 960; mb >= 64; mb -= 64) {
        void *p = malloc(mb * KB * KB);
        if (p) {
            best = mb;
            free(p);
            break;
        }
    }
    LOG("mem: largest malloc=%zu MB", best);
}

void probe_run(const char *path)
{
    g_log = fopen(path, "w");
    if (!g_log)
        return;
    LOG("probe start, relocated image running");
    probe_memory();
    b_sem_init(&g_block_sem, 0, 0);
    probe_futex();
    probe_threads();
    dump_thread("spinning", spin_entry);
    g_spin_run = 1;
    // Reset: the first dump posted this, and the worker must really block in the kernel.
    b_sem_init(&g_block_sem, 0, 0);
    dump_thread("blocked-in-svc", block_entry);
    LOG("probe first part complete");
}

// Separate because either step can end the process, and neither may cost the
// game_load result that main() collects in between.
void probe_late(void)
{
    if (!g_log)
        return;
    probe_fault();
    LOG("probe complete");
    fclose(g_log);
    g_log = NULL;
}
