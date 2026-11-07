// newlib/libsysbase system layer over raw Linux syscalls, for running the port's
// ARM code under user-mode emulation.  Nothing here ships: it stands in for libnx.
#include "port/audio.h"
#include "port/bionic_thread.h"
#include "port/gles_backend.h"
#include "port/so_loader.h"

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <reent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

long sc(long n, long a, long b, long c, long d, long e, long f);
int qemu_clone(unsigned flags, void *stack, int *ptid, void *tls, int *ctid, void (*fn)(void *), void *arg);

#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_unlink 10
#define SYS_chdir 12
#define SYS_lseek 19
#define SYS_mkdir 39
#define SYS_rmdir 40
#define SYS_rename 38
#define SYS_nanosleep 162
#define SYS_mmap2 192
#define SYS_munmap 91
#define SYS_mprotect 125
#define SYS_stat64 195
#define SYS_fstat64 197
#define SYS_getdents64 217
#define SYS_futex 240
#define SYS_exit_group 248
#define SYS_clock_gettime 263
#define SYS_sched_yield 158
#define SYS_set_tls 0xF0005
#define SYS_ftruncate64 194
#define SYS_tkill 238

#define L_O_CREAT 0100
#define L_O_EXCL 0200
#define L_O_TRUNC 01000
#define L_O_APPEND 02000
#define L_O_DIRECTORY 040000
#define L_O_LARGEFILE 0400000

#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129

// ---- futex and time --------------------------------------------------------

struct l_ts {
    long sec, nsec;
};

int futex_wait(_Atomic uint32_t *w, uint32_t expected, int64_t timeout_ns)
{
    struct l_ts ts, *p = NULL;
    if (timeout_ns >= 0) {
        ts.sec = (long)(timeout_ns / 1000000000);
        ts.nsec = (long)(timeout_ns % 1000000000);
        p = &ts;
    }
    long r = sc(SYS_futex, (long)w, FUTEX_WAIT_PRIVATE, (long)expected, (long)p, 0, 0);
    return r == -110 ? B_ETIMEDOUT : 0;
}

void futex_wake(_Atomic uint32_t *w, int count)
{
    sc(SYS_futex, (long)w, FUTEX_WAKE_PRIVATE, count, 0, 0, 0);
}

static int64_t clock_ns(int id)
{
    struct l_ts ts;
    sc(SYS_clock_gettime, id, (long)&ts, 0, 0, 0, 0);
    return (int64_t)ts.sec * 1000000000 + ts.nsec;
}

int64_t b_realtime_ns(void) { return clock_ns(0); }
int64_t b_monotonic_ns(void) { return clock_ns(1); }

void b_sleep_ns(int64_t ns)
{
    struct l_ts ts = {(long)(ns / 1000000000), (long)(ns % 1000000000)};
    sc(SYS_nanosleep, (long)&ts, 0, 0, 0, 0, 0);
}

void qemu_yield(void)
{
    sc(SYS_sched_yield, 0, 0, 0, 0, 0, 0);
}

// ---- TLS and threads -------------------------------------------------------

extern char __tdata_start[], __tdata_end[], __tbss_start[], __tbss_end[];

#define TLS_SIZE 0x8000
static __thread struct _reent t_reent;
static __thread int t_reent_ready;

static void *tls_alloc(void)
{
    // ARM TLS variant 1: thread pointer, 8 reserved bytes, then the TLS image.
    char *block = calloc(1, TLS_SIZE);
    size_t data = (size_t)(__tdata_end - __tdata_start);
    memcpy(block + 8, __tdata_start, data);
    return block;
}

struct _reent *__syscall_getreent(void)
{
    // A zeroed _reent has NULL stdio pointers, so it must be set up on first use.
    if (!t_reent_ready) {
        t_reent_ready = 1;
        _REENT_INIT_PTR(&t_reent);
    }
    return &t_reent;
}

#define CLONE_FLAGS 0x50F00u // VM FS FILES SIGHAND THREAD SYSVSEM
#define CLONE_SETTLS 0x80000u

int qemu_thread_start(void (*entry)(void *), void *arg, void *stack, uint32_t stack_size)
{
    void *tls = tls_alloc();
    char *top = (char *)(((uintptr_t)stack + stack_size) & ~(uintptr_t)15);
    return qemu_clone(CLONE_FLAGS | CLONE_SETTLS, top, NULL, tls, NULL, entry, arg) < 0 ? -1 : 0;
}

void qemu_thread_exit(void)
{
    for (;;)
        sc(SYS_exit, 0, 0, 0, 0, 0, 0);
}

// ---- memory ----------------------------------------------------------------

extern char *fake_heap_start, *fake_heap_end;

#define MMAP_RW_ANON(size, prot)                                                                 \
    sc(SYS_mmap2, 0, (long)(size), (prot), 0x22 /* PRIVATE|ANON */, -1, 0)

static void *anon(size_t size, int prot)
{
    long r = MMAP_RW_ANON(size, prot);
    return (r < 0 && r > -4096) ? NULL : (void *)r;
}

void *so_code_reserve(size_t size, void **stage)
{
    *stage = anon(size, 3);
    return *stage;
}

int so_code_commit(void *virt, void *stage, size_t size)
{
    (void)virt;
    (void)stage;
    (void)size;
    return 0;
}

int so_code_protect(void *addr, size_t size, int exec)
{
    return sc(SYS_mprotect, (long)addr, (long)size, exec ? 5 : 3, 0, 0, 0) == 0 ? 0 : -1;
}

void port_icache_flush(void *start, int size)
{
    (void)start;
    (void)size; // emulation observes code writes itself
}

// Set for full-AOT runs: executable mappings fail as they do on Horizon, which proves
// nothing still depends on the JIT.
int qemu_refuse_exec;

// As src/loader/so_backend_nx.c does it, so Boehm's heap gets the console's high addresses.
void *b_mem_map(size_t size, int prot)
{
    if (prot & 4) {
        if (qemu_refuse_exec) {
            static const char msg[] = "b_mem_map: executable mapping refused\n";
            sc(SYS_write, 2, (long)msg, sizeof msg - 1, 0, 0, 0);
            return NULL;
        }
        return anon(size, prot);
    }
    size = (size + 0xFFF) & ~(size_t)0xFFF;
    void *p = memalign(0x1000, size);
    if (p)
        memset(p, 0, size);
    return p;
}
static int in_heap(void *addr) { return (char *)addr >= fake_heap_start && (char *)addr < fake_heap_end; }
int b_mem_unmap(void *addr, size_t size)
{
    if (in_heap(addr))
        return 0;
    return sc(SYS_munmap, (long)addr, (long)size, 0, 0, 0, 0) == 0 ? 0 : -1;
}
int b_mem_protect(void *addr, size_t size, int prot)
{
    if (in_heap(addr))
        return 0;
    return sc(SYS_mprotect, (long)addr, (long)size, prot, 0, 0, 0) == 0 ? 0 : -1;
}

extern int qemu_refuse_exec;

// ---- GLES backend: no GPU here, but the front end and shader pack run for real -------

void gles_state_init(void);

void gles_backend_init(void) { gles_state_init(); }
void gles_backend_present(void) {}

// Plays nothing, but reports whether FMOD really mixed anything, and keeps real time.
int audio_out_open(void) { return 0; }

void audio_out_write(const int16_t *block)
{
    static unsigned blocks, loud;
    int peak = 0;
    for (int i = 0; i < AUDIO_BLOCK_FRAMES * 2; i++) {
        int v = block[i] < 0 ? -block[i] : block[i];
        if (v > peak)
            peak = v;
    }
    static int top;
    loud += peak > 16;
    if (peak > top)
        top = peak;
    if (++blocks % 200 == 0) {
        char line[80];
        int n = snprintf(line, sizeof line, "audio: %u blocks, %u with sound, loudest %d\n", blocks, loud, top);
        sc(SYS_write, 2, (long)line, n, 0, 0, 0);
    }
    b_sleep_ns((int64_t)AUDIO_BLOCK_FRAMES * 1000000000 / 48000);
}
void gb_set_screen_size(int width, int height) { (void)width; (void)height; }

const char *gb_pak_path(void) { return "host:/sd/shaders.pak"; }

static const uint8_t *g_pak;

void gb_pak_loaded(const uint8_t *pak, uint32_t code_offset, uint32_t code_size)
{
    g_pak = pak;
    (void)code_offset;
    (void)code_size;
}

void *gb_program_create(const uint8_t *pak, const struct pak_record *record)
{
    (void)pak;
    return (void *)record;
}

// Anything new lands here so tools/extract_shaders.py --seen can add it to the pack.
void gb_shader_missing(uint64_t key, const char *vs, const char *fs)
{
    fprintf(stderr, "gles: program %016llx is not in the pack\n", (unsigned long long)key);
    char path[80];
    snprintf(path, sizeof path, "host:/sd/seen/%016llx.txt", (unsigned long long)key);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n//=====FS=====\n%s", vs, fs);
        fclose(f);
    }
}

static uint64_t g_tex_bytes[32], g_buffer_bytes;
void gl_thread_check(const char *what);

// The real backend is single-threaded, so a second caller would corrupt its command buffer.
void gl_thread_check(const char *what)
{
    static b_pthread_t owner;
    static int told;
    if (!owner)
        owner = b_pthread_self();
    if (owner != b_pthread_self() && told++ < 8)
        fprintf(stderr, "census GL from a second thread: %s\n", what);
}
static unsigned g_tex_count[32];

void qemu_gl_report(void)
{
    for (int f = 0; f < 32; f++)
        if (g_tex_count[f])
            fprintf(stderr, "census format %d: %u levels, %llu MB\n", f, g_tex_count[f],
                    (unsigned long long)(g_tex_bytes[f] >> 20));
}

void gb_buffer_release(struct gb_buffer *b) { (void)b; }
void gb_tex_data(struct gb_texture *t, int face, int level, int w, int h, enum gb_format f, const void *data,
                 uint32_t size)
{
    (void)face;
    gl_thread_check("tex_data");
    g_tex_count[f]++;
    g_tex_bytes[f] += size;
    if (!data)
        fprintf(stderr, "census target storage %dx%d format %d\n", w, h, f);
    if (level == 0) {
        t->width = w;
        t->height = h;
        t->format = f;
    }
}
void gb_tex_sub(struct gb_texture *t, int face, int level, int x, int y, int w, int h, const void *data, uint32_t size)
{
    (void)t; (void)face; (void)level; (void)x; (void)y; (void)w; (void)h; (void)data; (void)size;
}
void gb_tex_mipmap(struct gb_texture *t) { (void)t; }
void gb_tex_release(struct gb_texture *t) { (void)t; }
void gb_copy_framebuffer(struct gb_texture *t, int face, int level, int xoff, int yoff, int x, int y, int w, int h)
{
    (void)t; (void)face; (void)level; (void)xoff; (void)yoff; (void)x; (void)y; (void)w; (void)h;
}
void gb_bind_target(struct gb_texture *c, struct gb_texture *d, struct gb_texture *s)
{
    static int shown;
    if ((c || d || s) && shown++ < 40)
        fprintf(stderr, "census target color %dx%d f%d depth %dx%d f%d stencil f%d\n", c ? c->width : 0,
                c ? c->height : 0, c ? (int)c->format : 0, d ? d->width : 0, d ? d->height : 0, d ? (int)d->format : 0,
                s ? (int)s->format : 0);
}
// ---- draw capture ------------------------------------------------------------------
// host:/sd/capture_frame holds a frame number; every draw of the first frame at or after it is
// written to host:/sd/capture.txt, with its vertices run through glstate_matrix_mvp (and the
// game's _DepthBand remap) on the CPU, to show what the GPU would clip.
static FILE *g_cap;
static int g_cap_state; // 0 unread, 1 waiting, 2 capturing, 3 done
static uint64_t g_cap_from, g_cap_frame;

static int capture_on(void)
{
    if (g_cap_state == 0) {
        g_cap_state = 3;
        FILE *f = fopen("host:/sd/capture_frame", "r");
        if (f) {
            unsigned long long n;
            if (fscanf(f, "%llu", &n) == 1) {
                g_cap_from = n;
                g_cap_state = 1;
            }
            fclose(f);
        }
    }
    uint64_t now = gles_frame_count();
    if (g_cap_state == 1 && now >= g_cap_from) {
        g_cap = fopen("host:/sd/capture.txt", "w");
        g_cap_state = g_cap ? 2 : 3;
        g_cap_frame = now;
    }
    if (g_cap_state == 2 && now != g_cap_frame) {
        fclose(g_cap);
        g_cap = NULL;
        g_cap_state = 3;
        fprintf(stderr, "capture: frame %llu written\n", (unsigned long long)g_cap_frame);
    }
    return g_cap_state == 2;
}

struct cap_named {
    char name[44];
    uint32_t value;
};

static uint64_t record_key(const struct pak_record *r)
{
    if (!g_pak)
        return 0;
    uint32_t n;
    memcpy(&n, g_pak + 4, 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off;
        memcpy(&off, g_pak + 16 + i * 16 + 8, 4);
        if (g_pak + off == (const uint8_t *)r) {
            uint64_t k;
            memcpy(&k, g_pak + 16 + i * 16, 8);
            return k;
        }
    }
    return 0;
}

// stage 0 vertex, 1 fragment; returns the block offset or -1.
static int uniform_offset(const struct pak_record *r, const char *name, int stage)
{
    const struct cap_named *n = (const struct cap_named *)(const void *)(r + 1);
    n += r->attribute_count + r->sampler_count;
    for (int i = 0; i < r->uniform_count; i++)
        if ((int)(n[i].value & 0xFFFF) == stage && strcmp(n[i].name, name) == 0)
            return (int)(n[i].value >> 16);
    return -1;
}

static int attribute_location(const struct pak_record *r, const char *name)
{
    const struct cap_named *n = (const struct cap_named *)(const void *)(r + 1);
    for (int i = 0; i < r->attribute_count; i++)
        if (strcmp(n[i].name, name) == 0)
            return (int)n[i].value;
    return -1;
}

static uint32_t index_at(const struct gb_draw *d, int i)
{
    const uint8_t *base = d->index_buffer ? d->index_buffer->data + (uintptr_t)d->indices : d->indices;
    if (d->index_type == 0x1403) {
        uint16_t v;
        memcpy(&v, base + i * 2, 2);
        return v;
    }
    if (d->index_type == 0x1401)
        return base[i];
    if (d->index_type == 0x1405) {
        uint32_t v;
        memcpy(&v, base + i * 4, 4);
        return v;
    }
    return (uint32_t)(d->first + i);
}

static float attrib_component(const struct gb_attrib *a, uint32_t vertex, int c)
{
    if (c >= a->components)
        return c == 3 ? 1.0f : 0.0f;
    int size = a->type == 0x1406 ? 4 : (a->type == 0x1402 || a->type == 0x1403) ? 2 : 1;
    int stride = a->stride ? a->stride : size * a->components;
    const uint8_t *p = (a->buffer ? a->buffer->data + (uintptr_t)a->pointer : (const uint8_t *)a->pointer) +
                       vertex * (uint32_t)stride + c * size;
    switch (a->type) {
    case 0x1406: { float f; memcpy(&f, p, 4); return f; }
    case 0x1402: { int16_t v; memcpy(&v, p, 2); return a->normalized ? v / 32767.0f : v; }
    case 0x1403: { uint16_t v; memcpy(&v, p, 2); return a->normalized ? v / 65535.0f : v; }
    case 0x1400: { int8_t v = (int8_t)*p; return a->normalized ? v / 127.0f : v; }
    default: return a->normalized ? *p / 255.0f : *p;
    }
}

static void print_vec(const struct gb_draw *d, const struct pak_record *r, const char *name)
{
    for (int stage = 0; stage < 2; stage++) {
        int o = uniform_offset(r, name, stage);
        uint32_t size = stage ? d->fs_block_size : d->vs_block_size;
        const uint8_t *block = stage ? d->fs_block : d->vs_block;
        if (o >= 0 && (uint32_t)o + 16 <= size) {
            const float *v = (const float *)(const void *)(block + o);
            fprintf(g_cap, "  %s[%s] = %g %g %g %g\n", name, stage ? "fs" : "vs", v[0], v[1], v[2], v[3]);
        }
    }
}

static void capture_draw(const struct gb_draw *d)
{
    const struct pak_record *r = d->program;
    const struct gb_state *s = d->state;
    fprintf(g_cap, "draw prog=%016llx mode=%x count=%d idx=%x | depth test=%d func=%x write=%d cull=%d face=%x front=%x"
                   " blend=%d %x/%x mask=%x vp=%d,%d,%d,%d\n",
            (unsigned long long)record_key(r), (unsigned)d->mode, d->count, (unsigned)d->index_type, s->depth_test,
            (unsigned)s->depth_func, s->depth_write, s->cull, (unsigned)s->cull_face, (unsigned)s->front_face,
            s->blend, (unsigned)s->blend_src_rgb, (unsigned)s->blend_dst_rgb, s->color_mask, s->viewport[0],
            s->viewport[1], s->viewport[2], s->viewport[3]);
    for (int i = 0; i < d->texture_count; i++)
        if (d->textures[i])
            fprintf(g_cap, "  tex%d fmt=%d %dx%d cube=%d levels=%d\n", i, (int)d->textures[i]->format,
                    d->textures[i]->width, d->textures[i]->height, d->textures[i]->cube, d->textures[i]->levels);
    for (int i = 0; i < d->attrib_count; i++) {
        const struct gb_attrib *a = &d->attribs[i];
        if (a->enabled)
            fprintf(g_cap, "  attr%d type=%x n=%d norm=%d stride=%d %s\n", i, (unsigned)a->type, a->components,
                    a->normalized, a->stride, a->buffer ? "vbo" : "client");
    }
    print_vec(d, r, "_DepthBand");
    print_vec(d, r, "_unity_FogParams");
    print_vec(d, r, "_unity_FogColor");
    print_vec(d, r, "_Color");
    print_vec(d, r, "_TintColor");
    int mo = uniform_offset(r, "glstate_matrix_mvp", 0);
    int loc = attribute_location(r, "_glesVertex");
    if (mo < 0 || loc < 0 || loc >= d->attrib_count || !d->attribs[loc].enabled ||
        (uint32_t)mo + 64 > d->vs_block_size) {
        fprintf(g_cap, "  (no mvp/vertex: mvp@%d vertex loc %d)\n", mo, loc);
        return;
    }
    const float *m = (const float *)(const void *)((const uint8_t *)d->vs_block + mo); // column-major
    int bo = uniform_offset(r, "_DepthBand", 0);
    const float *band = bo >= 0 && (uint32_t)bo + 16 <= d->vs_block_size
                            ? (const float *)(const void *)((const uint8_t *)d->vs_block + bo)
                            : NULL;
    int n = d->count < 4096 ? d->count : 4096, inside = 0, near_out = 0, far_out = 0, side_out = 0;
    float zmin = 1e30f, zmax = -1e30f;
    const struct gb_attrib *a = &d->attribs[loc];
    for (int i = 0; i < n; i++) {
        uint32_t vi = index_at(d, i);
        float p[4], c[4];
        for (int k = 0; k < 4; k++)
            p[k] = attrib_component(a, vi, k);
        if (band)
            p[3] = 1.0f;
        for (int row = 0; row < 4; row++)
            c[row] = m[row] * p[0] + m[4 + row] * p[1] + m[8 + row] * p[2] + m[12 + row] * p[3];
        if (band)
            c[2] = c[2] * band[2] + c[3] * band[1];
        float w = c[3];
        if (w > 0) {
            float zn = c[2] / w;
            if (zn < zmin) zmin = zn;
            if (zn > zmax) zmax = zn;
        }
        if (c[2] < -w) near_out++;
        else if (c[2] > w) far_out++;
        else if (c[0] < -w || c[0] > w || c[1] < -w || c[1] > w) side_out++;
        else inside++;
        if (i < 3)
            fprintf(g_cap, "  v%u pos %g %g %g -> clip %g %g %g %g\n", (unsigned)vi, p[0], p[1], p[2], c[0], c[1],
                    c[2], c[3]);
    }
    fprintf(g_cap, "  verts %d: inside %d, beyond near %d, beyond far %d, off-side %d, ndc z %g..%g\n", n, inside,
            near_out, far_out, side_out, zmin, zmax);
}

void gb_clear(const struct gb_state *s, int color, int depth, int stencil, const float rgba[4], float z, int sv)
{
    (void)stencil; (void)sv;
    if (capture_on())
        fprintf(g_cap, "clear color=%d (%.3f %.3f %.3f %.3f) depth=%d z=%.3f mask=%x\n", color, rgba[0], rgba[1],
                rgba[2], rgba[3], depth, z, s ? s->color_mask : 0);
}

// Reads what the GPU would, so a bad attribute or uniform address still shows up here.
void (*qemu_draw_hook)(void);

void gb_draw(const struct gb_draw *d)
{
    volatile uint32_t sink = 0;
    gl_thread_check("draw");
    if (qemu_draw_hook)
        qemu_draw_hook();
    if (capture_on())
        capture_draw(d);
    for (int i = 0; i < d->attrib_count; i++)
        if (d->attribs[i].enabled && d->attribs[i].buffer && d->attribs[i].buffer->data)
            sink += *(const uint8_t *)d->attribs[i].buffer->data;
    if (d->vs_block_size)
        sink += *(const uint8_t *)d->vs_block;
    if (d->index_type && d->index_buffer && d->index_buffer->data)
        sink += d->index_buffer->data[(uintptr_t)d->indices];
}

// ---- signals ---------------------------------------------------------------
// Real faults exist under emulation, so Mono's handlers are installed for real.  The
// frame layout is genuine Linux, which is what those handlers expect.  On Horizon
// the same handlers must be driven from src/rt/exc32.S instead.

struct l_sigaction {
    void (*handler)(int);
    uint32_t flags;
    void (*restorer)(void);
    uint32_t mask[2];
};

struct b_sigaction {
    void (*handler)(int);
    uint32_t mask, flags;
    void (*restorer)(void);
};

#define SYS_rt_sigaction 174
#define SYS_gettid 224
#define SA_RESTORER 0x04000000u
#define SA_SIGINFO_L 4u
#define SA_RESTART_L 0x10000000u // the pause signal must not fail reads on the Windows-mounted /sd
#define EINTR_L 4
#define SIG_PAUSE 12 // SIGUSR2: the harness's stand-in for svcSetThreadActivity

void qemu_on_pause(const uint32_t *regs);

// ucontext: flags, link, stack_t, then sigcontext whose r0 follows three header words.
static void pause_handler(int sig, void *info, char *ucontext)
{
    (void)sig;
    (void)info;
    qemu_on_pause((const uint32_t *)(ucontext + 32));
}

// Names the failing address.  Mono replaces this for SIGSEGV once it starts.
static void crash_handler(int sig, void *info, char *ucontext)
{
    (void)info;
    const uint32_t *r = (const uint32_t *)(ucontext + 32); // r0..r15
    char pc[96], lr[96], line[300];
    int n = snprintf(line, sizeof line,
                     "CRASH signal %d pc=%s lr=%s sp=%08x r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x\n", sig,
                     so_describe(r[15], pc, sizeof pc), so_describe(r[14], lr, sizeof lr), (unsigned)r[13],
                     (unsigned)r[0], (unsigned)r[1], (unsigned)r[2], (unsigned)r[3], (unsigned)r[4]);
    sc(SYS_write, 2, (long)line, n, 0, 0, 0);

    // No frame pointers across these libraries, so the stack is scanned for return
    // addresses: anything pointing into a loaded library right after a call.
    const uint32_t *stack = (const uint32_t *)(r[13] & ~3u);
    int shown = 0;
    for (int i = 0; i < 2048 && shown < 24; i++) {
        char where[96];
        so_describe(stack[i], where, sizeof where);
        if (strchr(where, '+') && (stack[i] & 3) == 0) {
            n = snprintf(line, sizeof line, "  stack[%d] %s\n", i, where);
            sc(SYS_write, 2, (long)line, n, 0, 0, 0);
            shown++;
        }
    }
    sc(SYS_exit_group, 139, 0, 0, 0, 0, 0);
}

void qemu_pause_init(void)
{
    struct l_sigaction crash = {(void (*)(int))crash_handler, SA_SIGINFO_L, NULL, {0, 0}};
    sc(SYS_rt_sigaction, 11, (long)&crash, 0, 8, 0, 0);
    sc(SYS_rt_sigaction, 4, (long)&crash, 0, 8, 0, 0);
    sc(SYS_rt_sigaction, 7, (long)&crash, 0, 8, 0, 0);

    struct l_sigaction k = {(void (*)(int))pause_handler, SA_SIGINFO_L | SA_RESTART_L, NULL, {0xFFFFFFFFu, 0xFFFFFFFFu}};
    sc(SYS_rt_sigaction, SIG_PAUSE, (long)&k, 0, 8, 0, 0);
}

int qemu_gettid(void)
{
    return (int)sc(SYS_gettid, 0, 0, 0, 0, 0, 0);
}

void qemu_kick(int linux_tid)
{
    sc(SYS_tkill, linux_tid, SIG_PAUSE, 0, 0, 0, 0);
}

void b_sigaction_notify(int sig, const struct b_sigaction *act)
{
    if (sig != 4 && sig != 7 && sig != 8 && sig != 11)
        return;
    // In console-rules mode the guest's handlers stay out of the way, as they will on
    // Horizon, and the harness's own report shows the real failure.
    if (qemu_refuse_exec)
        return;
    struct l_sigaction k = {act->handler, act->flags & ~SA_RESTORER, NULL, {act->mask, 0}};
    sc(SYS_rt_sigaction, sig, (long)&k, 0, 8, 0, 0);
}

// ---- libsysbase hooks ------------------------------------------------------

void __syscall_exit(int rc)
{
    fflush(NULL);
    for (;;)
        sc(SYS_exit_group, rc, 0, 0, 0, 0, 0);
}

void __syscall_abort(void)
{
    static const char msg[] = "abort\n";
    sc(SYS_write, 2, (long)msg, sizeof msg - 1, 0, 0, 0);
    __syscall_exit(134);
    __builtin_unreachable();
}

int __syscall_gettod_r(struct _reent *r, struct timeval *tv, struct timezone *tz)
{
    (void)r;
    (void)tz;
    int64_t ns = b_realtime_ns();
    tv->tv_sec = ns / 1000000000;
    tv->tv_usec = (long)((ns % 1000000000) / 1000);
    return 0;
}

void __syscall_lock_init(_LOCK_T *l) { *l = 0; }
void __syscall_lock_close(_LOCK_T *l) { (void)l; }

void __syscall_lock_acquire(_LOCK_T *l)
{
    b_mutex_lock((b_mutex_t *)l);
}

int __syscall_lock_try_acquire(_LOCK_T *l)
{
    return b_mutex_trylock((b_mutex_t *)l) ? 1 : 0;
}

void __syscall_lock_release(_LOCK_T *l)
{
    b_mutex_unlock((b_mutex_t *)l);
}

// The first word doubles as a bionic-format recursive mutex.
static b_mutex_t *rec(_LOCK_RECURSIVE_T *l)
{
    uint32_t expected = 0;
    atomic_compare_exchange_strong((_Atomic uint32_t *)&l->lock, &expected, B_MUTEX_RECURSIVE_INIT);
    return (b_mutex_t *)&l->lock;
}

void __syscall_lock_init_recursive(_LOCK_RECURSIVE_T *l) { memset(l, 0, sizeof *l); }
void __syscall_lock_close_recursive(_LOCK_RECURSIVE_T *l) { (void)l; }
void __syscall_lock_acquire_recursive(_LOCK_RECURSIVE_T *l) { b_mutex_lock(rec(l)); }
int __syscall_lock_try_acquire_recursive(_LOCK_RECURSIVE_T *l) { return b_mutex_trylock(rec(l)) ? 1 : 0; }
void __syscall_lock_release_recursive(_LOCK_RECURSIVE_T *l) { b_mutex_unlock(rec(l)); }

static _LOCK_RECURSIVE_T g_malloc_lock;
void __syscall_malloc_lock(struct _reent *r) { (void)r; __syscall_lock_acquire_recursive(&g_malloc_lock); }
void __syscall_malloc_unlock(struct _reent *r) { (void)r; __syscall_lock_release_recursive(&g_malloc_lock); }

// ---- files: a devoptab over Linux -------------------------------------------

static const char *strip(const char *p)
{
    const char *c = strchr(p, ':');
    return c ? c + 1 : p;
}

static int result(struct _reent *r, long v)
{
    if (v < 0 && v > -4096) {
        // Linux numbers map onto the newlib values the bridge translates back.
        int e = (int)-v;
        r->_errno = e == 2 ? ENOENT : e == 16 ? EBUSY : e == 17 ? EEXIST : e == 39 ? ENOTEMPTY : e == 20 ? ENOTDIR : e == 21 ? EISDIR : EIO;
        return -1;
    }
    return (int)v;
}

struct l_stat64 {
    uint64_t st_dev;
    uint8_t pad0[4];
    uint32_t ino32;
    uint32_t mode, nlink, uid, gid;
    uint64_t rdev;
    uint8_t pad3[8];
    int64_t size;
    uint32_t blksize;
    uint8_t pad4[4];
    uint64_t blocks;
    uint32_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    uint64_t ino;
};

static void to_stat(const struct l_stat64 *l, struct stat *s)
{
    memset(s, 0, sizeof *s);
    s->st_mode = l->mode;
    s->st_size = l->size;
    s->st_ino = 0; // libnx's fsdev reports 0 for every file
    s->st_nlink = l->nlink;
    s->st_mtime = l->mtime;
}

// Horizon's filesystem rules, which Linux does not have.  One handle at a time may write a file,
// and once one does, no other handle can open it.  libnx's stat() opens the file, so stat by name
// fails on it too.  An open file cannot be deleted or renamed, and rename will not replace a target.
#define MAX_OPEN 64
static struct {
    int fd, writable;
    char path[160];
} g_open[MAX_OPEN];

static int open_state(const char *path)
{
    int state = 0; // 1 open, 2 open for writing
    for (int i = 0; i < MAX_OPEN; i++)
        if (g_open[i].path[0] && strcasecmp(g_open[i].path, path) == 0 && state < 1 + g_open[i].writable)
            state = 1 + g_open[i].writable;
    return state;
}

static int d_open(struct _reent *r, void *fs, const char *path, int flags, int mode)
{
    int state = open_state(strip(path));
    if (state == 2 || (state == 1 && (flags & 3))) {
        r->_errno = EBUSY;
        return -1;
    }
    int lf = flags & 3;
    if (flags & O_CREAT) lf |= L_O_CREAT;
    if (flags & O_EXCL) lf |= L_O_EXCL;
    if (flags & O_TRUNC) lf |= L_O_TRUNC;
    if (flags & O_APPEND) lf |= L_O_APPEND;
    long fd = sc(SYS_open, (long)strip(path), lf | L_O_LARGEFILE, mode, 0, 0, 0);
    if (result(r, fd) < 0)
        return -1;
    *(int *)fs = (int)fd;
    for (int i = 0; i < MAX_OPEN; i++)
        if (!g_open[i].path[0]) {
            g_open[i].fd = (int)fd;
            g_open[i].writable = (flags & 3) != 0;
            snprintf(g_open[i].path, sizeof g_open[i].path, "%s", strip(path));
            break;
        }
    return 0;
}

static int d_close(struct _reent *r, void *fd)
{
    for (int i = 0; i < MAX_OPEN; i++)
        if (g_open[i].path[0] && g_open[i].fd == *(int *)fd)
            g_open[i].path[0] = 0;
    return result(r, sc(SYS_close, *(int *)fd, 0, 0, 0, 0, 0));
}

static ssize_t d_write(struct _reent *r, void *fd, const char *p, size_t n)
{
    return result(r, sc(SYS_write, *(int *)fd, (long)p, (long)n, 0, 0, 0));
}

static ssize_t d_read(struct _reent *r, void *fd, char *p, size_t n)
{
    long got;
    do
        got = sc(SYS_read, *(int *)fd, (long)p, (long)n, 0, 0, 0);
    while (got == -EINTR_L);
    return result(r, got);
}

static off_t d_seek(struct _reent *r, void *fd, off_t pos, int dir)
{
    return result(r, sc(SYS_lseek, *(int *)fd, (long)pos, dir, 0, 0, 0));
}

static int d_fstat(struct _reent *r, void *fd, struct stat *s)
{
    struct l_stat64 l;
    if (result(r, sc(SYS_fstat64, *(int *)fd, (long)&l, 0, 0, 0, 0)) < 0)
        return -1;
    to_stat(&l, s);
    return 0;
}

static int d_stat(struct _reent *r, const char *path, struct stat *s)
{
    struct l_stat64 l;
    if (open_state(strip(path)) == 2) {
        r->_errno = EBUSY;
        return -1;
    }
    if (result(r, sc(SYS_stat64, (long)strip(path), (long)&l, 0, 0, 0, 0)) < 0)
        return -1;
    to_stat(&l, s);
    return 0;
}

static int d_unlink(struct _reent *r, const char *p)
{
    if (open_state(strip(p))) {
        r->_errno = EBUSY;
        return -1;
    }
    return result(r, sc(SYS_unlink, (long)strip(p), 0, 0, 0, 0, 0));
}
static int d_chdir(struct _reent *r, const char *p) { return result(r, sc(SYS_chdir, (long)strip(p), 0, 0, 0, 0, 0)); }
static int d_rmdir(struct _reent *r, const char *p) { return result(r, sc(SYS_rmdir, (long)strip(p), 0, 0, 0, 0, 0)); }

static int d_rename(struct _reent *r, const char *a, const char *b)
{
    struct l_stat64 l;
    if (open_state(strip(a))) {
        r->_errno = EBUSY;
        return -1;
    }
    if (sc(SYS_stat64, (long)strip(b), (long)&l, 0, 0, 0, 0) == 0) {
        r->_errno = EEXIST;
        return -1;
    }
    return result(r, sc(SYS_rename, (long)strip(a), (long)strip(b), 0, 0, 0, 0));
}

static int d_mkdir(struct _reent *r, const char *p, int mode)
{
    return result(r, sc(SYS_mkdir, (long)strip(p), mode, 0, 0, 0, 0));
}

static int d_ftruncate(struct _reent *r, void *fd, off_t len)
{
    return result(r, sc(SYS_ftruncate64, *(int *)fd, 0, (long)len, (long)(len >> 32), 0, 0));
}

struct dir_state {
    int fd, pos, end;
    char buf[2048];
};

static DIR_ITER *d_diropen(struct _reent *r, DIR_ITER *it, const char *path)
{
    struct dir_state *d = it->dirStruct;
    long fd = sc(SYS_open, (long)strip(path), L_O_DIRECTORY | L_O_LARGEFILE, 0, 0, 0, 0);
    if (result(r, fd) < 0)
        return NULL;
    d->fd = (int)fd;
    d->pos = d->end = 0;
    return it;
}

static int d_dirnext(struct _reent *r, DIR_ITER *it, char *name, struct stat *s)
{
    struct dir_state *d = it->dirStruct;
    if (d->pos >= d->end) {
        long n = sc(SYS_getdents64, d->fd, (long)d->buf, sizeof d->buf, 0, 0, 0);
        if (n <= 0) {
            r->_errno = ENOENT;
            return -1;
        }
        d->pos = 0;
        d->end = (int)n;
    }
    char *e = d->buf + d->pos;
    uint16_t reclen;
    memcpy(&reclen, e + 16, 2);
    uint8_t type = (uint8_t)e[18];
    strcpy(name, e + 19);
    memset(s, 0, sizeof *s);
    s->st_mode = type == 4 ? S_IFDIR : S_IFREG;
    d->pos += reclen;
    return 0;
}

static int d_dirclose(struct _reent *r, DIR_ITER *it)
{
    struct dir_state *d = it->dirStruct;
    return result(r, sc(SYS_close, d->fd, 0, 0, 0, 0, 0));
}

static const devoptab_t g_dev = {
    .name = "host",
    .structSize = sizeof(int),
    .open_r = d_open,
    .close_r = d_close,
    .write_r = d_write,
    .read_r = d_read,
    .seek_r = d_seek,
    .fstat_r = d_fstat,
    .stat_r = d_stat,
    .lstat_r = d_stat,
    .unlink_r = d_unlink,
    .chdir_r = d_chdir,
    .rename_r = d_rename,
    .mkdir_r = d_mkdir,
    .rmdir_r = d_rmdir,
    .ftruncate_r = d_ftruncate,
    .dirStateSize = sizeof(struct dir_state),
    .diropen_r = d_diropen,
    .dirnext_r = d_dirnext,
    .dirclose_r = d_dirclose,
};

static ssize_t std_write(struct _reent *r, void *fd, const char *p, size_t n)
{
    (void)fd;
    return result(r, sc(SYS_write, 2, (long)p, (long)n, 0, 0, 0));
}

static const devoptab_t g_stdio = {.name = "stdout", .write_r = std_write};

int main(void);

void qemu_main(void)
{
    // Everything else needs TLS, so it comes first.  Calloc is not usable yet.
    // .bss is not cleared here: the kernel already did, and this stack lives in it.
    static char main_tls[TLS_SIZE] __attribute__((aligned(16)));
    memcpy(main_tls + 8, __tdata_start, (size_t)(__tdata_end - __tdata_start));
    sc(SYS_set_tls, (long)main_tls, 0, 0, 0, 0, 0);

    // Where the console's heap landed (probe.log): most pointers are at or above 0xC0000000,
    // which Android and Ryujinx never produce.
    size_t heap = 1008u * 1024 * 1024;
    long at = sc(SYS_mmap2, 0xBF800000, (long)heap, 3, 0x32 /* PRIVATE|FIXED|ANON */, -1, 0);
    if (at != (long)0xBF800000)
        __syscall_exit(99);
    fake_heap_start = (char *)0xBF800000;
    fake_heap_end = fake_heap_start + heap;

    devoptab_list[STD_OUT] = &g_stdio;
    devoptab_list[STD_ERR] = &g_stdio;
    AddDevice(&g_dev);
    setvbuf(stderr, NULL, _IONBF, 0);
    __syscall_exit(main());
}
