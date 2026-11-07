// The installed title's program: a 32-bit NRO loader.
//
// sphaira and hbmenu start NROs inside nx-hbloader, which is 64-bit and cannot run AArch32 code: the
// CPU mode is fixed when the kernel creates the process, from the NPDM.  This program is created
// 32-bit (it shares the port's NPDM) and does what hbloader does for one NRO, sdmc:/switch/cod_st/
// cod_st.nro, so the title is installed once and the port is updated by replacing that file.
//
// What it hands the NRO (docs/PORTING.md, "32-bit NRO"):
//   * the image mapped as code (svcMapProcessCodeMemory from the heap, then text RX, ro R, data RW);
//   * entry at the image's offset 0 with r0 = config entries, r1 = -1;
//   * config: main thread, own process handle, the rest of the heap as a heap override (the port's
//     nx_init.c uses it instead of svcSetHeapSize), all syscalls available, applet type Application;
//   * kernel exceptions: they enter this program's crt0, which branches to the NRO's (exc_forward.S).
// It does not relocate the NRO: the NRO's crt0_reloc.c does that itself.
#include <switch.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/rt/rt_settings.h"

#define NRO_PATH "sdmc:/switch/cod_st/cod_st.nro"
#define LOG_PATH "sdmc:/switch/cod_st/loader.log"

// The kernel's NRO layout (switch/nro.h has it too, kept here for the 32-bit offsets).
typedef struct {
    u32 file_off, size;
} Segment;
typedef struct {
    u32 unused;
    u32 mod_offset;
    u32 padding[2];
} NroStartHeader;
typedef struct {
    u32 magic; // "NRO0"
    u32 version, size, flags;
    Segment segments[3]; // text, ro, data
    u32 bss_size;
} NroHeaderStart;

// Read by exc_forward.S: where to send kernel exceptions once the NRO is in place.
__attribute__((visibility("hidden"))) volatile uint32_t g_nro_entry;

// Our own small heap, out of .bss: the process heap is the NRO's.
static u8 g_small_heap[1 << 20] __attribute__((aligned(16)));
void __libnx_initheap(void)
{
    extern char *fake_heap_start, *fake_heap_end;
    fake_heap_start = (char *)g_small_heap;
    fake_heap_end = (char *)g_small_heap + sizeof g_small_heap;
}

// Only what reading the file needs; the NRO starts every service itself.
void __appInit(void)
{
    if (R_FAILED(smInitialize()))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
    if (R_SUCCEEDED(fsInitialize()))
        fsdevMountSdmc();
}

void __appExit(void)
{
    fsdevUnmountAll();
    fsExit();
    smExit();
}

static FILE *g_log;
#define LOG(...)                                                                                         \
    do {                                                                                                 \
        if (g_log) {                                                                                     \
            fprintf(g_log, __VA_ARGS__);                                                                 \
            fflush(g_log);                                                                               \
        }                                                                                                \
    } while (0)

static void __attribute__((noreturn)) fail(u32 code, Result rc)
{
    LOG("loader: failed at step %u, rc 0x%x\n", (unsigned)code, (unsigned)rc);
    if (g_log)
        fclose(g_log);
    // Atmosphere's crash report shows these as Break Address / Break Size.
    svcBreak(0, 0xC0D50000u | code, rc);
    for (;;) {
    }
}

// A free range in the 32-bit code region [0x200000, 0x40000000), where the kernel takes
// AliasCode mappings for 32-bit processes (see src/rt/nx32_virtmem.c), with guard pages.
static u32 find_free_code_range(u32 size)
{
    u32 a = 0x00200000u;
    while (a < 0x40000000u) {
        MemoryInfo mi;
        u32 page;
        if (R_FAILED(svcQueryMemory(&mi, &page, a)))
            return 0;
        u64 end = mi.addr + mi.size;
        if (mi.type == MemType_Unmapped) {
            u64 start = ((u64)a + 0x10000u + 0xFFFFu) & ~0xFFFFull;
            if (end > start && end - start >= (u64)size + 0x10000u)
                return (u32)start;
        }
        if (end <= a || end > 0x40000000ull)
            return 0;
        a = (u32)end;
    }
    return 0;
}

// The NRO returns here if its main returns: end the process, as an NSO would.
static void loader_return(void)
{
    svcExitProcess();
}

static ConfigEntry g_config[10];
static char g_argv[] = NRO_PATH;
static const char g_loader_info[] = "cod_st 32-bit NRO loader\n";

extern volatile uint32_t __dcr_self_handle __attribute__((visibility("hidden")));

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    g_log = fopen(LOG_PATH, "w");
    LOG("loader: start\n");

    FILE *f = fopen(NRO_PATH, "rb");
    if (!f)
        fail(1, 0);
    NroStartHeader start;
    NroHeaderStart hdr;
    if (fread(&start, sizeof start, 1, f) != 1 || fread(&hdr, sizeof hdr, 1, f) != 1 ||
        hdr.magic != 0x304F524Eu /* "NRO0" */)
        fail(2, 0);
    for (int i = 0; i < 3; i++)
        if (hdr.segments[i].file_off & 0xFFF)
            fail(3, hdr.segments[i].file_off);
    u32 rw_size = (hdr.segments[2].size + hdr.bss_size + 0xFFF) & ~0xFFFu;
    u32 image_size = hdr.segments[2].file_off + rw_size;
    LOG("loader: %s: file %u bytes, text %u ro %u data %u bss %u, image %u\n", NRO_PATH, (unsigned)hdr.size,
        (unsigned)hdr.segments[0].size, (unsigned)hdr.segments[1].size, (unsigned)hdr.segments[2].size,
        (unsigned)hdr.bss_size, (unsigned)image_size);

    // The process heap, sized as the port's nx_init.c sizes it: the image at its start, the rest
    // for the port.
    u64 total = 0, used = 0, heap_region = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&heap_region, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0);
    u64 want = total > used ? total - used : 0;
    const u64 reserve = (u64)RT_GFX_RESERVE_MB << 20;
    want = want > reserve ? want - reserve : want / 2;
    if (heap_region && want > heap_region)
        want = heap_region;
    want &= ~0x1FFFFFull;
    void *heap = NULL;
    Result rc = svcSetHeapSize(&heap, (u32)want);
    for (u64 next = want >> 1; R_FAILED(rc) && next >= (64ull << 20); next >>= 1) {
        want = next & ~0x1FFFFFull;
        rc = svcSetHeapSize(&heap, (u32)want);
    }
    if (R_FAILED(rc))
        fail(4, rc);
    LOG("loader: heap %p, %u MB\n", heap, (unsigned)(want >> 20));

    u8 *image = heap;
    if (fseek(f, 0, SEEK_SET) != 0 || fread(image, 1, hdr.size, f) != hdr.size)
        fail(5, 0);
    fclose(f);
    memset(image + hdr.size, 0, image_size > hdr.size ? image_size - hdr.size : 0);

    Handle self = __dcr_self_handle ? __dcr_self_handle : envGetOwnProcessHandle();
    if (!self || self == INVALID_HANDLE)
        fail(6, 0);
    u32 base = find_free_code_range(image_size);
    if (!base)
        fail(7, image_size);
    rc = svcMapProcessCodeMemory(self, base, (u64)(uintptr_t)image, image_size);
    if (R_FAILED(rc))
        fail(8, rc);
    rc = svcSetProcessMemoryPermission(self, base + hdr.segments[0].file_off, hdr.segments[0].size, Perm_Rx);
    if (R_SUCCEEDED(rc))
        rc = svcSetProcessMemoryPermission(self, base + hdr.segments[1].file_off, hdr.segments[1].size, Perm_R);
    if (R_SUCCEEDED(rc))
        rc = svcSetProcessMemoryPermission(self, base + hdr.segments[2].file_off, rw_size, Perm_Rw);
    if (R_FAILED(rc))
        fail(9, rc);
    LOG("loader: image at 0x%x, process handle 0x%x\n", (unsigned)base, (unsigned)self);

    int n = 0;
    g_config[n++] = (ConfigEntry){EntryType_MainThreadHandle, 0, {envGetMainThreadHandle(), 0}};
    g_config[n++] = (ConfigEntry){EntryType_ProcessHandle, 0, {self, 0}};
    g_config[n++] = (ConfigEntry){EntryType_AppletType, 0, {AppletType_Application, 0}};
    g_config[n++] = (ConfigEntry){EntryType_OverrideHeap, 0,
                                  {(uintptr_t)image + image_size, want - image_size}};
    g_config[n++] = (ConfigEntry){EntryType_SyscallAvailableHint, 0, {UINT64_MAX, UINT64_MAX}};
    g_config[n++] = (ConfigEntry){EntryType_SyscallAvailableHint2, 0, {UINT64_MAX, 0}};
    g_config[n++] = (ConfigEntry){EntryType_Argv, 0, {0, (uintptr_t)g_argv}};
    g_config[n++] = (ConfigEntry){EntryType_EndOfList, 0, {(uintptr_t)g_loader_info, sizeof g_loader_info - 1}};
    LOG("loader: entering the NRO\n");
    if (g_log)
        fclose(g_log);
    g_log = NULL;

    // Everything this program opened is closed: the NRO starts sm, fs and the rest itself.
    __appExit();
    g_nro_entry = base;
    __asm__ volatile("dmb ish" ::: "memory");

    register u32 cfg __asm__("r4") = (u32)(uintptr_t)g_config;
    register u32 ret __asm__("r5") = (u32)(uintptr_t)loader_return;
    register u32 entry __asm__("r6") = base;
    __asm__ volatile("mov r0, r4\n\t"
                     "mvn r1, #0\n\t"
                     "mov lr, r5\n\t"
                     "bx r6"
                     :
                     : "r"(cfg), "r"(ret), "r"(entry)
                     : "r0", "r1", "lr", "memory");
    __builtin_unreachable();
}
