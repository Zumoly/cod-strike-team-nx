// Horizon backend for code memory.  The rules here come from real hardware boots of the
// android32 ports (see src/rt/README.md):
//   * a code page made writable can never become executable again, so images are built
//     in plain heap staging memory and only then mapped as code;
//   * svcMapProcessCodeMemory needs a real process handle, not the pseudo-handle;
//   * RX is set first from the freshly mapped state, then RW for data.
// Under an emulator that takes the direct relocation path, code runs in place from the
// staging buffer, because it does not enforce no-execute on heap pages.
#ifdef __SWITCH__

#include "port/bionic_thread.h"
#include "port/so_loader.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "../rt/selfproc.h"

#define PAGE 0x1000u
#define RELOC_PATH_EMULATOR 2

extern volatile uint32_t __dcr_reloc_path __attribute__((visibility("hidden")));

static int in_place(void)
{
    return __dcr_reloc_path == RELOC_PATH_EMULATOR;
}

// Staging memory is donated to the code mapping, so it is never freed.
//
// Large libraries (libunity, and Mono's AOT images of the game's assemblies, about 45 MB of
// code in all) are placed on 2 MB boundaries, both the staging heap memory and the code
// address.  The kernel maps a 2 MB block as one TLB entry only when the virtual and physical
// addresses are aligned alike; in 4 KB pages a TLB miss costs 100-170 ns on the console
// (measured by the torchlight-nx port), and 45 MB of code is far beyond what the TLB covers
// in 4 KB pages.  The heap itself starts on a 2 MB boundary, so a 2 MB-aligned allocation
// in it is 2 MB-aligned physically as well, in the common case.
#define BIG 0x200000u
#define BIG_FROM 0x100000u // smaller libraries are not worth the up-to-2 MB of padding

void *so_code_reserve(size_t size, void **stage)
{
    size_t align = size >= BIG_FROM ? BIG : PAGE;
    size_t span = (size + align - 1) & ~(size_t)(align - 1);
    void *staging = memalign(align, span);
    if (!staging && align != PAGE) {
        align = PAGE;
        span = size;
        staging = memalign(PAGE, size);
    }
    if (!staging)
        return NULL;
    *stage = staging;
    if (in_place())
        return staging;

    virtmemLock();
    void *virt = virtmemFindCodeMemory(span + align - PAGE, PAGE);
    if (virt) {
        virt = (void *)(((uintptr_t)virt + align - 1) & ~(uintptr_t)(align - 1));
        virtmemAddReservation(virt, span);
    }
    virtmemUnlock();
    if (virt && align != PAGE)
        fprintf(stderr, "so_code_reserve: %zu KB at %p from %p, 2 MB aligned\n", size / 1024, virt, staging);
    return virt;
}

int so_code_commit(void *virt, void *stage, size_t size)
{
    armDCacheFlush(stage, size);
    if (in_place())
        return 0;
    Result rc = svcMapProcessCodeMemory(dcr_self_process(), (u64)(uintptr_t)virt, (u64)(uintptr_t)stage, size);
    if (R_FAILED(rc)) {
        fprintf(stderr, "so_code_commit: map at %p size 0x%zx failed 0x%x\n", virt, size, rc);
        return -1;
    }
    return 0;
}

int so_code_protect(void *addr, size_t size, int exec)
{
    if (in_place())
        return 0;
    Result rc = svcSetProcessMemoryPermission(dcr_self_process(), (u64)(uintptr_t)addr, size,
                                              exec ? Perm_Rx : Perm_Rw);
    if (R_FAILED(rc)) {
        fprintf(stderr, "so_code_protect: %s at %p size 0x%zx failed 0x%x\n", exec ? "RX" : "RW", addr, size, rc);
        return -1;
    }
    return 0;
}

// Full AOT means nothing asks for executable memory, so a request is a bug worth seeing.
// Mappings made by b_mem_map: start and size, open addressing on the start address.
#define MAP_SLOTS 16384
static struct {
    uintptr_t start;
    size_t size;
} g_maps[MAP_SLOTS];
static uint32_t g_map_lock;
uint32_t b_mem_mapped_bytes, b_mem_kept_bytes, b_mem_freed_bytes; // for the watchdog

static void map_lock(void)
{
    while (__atomic_exchange_n(&g_map_lock, 1, __ATOMIC_ACQUIRE))
        svcSleepThread(0);
}

static void map_unlock(void) { __atomic_store_n(&g_map_lock, 0, __ATOMIC_RELEASE); }

static unsigned map_slot(uintptr_t a) { return (unsigned)((a >> 12) * 2654435761u) % MAP_SLOTS; }

static void track_map(void *p, size_t size)
{
    map_lock();
    unsigned i = map_slot((uintptr_t)p);
    for (int n = 0; n < MAP_SLOTS; n++, i = (i + 1) % MAP_SLOTS)
        if (g_maps[i].start == 0 || g_maps[i].start == 1) {
            g_maps[i].start = (uintptr_t)p;
            g_maps[i].size = size;
            b_mem_mapped_bytes += size;
            break;
        }
    map_unlock();
}

// 1 if [p, p+size) covers a whole tracked region starting at p; the region is forgotten.
static int untrack_map(void *p, size_t size)
{
    int found = 0;
    map_lock();
    unsigned i = map_slot((uintptr_t)p);
    for (int n = 0; n < MAP_SLOTS && g_maps[i].start != 0; n++, i = (i + 1) % MAP_SLOTS)
        if (g_maps[i].start == (uintptr_t)p) {
            if (size >= g_maps[i].size) {
                b_mem_mapped_bytes -= g_maps[i].size;
                b_mem_freed_bytes += g_maps[i].size;
                g_maps[i].start = 1; // tombstone keeps later probes going
                found = 1;
            }
            break;
        }
    map_unlock();
    return found;
}

void *b_mem_map(size_t size, int prot)
{
    if (prot & 4) {
        fprintf(stderr, "b_mem_map: executable mapping refused\n");
        return NULL;
    }
    size = (size + PAGE - 1) & ~(size_t)(PAGE - 1);
    void *p = memalign(PAGE, size);
    if (p) {
        memset(p, 0, size);
        track_map(p, size);
    }
    return p;
}

// A whole region unmapped from its start goes back to the heap.  A partial unmap (Mono trims an
// over-sized mapping to align it) cannot return memory to a heap allocator, so the region stays.
// Run 13: freeing nothing here leaked the heap at ~0.2 MB/s in survival until malloc failed.
int b_mem_unmap(void *addr, size_t size)
{
    size = (size + PAGE - 1) & ~(size_t)(PAGE - 1);
    if (untrack_map(addr, size))
        free(addr);
    else
        b_mem_kept_bytes += size;
    return 0;
}

int b_mem_protect(void *addr, size_t size, int prot)
{
    (void)addr;
    (void)size;
    (void)prot;
    return 0; // heap pages stay RW; only guard-page behaviour is lost
}

void b_sleep_ns(int64_t ns)
{
    uint32_t spill[8]; // see futex_wait
    __asm__ volatile("stmia %0, {r4-r11}" : : "r"(spill) : "memory");
    b_blocking_enter(spill);
    svcSleepThread(ns);
    b_blocking_exit();
}

// Mono has nothing to flush in full AOT: precompiled code is never rewritten.
void port_icache_flush(void *start, int size)
{
    (void)start;
    (void)size;
}

#endif
