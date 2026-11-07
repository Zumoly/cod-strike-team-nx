// Writes sdmc:/switch/cod_st/watch.log once a second from a thread above the game's.  The file is
// flushed on every line, so it survives anything short of a dead SD card:
//   - lines stop at the moment the whole console died, and the last says what the GPU was doing;
//   - lines continuing with a stuck frame count mean only the game hung, and a dump shows where.
// It writes through the FS service directly, because newlib's stdio locks may be held by the very
// thread that has hung.
#include "port/bionic_thread.h"
#include "port/cpu_clock.h"
#include "port/gles_backend.h"
#include "port/profiler.h"
#include "port/so_loader.h"

#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>

#define PATH "/switch/cod_st/watch.log"
#define PRIORITY 0x2B
#define STALL_SECONDS 2
#define MAX_DUMPS 10
#define SAMPLE_MS 20
#define SAMPLES_PER_SECOND 50
#define REPORT_SECONDS 30
#define FIRST_DUMP_FRAME 30
#define MAX_THREAD_ID 64
#define STACK_WORDS 2048
#define RETURNS_SHOWN 14

extern volatile int port_frame;
extern b_pthread_t port_main_thread;
extern volatile uint32_t port_traps;
extern volatile uint32_t b_gc_suspends;
extern char *fake_heap_start;
size_t dcr_readable(uint32_t p, size_t want);

static Thread g_thread;
static FsFile g_file;
static s64 g_size;
static int g_open;

static void put(const char *text)
{
    size_t n = strlen(text);
    if (g_open && R_SUCCEEDED(fsFileWrite(&g_file, g_size, text, n, FsWriteOption_Flush)))
        g_size += (s64)n;
}

static void dump_thread(uint32_t id, b_pthread_t t)
{
    char line[200], a[96], b[96];
    uint32_t regs[16];
    if (!b_gc_pause_begin())
        return;
    b_thread_pause(t, regs);
    snprintf(line, sizeof line, "thread %u pc=%s lr=%s sp=%08x\n", (unsigned)id, so_describe(regs[15], a, sizeof a),
             so_describe(regs[14], b, sizeof b), (unsigned)regs[13]);
    put(line);
    uint32_t sp = regs[13] & ~3u;
    size_t bytes = dcr_readable(sp, STACK_WORDS * 4);
    int shown = 0;
    for (size_t i = 0; i + 4 <= bytes && shown < RETURNS_SHOWN; i += 4) {
        uint32_t v = *(const volatile uint32_t *)(uintptr_t)(sp + i);
        so_describe(v & ~1u, a, sizeof a);
        if (a[0] != '0') {
            snprintf(line, sizeof line, "    %s\n", a);
            put(line);
            shown++;
        }
    }
    b_thread_unpause(t);
    b_gc_pause_end();
}

// One line of milliseconds per frame over the last second, for the frame rate work.
static void report_frame_time(char *line, size_t size, int frame)
{
    static uint64_t last[4];
    static int last_frame;
    const uint64_t now[4] = {gb_stats.update_ticks, gb_stats.fence_ticks, gb_stats.record_ticks, gb_stats.acquire_ticks};
    unsigned ms[4];
    unsigned frames = frame > last_frame ? (unsigned)(frame - last_frame) : 1;
    for (int i = 0; i < 4; i++) {
        ms[i] = (unsigned)(armTicksToNs(now[i] - last[i]) / 1000000u / frames);
        last[i] = now[i];
    }
    last_frame = frame;
    snprintf(line, size, "  ms/frame: game=%u gpu_wait=%u draw_calls=%u swap_wait=%u traps=%u cpu=%uMHz\n", ms[0], ms[1], ms[2],
             ms[3], (unsigned)port_traps, cpu_clock_mhz());
    put(line);
}

static void watch(void *arg)
{
    (void)arg;
    int last_frame = -1, still = 0, dumps = 0;
    for (unsigned second = 0;; second++) {
        for (int i = 0; i < SAMPLES_PER_SECOND; i++) {
            svcSleepThread(SAMPLE_MS * 1000000ll);
            if (port_main_thread && port_frame >= FIRST_DUMP_FRAME)
                profiler_sample(port_main_thread);
        }
        if (second % REPORT_SECONDS == REPORT_SECONDS - 1)
            profiler_report(put);
        cpu_clock_keep();
        int frame = port_frame;
        char line[260];
        snprintf(line, sizeof line,
                 "t=%u frame=%d draws=%llu gpu=%s images=%uMB buffers=%uMB textures=%u uploads=%uMB flushes=%u "
                 "heap=%uMB gc=%u\n",
                 second, frame, (unsigned long long)gles_draw_count(), gb_stats.stage,
                 (unsigned)(gb_stats.image_bytes >> 20), (unsigned)(gb_stats.buffer_bytes >> 20),
                 (unsigned)gb_stats.textures, (unsigned)(gb_stats.upload_bytes >> 20), (unsigned)gb_stats.flushes,
                 (unsigned)(((char *)sbrk(0) - fake_heap_start) >> 20), (unsigned)b_gc_suspends);
        put(line);
        report_frame_time(line, sizeof line, frame);
        if (second % 30 == 29) {
            extern uint32_t gb_realizes, gb_unrealizes, gb_pending_bytes;
            extern void heap_guard_report(void (*)(const char *));
            snprintf(line, sizeof line, "textures: made %u, dropped %u, level data held %u KB\n",
                     (unsigned)gb_realizes, (unsigned)gb_unrealizes, (unsigned)(gb_pending_bytes >> 10));
            put(line);
            extern uint32_t b_mem_mapped_bytes, b_mem_kept_bytes, b_mem_freed_bytes;
            snprintf(line, sizeof line, "mmap: live %u MB, freed by munmap %u MB, partial unmaps kept %u MB\n",
                     (unsigned)(b_mem_mapped_bytes >> 20), (unsigned)(b_mem_freed_bytes >> 20),
                     (unsigned)(b_mem_kept_bytes >> 20));
            put(line);
            heap_guard_report(put);
            extern void alloc_track_report(void (*)(const char *));
            alloc_track_report(put);
        }

        // sd:/switch/cod_st/thread_dumps: every thread's state every 20 s, for waits that keep frames
        // coming (run 20: two minutes on the campaign loading screen with nothing logged).
        static int dumps_wanted = -1;
        if (dumps_wanted < 0)
            dumps_wanted = access("sdmc:/switch/cod_st/thread_dumps", F_OK) == 0;
        if (dumps_wanted && second % 20 == 19 && frame >= FIRST_DUMP_FRAME) {
            snprintf(line, sizeof line, "--- periodic dump at t=%u frame=%d: every thread\n", second, frame);
            put(line);
            for (uint32_t id = 0; id < MAX_THREAD_ID; id++) {
                b_pthread_t t = b_thread_by_id(id);
                if (t && b_thread_has_handle(t))
                    dump_thread(id, t);
            }
        }

        still = frame == last_frame ? still + 1 : 0;
        last_frame = frame;
        // Unity's startup legitimately takes seconds per frame.
        if (still == STALL_SECONDS && dumps < MAX_DUMPS && frame >= FIRST_DUMP_FRAME) {
            still = 0;
            dumps++;
            put("--- no new frame: every thread\n");
            for (uint32_t id = 0; id < MAX_THREAD_ID; id++) {
                b_pthread_t t = b_thread_by_id(id);
                if (t && b_thread_has_handle(t))
                    dump_thread(id, t);
            }
        }
    }
}

void watchdog_start(void)
{
    FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs)
        return;
    fsFsDeleteFile(fs, PATH);
    fsFsCreateFile(fs, PATH, 0, 0);
    g_open = R_SUCCEEDED(fsFsOpenFile(fs, PATH, FsOpenMode_Write | FsOpenMode_Append, &g_file));
    if (R_SUCCEEDED(threadCreate(&g_thread, watch, NULL, NULL, 0x10000, PRIORITY, -2)))
        threadStart(&g_thread);
}
