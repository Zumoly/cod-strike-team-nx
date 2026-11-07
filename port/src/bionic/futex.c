#include "port/bionic_thread.h"

#include <limits.h>

#ifdef __SWITCH__
#include <switch.h>
#include <sys/time.h>

int futex_wait(_Atomic uint32_t *word, uint32_t expected, int64_t timeout_ns)
{
    // Kernel-side compare and sleep is atomic, like FUTEX_WAIT.  A value mismatch
    // returns InvalidState, which is a normal "retry" for every caller here.
    // The kernel may not report registers for a thread inside an SVC, so callee-saved
    // ones are spilled here, where the collector scans them.
    uint32_t spill[8];
    __asm__ volatile("stmia %0, {r4-r11}" : : "r"(spill) : "memory");
    b_blocking_enter(spill);
    Result rc = svcWaitForAddress((void *)word, ArbitrationType_WaitIfEqual, (s32)expected, timeout_ns);
    b_blocking_exit();
    return rc == KERNELRESULT(TimedOut) ? B_ETIMEDOUT : 0;
}

void futex_wake(_Atomic uint32_t *word, int count)
{
    svcSignalToAddress((void *)word, SignalType_Signal, 0, count == INT_MAX ? -1 : count);
}

int64_t b_monotonic_ns(void)
{
    return (int64_t)armTicksToNs(armGetSystemTick());
}

int64_t b_realtime_ns(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000000 + (int64_t)tv.tv_usec * 1000;
}

#elif defined(PORT_QEMU)
// Provided by the emulation harness, tests/qemu/sys.c.
#else // Linux host, used by tests/test_threads.c
#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

int futex_wait(_Atomic uint32_t *word, uint32_t expected, int64_t timeout_ns)
{
    struct timespec ts, *tsp = NULL;
    if (timeout_ns >= 0) {
        ts.tv_sec = (time_t)(timeout_ns / 1000000000);
        ts.tv_nsec = (long)(timeout_ns % 1000000000);
        tsp = &ts;
    }
    long r = syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, expected, tsp, NULL, 0);
    return (r < 0 && errno == ETIMEDOUT) ? B_ETIMEDOUT : 0;
}

void futex_wake(_Atomic uint32_t *word, int count)
{
    syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, count, NULL, NULL, 0);
}

static int64_t clock_ns(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int64_t b_monotonic_ns(void) { return clock_ns(CLOCK_MONOTONIC); }
int64_t b_realtime_ns(void) { return clock_ns(CLOCK_REALTIME); }
#endif
