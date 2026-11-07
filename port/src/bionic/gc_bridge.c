// Boehm stop-the-world without signals.
//
// libmono's collector stops a thread with tkill(id, 30); the thread's signal handler
// records its stack pointer and registers, posts a semaphore, and waits for signal 24.
// Horizon has no signals, so this does the handler's work from outside: pause the
// thread, spill its registers onto its own stack, and fill in what the handler would.
//
// Offsets come from the handler at 0x2e0540 in this exact libmono, which
// b_gc_bridge_init verifies.  GC_thread: +12 last signal, +16 last stop count, +20 sp.
#include "port/bionic_fs.h"
#include "port/bionic_thread.h"
#include "port/so_loader.h"

#include <stdatomic.h>

#include <stdio.h>

#define SIG_SUSPEND 30
#define SIG_RESTART 24
#define ESRCH_L 3
#define ENOSYS_L 38
#define TID_BASE 1000 // b_gettid reports 1000 + thread id

#define GC_SIGNAL 3     // word index of +12
#define GC_STOP_COUNT 4 // +16
#define GC_STACK_PTR 5  // +20

#define HANDLER_GOT_PC 0x2e0548u    // add r4, pc, r4
#define LIT_GOT 0x2e065cu
#define LIT_STOP_COUNT 0x2e0660u
#define LIT_ACK_SEM 0x2e0674u
#define CHECK_SIG_CMP 0x2e0568u     // cmp r3, #30
#define CHECK_STORE_SP 0x2e05f0u    // str r2, [r3, #20]

volatile uint32_t b_gc_suspends; // reported in crash.log

static uint32_t *g_stop_count;
static b_sem_t *g_ack_sem;
static uint32_t *(*g_lookup)(b_pthread_t);

// Pausing a thread the collector has stopped would restart it mid-collection, so anything else
// that pauses threads takes turns with the collector here.
static atomic_int g_collector_pauses, g_other_pause;

int b_gc_pause_begin(void)
{
    atomic_store(&g_other_pause, 1);
    if (atomic_load(&g_collector_pauses) == 0)
        return 1;
    atomic_store(&g_other_pause, 0);
    return 0;
}

void b_gc_pause_end(void)
{
    atomic_store(&g_other_pause, 0);
}

static uint32_t text_word(const so_module *m, uint32_t offset)
{
    return *(const uint32_t *)(m->base + offset);
}

int b_gc_bridge_init(const so_module *mono)
{
    if (text_word(mono, CHECK_SIG_CMP) != 0xE353001Eu || text_word(mono, CHECK_STORE_SP) != 0xE5832014u) {
        fprintf(stderr, "gc_bridge: libmono does not match the analysed build\n");
        return -1;
    }
    uintptr_t got = mono->base + HANDLER_GOT_PC + text_word(mono, LIT_GOT);
    g_stop_count = *(uint32_t **)(got + text_word(mono, LIT_STOP_COUNT));
    g_ack_sem = *(b_sem_t **)(got + text_word(mono, LIT_ACK_SEM));
    g_lookup = (uint32_t * (*)(b_pthread_t)) so_symbol(mono, "GC_lookup_thread");

    uintptr_t lo = mono->base, hi = mono->base + mono->span;
    if ((uintptr_t)g_stop_count < lo || (uintptr_t)g_stop_count >= hi || (uintptr_t)g_ack_sem < lo ||
        (uintptr_t)g_ack_sem >= hi || !g_lookup) {
        fprintf(stderr, "gc_bridge: symbols resolved outside libmono\n");
        return -1;
    }
    return 0;
}

static void suspend_thread(b_pthread_t t, uint32_t *gc)
{
    uint32_t regs[16];
    b_gc_suspends++;
    atomic_fetch_add(&g_collector_pauses, 1);
    while (atomic_load(&g_other_pause))
        b_sched_yield();
    b_thread_pause(t, regs);

    // A thread parked in a blocking bridge call already spilled its callee-saved
    // registers there, and the kernel may not report them while it is in an SVC.
    uint32_t sp = (uint32_t)(uintptr_t)b_thread_blocked_sp(t);
    if (!sp) {
        // Below SP is unused on ARM, and the thread cannot run until resumed.
        sp = (regs[13] - 16 * sizeof(uint32_t)) & ~7u;
        uint32_t *spill = (uint32_t *)(uintptr_t)sp;
        for (int i = 0; i < 16; i++)
            spill[i] = regs[i];
    }
    gc[GC_STACK_PTR] = sp;
    gc[GC_STOP_COUNT] = *g_stop_count;
    gc[GC_SIGNAL] = 0;
    b_sem_post(g_ack_sem);
}

static void resume_thread(b_pthread_t t, uint32_t *gc)
{
    gc[GC_SIGNAL] = SIG_RESTART;
    b_thread_unpause(t);
    atomic_fetch_sub(&g_collector_pauses, 1);
    b_sem_post(g_ack_sem);
}

// Boehm is tkill's only caller in these libraries.
int b_tkill(int id, int sig)
{
    b_pthread_t t = b_thread_by_id((uint32_t)(id - TID_BASE));
    uint32_t *gc = (t && g_lookup) ? g_lookup(t) : NULL;
    if (!gc) {
        *b_errno() = ESRCH_L;
        return -1;
    }
    switch (sig) {
    case 0:
        return 0;
    case SIG_SUSPEND:
        suspend_thread(t, gc);
        return 0;
    case SIG_RESTART:
        resume_thread(t, gc);
        return 0;
    default:
        *b_errno() = ENOSYS_L;
        return -1;
    }
}
