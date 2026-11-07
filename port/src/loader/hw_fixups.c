// Instructions the game's ARMv7 code uses that a real ARMv8 core may refuse.  Emulators run them
// all, so nothing here is exercised in Ryujinx.  SWP is removed from ARMv8 and CP15 barriers trap
// unless the kernel allows them.  so_patch_kuser rewrites the barriers it can see, and this
// repairs the rest when they fault.
#include <stdatomic.h>
#include <switch.h>

#include "../rt/exc_handler.h"

int probe_fault_hook(ThreadExceptionInfo32 *info);

volatile uint32_t port_traps; // each one costs a trip through the kernel; reported in watch.log

#define IS_ARM_CP15_BARRIER(w)                                                  \
    (((w) & 0xFFFF0FFFu) == 0xEE070FBAu || ((w) & 0xFFFF0FFFu) == 0xEE070F9Au || \
     ((w) & 0xFFFF0FFFu) == 0xEE070F95u)

static int fix_arm(uint32_t w, ThreadExceptionInfo32 *info, ThreadExceptionFrame32 *frame)
{
    if (IS_ARM_CP15_BARRIER(w)) {
        atomic_thread_fence(memory_order_seq_cst);
        info->pc += 4;
        return 1;
    }
    if ((w & 0xFFB00FF0u) == 0xE1000090u) { // swp and swpb, condition always
        uint32_t old, value = *rt_exc_reg(info, frame, w & 15);
        void *addr = (void *)(uintptr_t)*rt_exc_reg(info, frame, (w >> 16) & 15);
        if (w & (1u << 22))
            old = atomic_exchange((_Atomic uint8_t *)addr, (uint8_t)value);
        else
            old = atomic_exchange((_Atomic uint32_t *)addr, value);
        *rt_exc_reg(info, frame, (w >> 12) & 15) = old;
        info->pc += 4;
        return 1;
    }
    return 0;
}

int port_exception_hook(uint32_t type, ThreadExceptionInfo32 *info, ThreadExceptionFrame32 *frame)
{
    if (probe_fault_hook(info))
        return 1;
    // The kernel reports trapped instructions as instruction aborts.
    if (type != EXC_INSTRUCTION_ABORT && type != EXC_UNDEFINED_INSTRUCTION)
        return 0;
    if ((info->pc & 3) || (info->pstate & 0x20) || dcr_readable(info->pc, 4) < 4)
        return 0;
    port_traps++;
    return fix_arm(*(const volatile uint32_t *)(uintptr_t)info->pc, info, frame);
}
