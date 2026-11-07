// What happens when a thread faults; entered from exc32.S.  Adapted from the mc3_nx android32
// runtime (MIT).  A fault the port can repair resumes the thread.  Anything else is a crash:
// registers, module offsets and a stack scan go to crash.log, then a failure result hands the
// fault back to the kernel, which ends the process and lets Atmosphere write its own report.
//
// No newlib or port locks here: the faulting thread may hold any of them.  The file is written
// through the FS service directly.
#include "exc_handler.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>

#include "port/so_loader.h"

#define CRASH_PATH "/switch/cod_st/crash.log"

_Static_assert(sizeof(ThreadExceptionInfo32) == 0x44, "the kernel's 32-bit ExceptionInfo");
_Static_assert(sizeof(ThreadExceptionFrame32) == 288, "must match exc32.S");

int port_log_fd = -1;
extern volatile uint32_t b_gc_suspends;

uint32_t *rt_exc_reg(ThreadExceptionInfo32 *i, ThreadExceptionFrame32 *f, unsigned n)
{
    if (n < 8)
        return &i->r[n];
    if (n < 13)
        return &f->r8_12[n - 8];
    if (n == 13)
        return &i->sp;
    if (n == 14)
        return &i->lr;
    return &i->pc;
}

extern char _start[];
extern char __rodata_start[] __attribute__((visibility("hidden"))); // end of our .text (dcr32.ld)
static char g_crash[8192];
static size_t g_crash_len;

static void out(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof line)
        n = sizeof line - 1;
    svcOutputDebugString(line, (u32)n);
    if (g_crash_len + (size_t)n < sizeof g_crash) {
        memcpy(g_crash + g_crash_len, line, (size_t)n);
        g_crash_len += (size_t)n;
    }
}

size_t dcr_readable(uint32_t p, size_t want)
{
    MemoryInfo mi;
    u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, p)) || !(mi.perm & Perm_R) || mi.type == MemType_Unmapped)
        return 0;
    uint64_t end = mi.addr + mi.size;
    if (end <= p)
        return 0;
    return end - p < want ? (size_t)(end - p) : want;
}

static int in_self(uint32_t a)
{
    return a >= (uint32_t)(uintptr_t)_start && a < (uint32_t)(uintptr_t)__rodata_start;
}

// so_describe prints a bare hex address for anything outside a module.
static int in_module(uint32_t a, char *buf, size_t cap)
{
    return so_describe(a, buf, cap)[0] != '0';
}

static const char *where(uint32_t a, char *buf, size_t cap)
{
    if (in_module(a, buf, cap))
        return buf;
    if (in_self(a))
        snprintf(buf, cap, "cod_st+0x%lx", (unsigned long)(a - (uint32_t)(uintptr_t)_start));
    else
        snprintf(buf, cap, "?");
    return buf;
}

static int is_code(uint32_t a)
{
    char buf[96];
    return in_module(a, buf, sizeof buf) || in_self(a);
}

static void write_crash_file(void)
{
    FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs)
        return;
    fsFsCreateFile(fs, CRASH_PATH, 0, 0);
    FsFile file;
    if (R_FAILED(fsFsOpenFile(fs, CRASH_PATH, FsOpenMode_Write | FsOpenMode_Append, &file)))
        return;
    s64 size = 0;
    fsFileGetSize(&file, &size);
    fsFileWrite(&file, size, g_crash, g_crash_len, FsWriteOption_Flush);
    fsFileClose(&file);
}

static const char *type_name(uint32_t t)
{
    switch (t) {
    case EXC_INSTRUCTION_ABORT: return "instruction abort";
    case EXC_DATA_ABORT: return "data abort";
    case EXC_UNALIGNED_INSTRUCTION: return "unaligned instruction";
    case EXC_UNALIGNED_DATA: return "unaligned data";
    case EXC_UNDEFINED_INSTRUCTION: return "undefined instruction";
    case EXC_EXCEPTION_INSTRUCTION: return "exception instruction";
    case EXC_MEMORY_SYSTEM_ERROR: return "memory system error";
    case EXC_FPU: return "FPU exception";
    case EXC_INVALID_SYSCALL: return "invalid system call";
    case EXC_SYSCALL_BREAK: return "svcBreak";
    default: return "exception";
    }
}

static void crash_report(uint32_t type, ThreadExceptionInfo32 *i, ThreadExceptionFrame32 *f)
{
    char w1[96], w2[96];
    u64 tid = 0;
    svcGetThreadId(&tid, CUR_THREAD_HANDLE);
    g_crash_len = 0;
    out("\n[crash] ===== %s (0x%lx) in thread %llu, after %lu GC thread stops =====\n", type_name(type),
        (unsigned long)type, (unsigned long long)tid, (unsigned long)b_gc_suspends);
    out("[crash] pc  %08lx  %s%s\n", (unsigned long)i->pc, where(i->pc, w1, sizeof w1),
        (i->pstate & 0x20) ? " (Thumb)" : "");
    out("[crash] lr  %08lx  %s\n", (unsigned long)i->lr, where(i->lr, w2, sizeof w2));
    out("[crash] far %08lx  esr %08lx  pstate %08lx\n", (unsigned long)i->far, (unsigned long)i->esr,
        (unsigned long)i->pstate);
    for (unsigned r = 0; r < 13; r += 4) {
        char line[128];
        int n = 0;
        for (unsigned k = r; k < r + 4 && k < 13; k++)
            n += snprintf(line + n, sizeof line - n, " r%-2u %08lx", k, (unsigned long)*rt_exc_reg(i, f, k));
        out("[crash]%s\n", line);
    }
    out("[crash] sp  %08lx\n", (unsigned long)i->sp);
    if (!(i->pstate & 0x20) && dcr_readable(i->pc & ~3u, 4))
        out("[crash] insn %08lx\n", (unsigned long)*(const volatile uint32_t *)(i->pc & ~3u));

    // Return addresses on the stack (heuristic: words that point into code).
    MemoryInfo mi;
    u32 pi;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, i->sp)) && mi.perm & Perm_R) {
        uint32_t end = (uint32_t)(mi.addr + mi.size), shown = 0;
        for (uint32_t a = i->sp & ~3u; a + 4 <= end && a < i->sp + 0x2000 && shown < 32; a += 4) {
            uint32_t v = *(const volatile uint32_t *)a;
            if (is_code(v & ~1u)) {
                out("[crash]   #%-2lu %08lx  %s\n", (unsigned long)shown, (unsigned long)v,
                    where(v & ~1u, w1, sizeof w1));
                shown++;
            }
        }
    }
    write_crash_file();
    // Horizon's file cache can hold the tail of game.log until a flush.
    if (port_log_fd >= 0) {
        fflush(stderr);
        fsync(port_log_fd);
    }
}

Result dcr_exception_dispatch(uint32_t type, ThreadExceptionInfo32 *info, ThreadExceptionFrame32 *frame)
{
    if (port_exception_hook(type, info, frame))
        return 0;
    crash_report(type, info, frame);
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
}
