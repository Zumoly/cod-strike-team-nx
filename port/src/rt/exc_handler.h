// The fault handler's interface; adapted from the mc3_nx android32 runtime (MIT).
#ifndef PORT_EXC_HANDLER_H
#define PORT_EXC_HANDLER_H
#include <stddef.h>
#include <stdint.h>
#include <switch.h>

// Exception types (the kernel's r0 at entry).
#define EXC_INSTRUCTION_ABORT 0x100
#define EXC_DATA_ABORT 0x101
#define EXC_UNALIGNED_INSTRUCTION 0x102
#define EXC_UNALIGNED_DATA 0x103
#define EXC_UNDEFINED_INSTRUCTION 0x104
#define EXC_EXCEPTION_INSTRUCTION 0x105
#define EXC_MEMORY_SYSTEM_ERROR 0x106
#define EXC_FPU 0x200
#define EXC_INVALID_SYSCALL 0x301
#define EXC_SYSCALL_BREAK 0x302

// 0 resumes the thread with *info and *frame.  Anything else ends the process.
Result dcr_exception_dispatch(uint32_t type, ThreadExceptionInfo32 *info, ThreadExceptionFrame32 *frame);

// Register n (0-15) of the faulting thread: writable, and what it resumes with.
uint32_t *rt_exc_reg(ThreadExceptionInfo32 *info, ThreadExceptionFrame32 *frame, unsigned n);

// Mapped and readable bytes from p, at most want.
size_t dcr_readable(uint32_t p, size_t want);

// Runs in exception context, before the crash report: no locks and no allocation.  Returns 1
// when the fault was handled and the thread should resume (src/loader/hw_fixups.c).
int port_exception_hook(uint32_t type, ThreadExceptionInfo32 *info, ThreadExceptionFrame32 *frame);

// game.log, flushed after a crash report.
extern int port_log_fd;

#endif
