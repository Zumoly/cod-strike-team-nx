// The deko3D side of src/gl/gles.c.
//
// Everything renders the way GL does, with row 0 at the bottom: the device uses an upper-left
// origin with a downward Y axis, so normalized -1 lands on row 0, and a texture rendered to reads
// back the same way a texture uploaded with glTexImage2D does.  The default framebuffer is an
// ordinary offscreen image, flipped into the swapchain at present.
//
// Two frames can be in flight.  Each owns a command buffer and a ring for per-draw data.  When a
// ring or command buffer fills mid-frame, flush_and_wait() submits, idles the GPU and starts over.
#include "port/gles_backend.h"
#include "port/renderer.h"

#include <deko3d.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>

#define NUM_IMAGES 2
#define NUM_FRAMES 2
#define CMD_SIZE (6u * 1024 * 1024)
#define CMD_CHUNK (128u * 1024)
#define CMD_MARGIN (256u * 1024)
#define RING_SIZE (32u * 1024 * 1024) // per frame; a 2048x2048 RGBA8 level (16 MB) still fits half of it
// GPU heap blocks come out of the 1 GB process heap in one piece each.  64 MB blocks left up to
// 63 MB unused per heap, and loading the campaign after survival needed one more 64 MB piece the
// fragmented heap did not have (run 18).  Smaller blocks, and empty ones are given back.
#define CHUNK_SIZE (16u * 1024 * 1024)
#define MAX_IMAGE_DESCRIPTORS 8192
#define MAX_SAMPLER_DESCRIPTORS 128
#define MAX_DEFERRED 8192
#define HEAP_AFTER_FRAMES 60 // unchanged this long, a buffer leaves the ring for the heap

#define PAK_PATH "sdmc:/switch/cod_st/shaders.pak"
#define MISS_DIR "sdmc:/switch/cod_st/shader_miss"

// ---- GPU memory heap ---------------------------------------------------------

struct free_range {
    uint32_t offset, size;
    struct free_range *next;
};

struct chunk {
    DkMemBlock mem;
    uint8_t *cpu;
    DkGpuAddr gpu;
    uint32_t size;
    struct free_range *free;
    struct chunk *next;
    struct heap *owner;
};

struct heap {
    uint32_t flags;
    struct chunk *chunks;
};

struct allocation {
    struct chunk *chunk;
    uint32_t offset, size;
};

struct deferred {
    uint64_t frame;
    struct allocation a;
    int descriptor;
};

struct frame {
    DkCmdBuf cmd;
    DkMemBlock cmd_mem;
    uint32_t cmd_used;
    DkMemBlock ring_mem;
    uint8_t *ring_cpu;
    DkGpuAddr ring_gpu;
    uint32_t ring_used;
    DkFence fence;
    int fence_pending;
};

struct gtex {
    DkImage image;
    DkImageLayout layout;
    struct allocation a;
    int descriptor;
    int realized, renderable, want_mipmaps;
    int queued; // in g_unrealized, waiting to be made at the end of the frame
    int blit_ok; // made with the 2D engine flag: may be a blit source or destination
    int levels; // in the image
    int layout_w, layout_h, faces; // what the image was made with, for clamping uploads
    DkImageFormat dk_format;
    DkImageSwizzle swizzle[4];
    struct {
        void *data;
        uint32_t size;
        int w, h;
    } pending[6][GB_MAX_LEVELS];
};

struct gprogram {
    DkShader shaders[2];
    const struct pak_record *record; // for the draw capture
};

static struct {
    DkDevice device;
    DkQueue queue;
    struct frame frames[NUM_FRAMES];
    int current;
    int frame_open;
    u64 frame_ended, recording_since;
    uint64_t frame_number;
    uint32_t epoch;

    DkMemBlock swap_mem;
    DkImage swap_images[NUM_IMAGES];
    DkSwapchain swapchain;
    DkImage back_color, back_depth;
    DkMemBlock back_mem;

    DkMemBlock descriptor_mem;
    DkGpuAddr image_descriptors_gpu, sampler_descriptors_gpu;
    DkImageDescriptor *image_descriptors;
    DkSamplerDescriptor *sampler_descriptors;
    int next_descriptor;
    int free_descriptors[MAX_IMAGE_DESCRIPTORS];
    int free_descriptor_count;
    struct {
        uint32_t min, mag, wrap_s, wrap_t, aniso;
    } samplers[MAX_SAMPLER_DESCRIPTORS];
    int sampler_count;
    int black_descriptor;

    struct heap buffer_heap, image_heap;
    struct deferred deferred[MAX_DEFERRED];
    int deferred_count;

    DkMemBlock code_mem;
    const uint8_t *pak;

    // Current GPU state, so unchanged state is not re-recorded.
    int all_dirty;
    struct gb_state bound;
    int target_is_set;
    struct gb_texture *color_target, *depth_target, *stencil_target;
    int target_width, target_height;
    int target_has_depth, target_has_stencil;
    const struct gprogram *bound_program;
    DkVtxAttribState attrib_states[GB_MAX_ATTRIBS];
    DkVtxBufferState buffer_states[GB_MAX_ATTRIBS];
    int attrib_count;
    uint32_t draws_skipped;
    int screen_width, screen_height;
} g;

struct gb_stats_t gb_stats = {.stage = "idle"};

static void gpu_error_report(const char *why);
static void event(const char *what, int a, int b, int c, int d, int e, int f);

// deko3d aborts the process right after an error with no return path (run 21: 2359-0001 from
// dkQueueAcquireImage), so the message and the last draws go to a file of their own first.
static void debug_callback(void *user, const char *context, DkResult result, const char *message)
{
    (void)user;
    char why[256];
    snprintf(why, sizeof why, "deko3d: %s: result %d: %s", context ? context : "?", (int)result,
             message ? message : "");
    fprintf(stderr, "%s\n", why);
    fflush(stderr);
    gpu_error_report(why);
}

static uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static struct frame *cur(void)
{
    return &g.frames[g.current];
}

static DkMemBlock make_block(uint32_t size, uint32_t flags)
{
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, g.device, align_up(size, DK_MEMBLOCK_ALIGNMENT));
    maker.flags = flags;
    DkMemBlock block = dkMemBlockCreate(&maker);
    if (!block)
        fprintf(stderr, "deko3d: cannot allocate a %u byte block\n", (unsigned)size);
    return block;
}

static int heap_alloc(struct heap *h, uint32_t size, uint32_t alignment, struct allocation *out)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct chunk *c = h->chunks; c; c = c->next) {
            struct free_range **link = &c->free;
            for (struct free_range *r = *link; r; link = &r->next, r = *link) {
                uint32_t start = align_up(r->offset, alignment);
                uint32_t pad = start - r->offset;
                if (pad + size > r->size)
                    continue;
                uint32_t tail = r->size - pad - size;
                uint32_t tail_offset = start + size;
                if (pad) {
                    r->size = pad;
                    if (tail) {
                        struct free_range *n = malloc(sizeof *n);
                        n->offset = tail_offset;
                        n->size = tail;
                        n->next = r->next;
                        r->next = n;
                    }
                } else if (tail) {
                    r->offset = tail_offset;
                    r->size = tail;
                } else {
                    *link = r->next;
                    free(r);
                }
                out->chunk = c;
                out->offset = start;
                out->size = size;
                return 0;
            }
        }
        uint32_t chunk_size = size + alignment > CHUNK_SIZE ? align_up(size + alignment, 0x10000) : CHUNK_SIZE;
        struct chunk *c = calloc(1, sizeof *c);
        c->mem = make_block(chunk_size, h->flags);
        if (!c->mem) {
            free(c);
            return -1;
        }
        c->cpu = (h->flags & DkMemBlockFlags_CpuAccessMask) ? dkMemBlockGetCpuAddr(c->mem) : NULL;
        c->gpu = dkMemBlockGetGpuAddr(c->mem);
        c->size = chunk_size;
        c->free = malloc(sizeof *c->free);
        c->free->offset = 0;
        c->free->size = chunk_size;
        c->free->next = NULL;
        c->owner = h;
        c->next = h->chunks;
        h->chunks = c;
    }
    return -1;
}

static void heap_free(const struct allocation *a)
{
    struct chunk *c = a->chunk;
    struct free_range **link = &c->free;
    while (*link && (*link)->offset < a->offset)
        link = &(*link)->next;
    struct free_range *n = malloc(sizeof *n);
    n->offset = a->offset;
    n->size = a->size;
    n->next = *link;
    *link = n;
    // Merge with the neighbours, which keeps first fit from fragmenting between levels.
    if (n->next && n->offset + n->size == n->next->offset) {
        struct free_range *x = n->next;
        n->size += x->size;
        n->next = x->next;
        free(x);
    }
    struct free_range *p = c->free;
    while (p && p->next != n && p != n)
        p = p->next;
    if (p && p != n && p->offset + p->size == n->offset) {
        p->size += n->size;
        p->next = n->next;
        free(n);
    }
    // A block with nothing left in it goes back to the process heap (one stays, to save churn).
    struct free_range *only = c->free;
    struct heap *h = c->owner;
    if (h && only && !only->next && only->offset == 0 && only->size == c->size && !(h->chunks == c && !c->next)) {
        struct chunk **link = &h->chunks;
        while (*link && *link != c)
            link = &(*link)->next;
        if (*link) {
            *link = c->next;
            event("gpu block freed", (int)c->size, h == &g.image_heap, 0, 0, 0, 0);
            dkMemBlockDestroy(c->mem);
            free(only);
            free(c);
        }
    }
}

// Freed only once the GPU cannot still be reading it.
static void defer_free(const struct allocation *a, int descriptor)
{
    if (g.deferred_count == MAX_DEFERRED) {
        dkQueueWaitIdle(g.queue);
        for (int i = 0; i < g.deferred_count; i++) {
            if (g.deferred[i].a.chunk)
                heap_free(&g.deferred[i].a);
            if (g.deferred[i].descriptor >= 0)
                g.free_descriptors[g.free_descriptor_count++] = g.deferred[i].descriptor;
        }
        g.deferred_count = 0;
    }
    struct deferred *d = &g.deferred[g.deferred_count++];
    d->frame = g.frame_number;
    d->a = *a;
    d->descriptor = descriptor;
}

static void run_deferred(void)
{
    int kept = 0;
    for (int i = 0; i < g.deferred_count; i++) {
        struct deferred *d = &g.deferred[i];
        if (g.frame_number >= d->frame + NUM_FRAMES + 1) {
            if (d->a.chunk)
                heap_free(&d->a);
            if (d->descriptor >= 0)
                g.free_descriptors[g.free_descriptor_count++] = d->descriptor;
        } else {
            g.deferred[kept++] = *d;
        }
    }
    g.deferred_count = kept;
}

// ---- frames ------------------------------------------------------------------

static void add_cmd_memory(void *user, DkCmdBuf cmd, size_t min)
{
    struct frame *f = user;
    uint32_t size = min > CMD_CHUNK ? align_up((uint32_t)min, 0x1000) : CMD_CHUNK;
    if (f->cmd_used + size > CMD_SIZE) {
        fprintf(stderr, "deko3d: command memory exhausted\n");
        size = CMD_SIZE - f->cmd_used; // the margin check keeps this from being reached
    }
    dkCmdBufAddMemory(cmd, f->cmd_mem, f->cmd_used, size);
    f->cmd_used += size;
}

static void reset_recording(struct frame *f)
{
    dkCmdBufClear(f->cmd);
    f->cmd_used = 0;
    f->ring_used = 0;
    g.epoch++;
    g.all_dirty = 1;
    g.bound_program = NULL;
    g.target_is_set = 0;
    dkCmdBufBindImageDescriptorSet(f->cmd, g.image_descriptors_gpu, MAX_IMAGE_DESCRIPTORS);
    dkCmdBufBindSamplerDescriptorSet(f->cmd, g.sampler_descriptors_gpu, MAX_SAMPLER_DESCRIPTORS);
    dkCmdBufBarrier(f->cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
}

static void begin_frame(void)
{
    if (g.frame_open)
        return;
    struct frame *f = cur();
    u64 opened = armGetSystemTick();
    gb_stats.update_ticks += opened - g.frame_ended;
    if (f->fence_pending) {
        gb_stats.stage = "waiting for frame fence";
        dkFenceWait(&f->fence, -1);
        f->fence_pending = 0;
    }
    g.recording_since = armGetSystemTick();
    gb_stats.fence_ticks += g.recording_since - opened;
    gb_stats.stage = "recording";
    g.frame_number++;
    run_deferred();
    reset_recording(f);
    g.frame_open = 1;
}

static void flush_and_wait(void)
{
    struct frame *f = cur();
    gb_stats.stage = "flush: GPU idle wait";
    gb_stats.flushes++;
    dkQueueSubmitCommands(g.queue, dkCmdBufFinishList(f->cmd));
    dkQueueWaitIdle(g.queue);
    gb_stats.stage = "recording";
    reset_recording(f);
}

static uint8_t *ring_alloc(uint32_t size, uint32_t alignment, DkGpuAddr *gpu)
{
    struct frame *f = cur();
    uint32_t start = align_up(f->ring_used, alignment);
    if (start + size > RING_SIZE) {
        flush_and_wait();
        start = 0;
    }
    f->ring_used = start + size;
    *gpu = f->ring_gpu + start;
    return f->ring_cpu + start;
}

// ---- initialization -----------------------------------------------------------

static void make_target_image(DkImage *image, DkImageFormat format, uint32_t flags, DkMemBlock mem, uint32_t offset)
{
    DkImageLayoutMaker maker;
    dkImageLayoutMakerDefaults(&maker, g.device);
    maker.flags = flags;
    maker.format = format;
    maker.dimensions[0] = RENDERER_WIDTH;
    maker.dimensions[1] = RENDERER_HEIGHT;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &maker);
    dkImageInitialize(image, &layout, mem, offset);
}

static uint32_t image_size(DkImageFormat format, uint32_t flags)
{
    DkImageLayoutMaker maker;
    dkImageLayoutMakerDefaults(&maker, g.device);
    maker.flags = flags;
    maker.format = format;
    maker.dimensions[0] = RENDERER_WIDTH;
    maker.dimensions[1] = RENDERER_HEIGHT;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &maker);
    return align_up((uint32_t)dkImageLayoutGetSize(&layout), dkImageLayoutGetAlignment(&layout));
}

int renderer_init(void)
{
    DkDeviceMaker device_maker;
    dkDeviceMakerDefaults(&device_maker);
    device_maker.cbDebug = debug_callback;
    device_maker.flags = DkDeviceFlags_DepthMinusOneToOne | DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_YAxisPointsDown;
    g.device = dkDeviceCreate(&device_maker);
    if (!g.device)
        return -1;
    g.screen_width = RENDERER_WIDTH;
    g.screen_height = RENDERER_HEIGHT;

    DkQueueMaker queue_maker;
    dkQueueMakerDefaults(&queue_maker, g.device);
    queue_maker.flags = DkQueueFlags_Graphics;
    g.queue = dkQueueCreate(&queue_maker);

    for (int i = 0; i < NUM_FRAMES; i++) {
        struct frame *f = &g.frames[i];
        f->cmd_mem = make_block(CMD_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        DkCmdBufMaker cmd_maker;
        dkCmdBufMakerDefaults(&cmd_maker, g.device);
        cmd_maker.userData = f;
        cmd_maker.cbAddMem = add_cmd_memory;
        f->cmd = dkCmdBufCreate(&cmd_maker);
        f->ring_mem = make_block(RING_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        f->ring_cpu = dkMemBlockGetCpuAddr(f->ring_mem);
        f->ring_gpu = dkMemBlockGetGpuAddr(f->ring_mem);
    }

    g.buffer_heap.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g.image_heap.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;

    uint32_t descriptors = MAX_IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor);
    uint32_t samplers = MAX_SAMPLER_DESCRIPTORS * sizeof(DkSamplerDescriptor);
    g.descriptor_mem = make_block(descriptors + samplers, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    g.image_descriptors = dkMemBlockGetCpuAddr(g.descriptor_mem);
    g.image_descriptors_gpu = dkMemBlockGetGpuAddr(g.descriptor_mem);
    g.sampler_descriptors = (DkSamplerDescriptor *)((uint8_t *)g.image_descriptors + descriptors);
    g.sampler_descriptors_gpu = g.image_descriptors_gpu + descriptors;

    const uint32_t swap_flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
    uint32_t swap_size = image_size(DkImageFormat_RGBA8_Unorm, swap_flags);
    g.swap_mem = make_block(swap_size * NUM_IMAGES, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    const DkImage *ptrs[NUM_IMAGES];
    for (unsigned i = 0; i < NUM_IMAGES; i++) {
        make_target_image(&g.swap_images[i], DkImageFormat_RGBA8_Unorm, swap_flags, g.swap_mem, i * swap_size);
        ptrs[i] = &g.swap_images[i];
    }
    DkSwapchainMaker sc_maker;
    dkSwapchainMakerDefaults(&sc_maker, g.device, nwindowGetDefault(), ptrs, NUM_IMAGES);
    g.swapchain = dkSwapchainCreate(&sc_maker);

    const uint32_t color_flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
    const uint32_t depth_flags = DkImageFlags_UsageRender | DkImageFlags_HwCompression;
    uint32_t color_size = image_size(DkImageFormat_RGBA8_Unorm, color_flags);
    uint32_t depth_size = image_size(DkImageFormat_Z24S8, depth_flags);
    g.back_mem = make_block(align_up(color_size, 0x10000) + depth_size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    make_target_image(&g.back_color, DkImageFormat_RGBA8_Unorm, color_flags, g.back_mem, 0);
    make_target_image(&g.back_depth, DkImageFormat_Z24S8, depth_flags, g.back_mem, align_up(color_size, 0x10000));
    return g.swapchain ? 0 : -1;
}

void renderer_present_clear(float r, float g_, float b)
{
    begin_frame();
    struct frame *f = cur();
    int slot = dkQueueAcquireImage(g.queue, g.swapchain);
    DkImageView view;
    dkImageViewDefaults(&view, &g.swap_images[slot]);
    const DkImageView *target = &view;
    dkCmdBufBindRenderTargets(f->cmd, &target, 1, NULL);
    dkCmdBufClearColorFloat(f->cmd, 0, DkColorMask_RGBA, r, g_, b, 1.0f);
    dkQueueSubmitCommands(g.queue, dkCmdBufFinishList(f->cmd));
    dkQueueSignalFence(g.queue, &f->fence, false);
    f->fence_pending = 1;
    dkQueuePresentImage(g.queue, g.swapchain, slot);
    g.frame_open = 0;
    g.target_is_set = 0;
    g.current = (g.current + 1) % NUM_FRAMES;
}

void renderer_shutdown(void)
{
    dkQueueWaitIdle(g.queue);
}

#define CAP_PAK g.pak
#define CAP_RECORD(d) (((const struct gprogram *)(d)->program)->record)
#include "draw_capture.h"

// ---- shaders -------------------------------------------------------------------

const char *gb_pak_path(void) { return PAK_PATH; }

void gb_pak_loaded(const uint8_t *pak, uint32_t code_offset, uint32_t code_size)
{
    g.pak = pak;
    g.code_mem = make_block(code_size + DK_SHADER_CODE_UNUSABLE_SIZE,
                            DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
    memcpy(dkMemBlockGetCpuAddr(g.code_mem), pak + code_offset, code_size);
}

void *gb_program_create(const uint8_t *pak, const struct pak_record *r)
{
    struct gprogram *p = malloc(sizeof *p);
    p->record = r;
    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, g.code_mem, r->vs_code);
    maker.control = pak + r->vs_control;
    dkShaderInitialize(&p->shaders[0], &maker);
    dkShaderMakerDefaults(&maker, g.code_mem, r->fs_code);
    maker.control = pak + r->fs_control;
    dkShaderInitialize(&p->shaders[1], &maker);
    if (!dkShaderIsValid(&p->shaders[0]) || !dkShaderIsValid(&p->shaders[1])) {
        fprintf(stderr, "deko3d: invalid shader in the pack\n");
        free(p);
        return NULL;
    }
    return p;
}

void gb_shader_missing(uint64_t key, const char *vs, const char *fs)
{
    fprintf(stderr, "gles: program %016llx is not in the pack; its draws are skipped\n", (unsigned long long)key);
    mkdir(MISS_DIR, 0777);
    char path[96];
    snprintf(path, sizeof path, MISS_DIR "/%016llx.txt", (unsigned long long)key);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n//=====FS=====\n%s", vs, fs);
        fclose(f);
    }
}

// ---- buffers -------------------------------------------------------------------

void gb_buffer_release(struct gb_buffer *b)
{
    struct allocation *a = b->priv;
    if (a) {
        gb_stats.buffer_bytes -= a->size;
        defer_free(a, -1);
        free(a);
        b->priv = NULL;
    }
    b->ring_epoch = 0;
}

static uint32_t ring_need(const struct gb_buffer *b)
{
    int dirty = b->dirty_lo < b->dirty_hi;
    if (b->priv && !dirty)
        return 0;
    return b->size + 16;
}

// Dynamic buffers live in the ring, so a draw can never see a later update.  Writing past
// everything written so far extends the ring copy in place, which is how Unity fills its dynamic
// VBOs; anything else takes a whole new copy.  A buffer left alone moves to the heap.
static DkGpuAddr buffer_address(struct gb_buffer *b)
{
    int dirty = b->dirty_lo < b->dirty_hi;
    struct allocation *a = b->priv;
    if (a && !dirty)
        return a->chunk->gpu + a->offset;
    if (a) {
        gb_stats.buffer_bytes -= a->size;
        defer_free(a, -1);
        free(a);
        b->priv = NULL;
    }
    if (b->ring_epoch == g.epoch) {
        if (!dirty)
            return b->ring_addr;
        if (b->dirty_lo >= b->ring_written) {
            memcpy(b->ring_cpu + b->dirty_lo, b->data + b->dirty_lo, b->dirty_hi - b->dirty_lo);
            b->ring_written = b->dirty_hi;
            b->dirty_lo = b->dirty_hi = 0;
            return b->ring_addr;
        }
    }
    if (g.frame_number >= b->last_change + HEAP_AFTER_FRAMES) {
        a = malloc(sizeof *a);
        if (heap_alloc(&g.buffer_heap, b->size, 16, a) == 0) {
            gb_stats.buffer_bytes += b->size;
            memcpy(a->chunk->cpu + a->offset, b->data, b->size);
            b->priv = a;
            b->dirty_lo = b->dirty_hi = 0;
            b->ring_epoch = 0;
            return a->chunk->gpu + a->offset;
        }
        free(a);
    }
    DkGpuAddr gpu;
    b->ring_cpu = ring_alloc(b->size, 16, &gpu);
    memcpy(b->ring_cpu, b->data, b->size);
    b->ring_addr = gpu;
    b->ring_epoch = g.epoch;
    b->ring_written = dirty ? b->dirty_hi : b->size;
    b->dirty_lo = b->dirty_hi = 0;
    return gpu;
}

// ---- textures ------------------------------------------------------------------

static struct gtex *gtex_of(struct gb_texture *t)
{
    if (!t->priv)
        t->priv = calloc(1, sizeof(struct gtex));
    return t->priv;
}

static void set_swizzle(struct gtex *x, DkImageSwizzle r, DkImageSwizzle g_, DkImageSwizzle b, DkImageSwizzle a)
{
    x->swizzle[0] = r;
    x->swizzle[1] = g_;
    x->swizzle[2] = b;
    x->swizzle[3] = a;
}

static void choose_format(struct gtex *x, enum gb_format f)
{
    const DkImageSwizzle R = DkImageSwizzle_Red, G = DkImageSwizzle_Green, B = DkImageSwizzle_Blue,
                         A = DkImageSwizzle_Alpha, ZERO = DkImageSwizzle_Zero, ONE = DkImageSwizzle_One;
    set_swizzle(x, R, G, B, A);
    switch (f) {
    case GB_BGRA8: x->dk_format = DkImageFormat_BGRA8_Unorm; break;
    case GB_RGBA4: x->dk_format = DkImageFormat_RGBA4_Unorm; set_swizzle(x, A, B, G, R); break;
    case GB_RGB5A1: x->dk_format = DkImageFormat_RGB5A1_Unorm; break;
    case GB_RGB565: x->dk_format = DkImageFormat_RGB565_Unorm; break;
    case GB_A8: x->dk_format = DkImageFormat_R8_Unorm; set_swizzle(x, ZERO, ZERO, ZERO, R); break;
    case GB_L8: x->dk_format = DkImageFormat_R8_Unorm; set_swizzle(x, R, R, R, ONE); break;
    case GB_LA8: x->dk_format = DkImageFormat_RG8_Unorm; set_swizzle(x, R, R, R, G); break;
    case GB_ETC1: x->dk_format = DkImageFormat_RGB_ETC2; break;
    case GB_BC1: x->dk_format = DkImageFormat_RGB_BC1; break;
    case GB_BC1A: x->dk_format = DkImageFormat_RGBA_BC1; break;
    case GB_BC2: x->dk_format = DkImageFormat_RGBA_BC2; break;
    case GB_BC3: x->dk_format = DkImageFormat_RGBA_BC3; break;
    case GB_D16: x->dk_format = DkImageFormat_Z16; break;
    case GB_D24S8: x->dk_format = DkImageFormat_Z24S8; break;
    case GB_S8: x->dk_format = DkImageFormat_S8; break;
    default: x->dk_format = DkImageFormat_RGBA8_Unorm; break;
    }
}

static int new_descriptor(void)
{
    if (g.free_descriptor_count)
        return g.free_descriptors[--g.free_descriptor_count];
    if (g.next_descriptor == MAX_IMAGE_DESCRIPTORS) {
        fprintf(stderr, "deko3d: out of image descriptors\n");
        return 0;
    }
    return g.next_descriptor++;
}

static void copy_to_image(struct gtex *x, int face, int level, int px, int py, int w, int h, const void *data,
                          uint32_t size)
{
    // The GPU writes the rectangle into the image as given, so one outside this level or face would
    // land in whatever memory follows the image.
    int lw = x->layout_w >> level, lh = x->layout_h >> level, faces = x->faces;
    if (lw < 1)
        lw = 1;
    if (lh < 1)
        lh = 1;
    if (face < 0 || face >= faces || px < 0 || py < 0 || px >= lw || py >= lh || w <= 0 || h <= 0 ||
        px + w > lw || py + h > lh || size > RING_SIZE / 2) {
        static int shown;
        if (shown++ < 10)
            fprintf(stderr, "deko3d: upload %d,%d %dx%d face %d level %d outside a %dx%d image (%d faces), %u bytes:"
                            " %s\n",
                    px, py, w, h, face, level, lw, lh, faces, (unsigned)size,
                    face < faces && px < lw && py < lh && size <= RING_SIZE / 2 ? "clipped" : "skipped");
        if (face < 0 || face >= faces || px < 0 || py < 0 || px >= lw || py >= lh || w <= 0 || h <= 0 ||
            size > RING_SIZE / 2)
            return;
        if (px + w > lw)
            w = lw - px;
        if (py + h > lh)
            h = lh - py;
    }
    DkGpuAddr gpu;
    uint8_t *cpu = ring_alloc(size, 256, &gpu);
    memcpy(cpu, data, size);
    gb_stats.upload_bytes += size;
    DkImageView view;
    dkImageViewDefaults(&view, &x->image);
    view.mipLevelOffset = (uint8_t)level;
    view.mipLevelCount = 1;
    DkCopyBuf src = {gpu, 0, 0};
    DkImageRect rect = {(uint32_t)px, (uint32_t)py, (uint32_t)face, (uint32_t)w, (uint32_t)h, 1};
    event("upload", w, h, level, face, x->descriptor, (int)size);
    dkCmdBufCopyBufferToImage(cur()->cmd, &src, &view, &rect, 0);
}

uint32_t gb_realizes, gb_unrealizes, gb_pending_bytes; // for the watchdog's minute report

static void clear_pending(struct gtex *x)
{
    for (int f = 0; f < 6; f++)
        for (int l = 0; l < GB_MAX_LEVELS; l++) {
            if (x->pending[f][l].data)
                gb_pending_bytes -= x->pending[f][l].size;
            free(x->pending[f][l].data);
            x->pending[f][l].data = NULL;
        }
}

static int full_mip_count(int w, int h)
{
    int n = 1;
    while ((w > 1 || h > 1) && n < GB_MAX_LEVELS) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        n++;
    }
    return n;
}

static void generate_mips(struct gtex *x, int w, int h)
{
    if (!x->blit_ok) // compressed: made without the 2D engine flag, which a blit needs
        return;
    event("mipmaps", w, h, x->levels, x->descriptor, 0, 0);
    DkCmdBuf cmd = cur()->cmd;
    for (int level = 1; level < x->levels; level++) {
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        DkImageView src, dst;
        dkImageViewDefaults(&src, &x->image);
        dkImageViewDefaults(&dst, &x->image);
        src.mipLevelOffset = (uint8_t)(level - 1);
        src.mipLevelCount = 1;
        dst.mipLevelOffset = (uint8_t)level;
        dst.mipLevelCount = 1;
        DkImageRect a = {0, 0, 0, (uint32_t)w, (uint32_t)h, 1}, b = {0, 0, 0, (uint32_t)nw, (uint32_t)nh, 1};
        dkCmdBufBarrier(cmd, DkBarrier_Primitives, DkInvalidateFlags_Image);
        dkCmdBufBlitImage(cmd, &src, &a, &dst, &b, DkBlitFlag_FilterLinear, 0);
        w = nw;
        h = nh;
    }
    dkCmdBufBarrier(cmd, DkBarrier_Primitives, DkInvalidateFlags_Image);
}

// Images are made on first use: only then are the mip count and whether it is rendered to known.
static void realize(struct gb_texture *t, int renderable)
{
    struct gtex *x = gtex_of(t);
    if (x->realized)
        return;
    if (!t->width || !t->height || t->format == GB_NONE)
        return;
    choose_format(x, t->format);
    int depth_format = t->format >= GB_D16;
    int compressed = t->format >= GB_ETC1 && t->format <= GB_BC3;

    int levels = t->levels ? t->levels : 1;
    if (x->want_mipmaps)
        levels = full_mip_count(t->width, t->height);
    x->levels = levels;

    DkImageLayoutMaker maker;
    dkImageLayoutMakerDefaults(&maker, g.device);
    maker.type = t->cube ? DkImageType_Cubemap : DkImageType_2D;
    maker.format = x->dk_format;
    maker.dimensions[0] = (uint32_t)t->width;
    maker.dimensions[1] = (uint32_t)t->height;
    maker.dimensions[2] = t->cube ? 6 : 1;
    maker.mipLevels = (uint32_t)levels;
    if (depth_format) // as the screen's own depth buffer, which works
        maker.flags = DkImageFlags_UsageRender | DkImageFlags_HwCompression;
    else if (renderable)
        maker.flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
    else if (!compressed)
        maker.flags = DkImageFlags_Usage2DEngine;
    dkImageLayoutInitialize(&x->layout, &maker);
    x->blit_ok = (maker.flags & DkImageFlags_Usage2DEngine) != 0;
    x->renderable = renderable || depth_format;
    x->layout_w = t->width;
    x->layout_h = t->height;
    x->faces = t->cube ? 6 : 1;

    uint32_t size = (uint32_t)dkImageLayoutGetSize(&x->layout);
    if (heap_alloc(&g.image_heap, size, dkImageLayoutGetAlignment(&x->layout), &x->a) != 0) {
        fprintf(stderr, "deko3d: out of memory for a %dx%d image\n", t->width, t->height);
        return;
    }
    dkImageInitialize(&x->image, &x->layout, x->a.chunk->mem, x->a.offset);
    x->realized = 1;
    gb_realizes++;
    event("image made", t->width, t->height, levels, (int)t->format, x->renderable, (int)size);
    gb_stats.image_bytes += size;
    gb_stats.textures++;

    if (!depth_format) {
        x->descriptor = new_descriptor();
        DkImageView view;
        dkImageViewDefaults(&view, &x->image);
        memcpy(view.swizzle, x->swizzle, sizeof view.swizzle);
        dkImageDescriptorInitialize(&g.image_descriptors[x->descriptor], &view, false, false);
        dkCmdBufBarrier(cur()->cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
    }

    int uploaded = 0;
    for (int f = 0; f < 6; f++)
        for (int l = 0; l < GB_MAX_LEVELS && l < levels; l++)
            if (x->pending[f][l].data) {
                copy_to_image(x, f, l, 0, 0, x->pending[f][l].w, x->pending[f][l].h, x->pending[f][l].data,
                              x->pending[f][l].size);
                uploaded = 1;
            }
    clear_pending(x);
    if (uploaded)
        dkCmdBufBarrier(cur()->cmd, DkBarrier_Primitives, DkInvalidateFlags_Image);
    if (x->want_mipmaps)
        generate_mips(x, t->width, t->height);
}

static void unrealize(struct gb_texture *t)
{
    struct gtex *x = t->priv;
    if (!x || !x->realized)
        return;
    defer_free(&x->a, t->format >= GB_D16 ? -1 : x->descriptor);
    gb_stats.image_bytes -= x->a.size;
    gb_stats.textures--;
    gb_unrealizes++;
    event("image dropped", t->width, t->height, x->levels, (int)t->format, x->descriptor, 0);
    x->realized = 0;
}

static void queue_realize(struct gb_texture *t, struct gtex *x);

void gb_tex_data(struct gb_texture *t, int face, int level, int w, int h, enum gb_format f, const void *data,
                 uint32_t size)
{
    begin_frame();
    struct gtex *x = gtex_of(t);
    if (level >= GB_MAX_LEVELS)
        return;
    // Render targets are always RGBA8: Unity only probes the small formats.
    if (!data && f < GB_D16)
        f = GB_RGBA8;
    if (level == 0 && (t->width != w || t->height != h || t->format != f)) {
        unrealize(t);
        clear_pending(x);
        t->levels = 0;
        x->want_mipmaps = 0;
    }
    if (level == 0) {
        t->width = w;
        t->height = h;
        t->format = f;
    }
    if (level + 1 > t->levels)
        t->levels = level + 1;

    if (x->realized) {
        if (level < x->levels && data)
            copy_to_image(x, face, level, 0, 0, w, h, data, size);
        else if (level >= x->levels) {
            // A level arrived after first use: rebuild the image.  Its old contents are gone,
            // which only happens to textures still being filled.
            unrealize(t);
        }
        dkCmdBufBarrier(cur()->cmd, DkBarrier_Primitives, DkInvalidateFlags_Image);
        return;
    }
    if (data) {
        if (x->pending[face][level].data)
            gb_pending_bytes -= x->pending[face][level].size;
        free(x->pending[face][level].data);
        x->pending[face][level].data = NULL;
        void *copy = malloc(size);
        if (!copy) {
            fprintf(stderr, "deko3d: out of memory holding a texture level\n");
            return;
        }
        memcpy(copy, data, size);
        x->pending[face][level].data = copy;
        x->pending[face][level].size = size;
        gb_pending_bytes += size;
        x->pending[face][level].w = w;
        x->pending[face][level].h = h;
        queue_realize(t, x);
    }
}

void gb_tex_sub(struct gb_texture *t, int face, int level, int px, int py, int w, int h, const void *data,
                uint32_t size)
{
    begin_frame();
    realize(t, 0);
    struct gtex *x = gtex_of(t);
    if (!x->realized || level >= x->levels)
        return;
    copy_to_image(x, face, level, px, py, w, h, data, size);
    dkCmdBufBarrier(cur()->cmd, DkBarrier_Primitives, DkInvalidateFlags_Image);
}

void gb_tex_mipmap(struct gb_texture *t)
{
    begin_frame();
    struct gtex *x = gtex_of(t);
    if (!x->realized) {
        x->want_mipmaps = 1;
        return;
    }
    if (x->levels > 1)
        generate_mips(x, t->width, t->height);
}

// Textures holding level data in RAM, made into images at the end of the frame they were filled in.
// Waiting for their first draw instead kept a second copy of every texture of a level being loaded
// (and of every texture never drawn) in the heap: loading a second survival map ran the 1 GB heap
// out and textures went missing.
static struct gb_texture **g_unrealized;
static int g_unrealized_count, g_unrealized_cap;

static void queue_realize(struct gb_texture *t, struct gtex *x)
{
    if (x->queued)
        return;
    if (g_unrealized_count == g_unrealized_cap) {
        int cap = g_unrealized_cap ? g_unrealized_cap * 2 : 256;
        struct gb_texture **grown = realloc(g_unrealized, (size_t)cap * sizeof *grown);
        if (!grown)
            return; // stays pending until drawn, as before
        g_unrealized = grown;
        g_unrealized_cap = cap;
    }
    g_unrealized[g_unrealized_count++] = t;
    x->queued = 1;
}

static void unqueue_realize(struct gb_texture *t, struct gtex *x)
{
    if (!x->queued)
        return;
    for (int i = 0; i < g_unrealized_count; i++)
        if (g_unrealized[i] == t) {
            g_unrealized[i] = g_unrealized[--g_unrealized_count];
            break;
        }
    x->queued = 0;
}

static void realize_queued(void)
{
    for (int i = 0; i < g_unrealized_count; i++) {
        struct gb_texture *t = g_unrealized[i];
        struct gtex *x = t->priv;
        if (!x)
            continue;
        x->queued = 0;
        realize(t, 0);
    }
    g_unrealized_count = 0;
}

void gb_tex_release(struct gb_texture *t)
{
    struct gtex *x = t->priv;
    if (!x)
        return;
    unqueue_realize(t, x);
    if (t == g.color_target || t == g.depth_target || t == g.stencil_target)
        g.target_is_set = 0;
    unrealize(t);
    clear_pending(x);
    free(x);
    t->priv = NULL;
}

void gb_copy_framebuffer(struct gb_texture *t, int face, int level, int xoff, int yoff, int px, int py, int w, int h)
{
    begin_frame();
    realize(t, 1);
    struct gtex *x = gtex_of(t);
    if (!x->realized || !x->blit_ok || level < 0 || level >= x->levels || face < 0 || face >= x->faces)
        return;
    // A blit outside either image writes or reads GPU memory that is not its own.
    struct gtex *from = g.color_target ? g.color_target->priv : NULL;
    int sw = RENDERER_WIDTH, sh = RENDERER_HEIGHT;
    if (g.color_target) {
        if (!from || !from->realized || !from->blit_ok)
            return;
        sw = from->layout_w;
        sh = from->layout_h;
    }
    int dw = x->layout_w >> level, dh = x->layout_h >> level;
    dw = dw > 0 ? dw : 1;
    dh = dh > 0 ? dh : 1;
    if (px < 0 || py < 0 || xoff < 0 || yoff < 0)
        return;
    if (px + w > sw)
        w = sw - px;
    if (py + h > sh)
        h = sh - py;
    if (xoff + w > dw)
        w = dw - xoff;
    if (yoff + h > dh)
        h = dh - yoff;
    if (w <= 0 || h <= 0)
        return;
    DkCmdBuf cmd = cur()->cmd;
    DkImageView src, dst;
    if (from)
        dkImageViewDefaults(&src, &from->image);
    else
        dkImageViewDefaults(&src, &g.back_color);
    dkImageViewDefaults(&dst, &x->image);
    dst.mipLevelOffset = (uint8_t)level;
    dst.mipLevelCount = 1;
    DkImageRect a = {(uint32_t)px, (uint32_t)py, 0, (uint32_t)w, (uint32_t)h, 1};
    DkImageRect b = {(uint32_t)xoff, (uint32_t)yoff, (uint32_t)face, (uint32_t)w, (uint32_t)h, 1};
    event("copy_framebuffer", px, py, w, h, x->descriptor, level);
    dkCmdBufBarrier(cmd, DkBarrier_Fragments, 0);
    dkCmdBufBlitImage(cmd, &src, &a, &dst, &b, 0, 0);
    dkCmdBufBarrier(cmd, DkBarrier_Primitives, DkInvalidateFlags_Image);
}

static int sampler_id(const struct gb_texture *t, int levels)
{
    uint32_t aniso = (uint32_t)t->anisotropy;
    uint32_t min = t->min_filter;
    if (levels <= 1 && min >= 0x2700)
        min = (min == 0x2700 || min == 0x2702) ? 0x2600 : 0x2601;
    for (int i = 0; i < g.sampler_count; i++)
        if (g.samplers[i].min == min && g.samplers[i].mag == t->mag_filter && g.samplers[i].wrap_s == t->wrap_s &&
            g.samplers[i].wrap_t == t->wrap_t && g.samplers[i].aniso == aniso)
            return i;
    if (g.sampler_count == MAX_SAMPLER_DESCRIPTORS)
        return 0;
    int id = g.sampler_count++;
    g.samplers[id].min = min;
    g.samplers[id].mag = t->mag_filter;
    g.samplers[id].wrap_s = t->wrap_s;
    g.samplers[id].wrap_t = t->wrap_t;
    g.samplers[id].aniso = aniso;

    DkSampler s;
    dkSamplerDefaults(&s);
    s.minFilter = (min == 0x2600 || min == 0x2700 || min == 0x2702) ? DkFilter_Nearest : DkFilter_Linear;
    s.magFilter = t->mag_filter == 0x2600 ? DkFilter_Nearest : DkFilter_Linear;
    s.mipFilter = min < 0x2700 ? DkMipFilter_None : (min == 0x2700 || min == 0x2701) ? DkMipFilter_Nearest : DkMipFilter_Linear;
    DkWrapMode wrap[2];
    uint32_t gl_wrap[2] = {t->wrap_s, t->wrap_t};
    for (int i = 0; i < 2; i++)
        wrap[i] = gl_wrap[i] == 0x812F ? DkWrapMode_ClampToEdge
                  : gl_wrap[i] == 0x8370 ? DkWrapMode_MirroredRepeat
                                         : DkWrapMode_Repeat;
    s.wrapMode[0] = wrap[0];
    s.wrapMode[1] = wrap[1];
    s.wrapMode[2] = wrap[1];
    s.maxAnisotropy = aniso > 1 ? (float)aniso : 1.0f;
    dkSamplerDescriptorInitialize(&g.sampler_descriptors[id], &s);
    dkCmdBufBarrier(cur()->cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
    return id;
}

// Texture unit with nothing bound: GL samples black.
static int black_texture(void)
{
    static struct gb_texture black;
    if (!black.format) {
        static const uint8_t pixel[4] = {0, 0, 0, 255};
        black.min_filter = black.mag_filter = 0x2600;
        black.wrap_s = black.wrap_t = 0x812F;
        black.anisotropy = 1.0f;
        gb_tex_data(&black, 0, 0, 1, 1, GB_RGBA8, pixel, 4);
        realize(&black, 0);
    }
    return gtex_of(&black)->descriptor;
}

// ---- render targets --------------------------------------------------------------

void gb_bind_target(struct gb_texture *color, struct gb_texture *depth, struct gb_texture *stencil)
{
    if (g.color_target != color || g.depth_target != depth || g.stencil_target != stencil)
        g.target_is_set = 0;
    g.color_target = color;
    g.depth_target = depth;
    g.stencil_target = stencil;
}

static void apply_target(void)
{
    if (g.target_is_set)
        return;
    DkCmdBuf cmd = cur()->cmd;
    dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
    DkImageView cv, dv;
    const DkImageView *color = &cv, *depth = NULL;
    if (!g.color_target && !g.depth_target) {
        dkImageViewDefaults(&cv, &g.back_color);
        dkImageViewDefaults(&dv, &g.back_depth);
        depth = &dv;
        g.target_width = RENDERER_WIDTH;
        g.target_height = RENDERER_HEIGHT;
        g.target_has_depth = g.target_has_stencil = 1;
    } else {
        g.target_width = RENDERER_WIDTH;
        g.target_height = RENDERER_HEIGHT;
        g.target_has_depth = g.target_has_stencil = 0;
        int count = 1;
        if (g.color_target) {
            realize(g.color_target, 1);
            struct gtex *x = gtex_of(g.color_target);
            if (!x->realized || !x->renderable) {
                // Binding an image that was never made (or not for rendering) points the GPU at
                // whatever the uninitialised view holds.
                static int shown;
                if (shown++ < 10)
                    fprintf(stderr, "deko3d: target %dx%d %s: drawing to no colour target\n",
                            g.color_target->width, g.color_target->height,
                            x->realized ? "was not made renderable" : "has no image");
                count = 0;
            } else {
                dkImageViewDefaults(&cv, &x->image);
                g.target_width = g.color_target->width;
                g.target_height = g.color_target->height;
            }
        } else {
            count = 0;
        }
        struct gb_texture *ds = g.depth_target ? g.depth_target : g.stencil_target;
        if (ds && ds->format < GB_D16) {
            // A colour image as the depth buffer is an invalid zeta format: the GPU queue errors.
            static int shown;
            if (shown++ < 10)
                fprintf(stderr, "deko3d: %dx%d format %d attached as depth: left off\n", ds->width, ds->height,
                        (int)ds->format);
            ds = NULL;
        }
        if (ds) {
            realize(ds, 1);
            struct gtex *x = gtex_of(ds);
            // A depth buffer smaller than the colour target is written past its end.
            int too_small = count && (x->layout_w < g.target_width || x->layout_h < g.target_height);
            if (x->realized && too_small) {
                static int shown;
                if (shown++ < 10)
                    fprintf(stderr, "deko3d: depth %dx%d smaller than target %dx%d: drawing without depth\n",
                            x->layout_w, x->layout_h, g.target_width, g.target_height);
            } else if (x->realized) {
                if (!count) {
                    g.target_width = x->layout_w;
                    g.target_height = x->layout_h;
                }
                dkImageViewDefaults(&dv, &x->image);
                depth = &dv;
                g.target_has_depth = ds->format != GB_S8;
                g.target_has_stencil = ds->format == GB_D24S8 || ds->format == GB_S8;
            }
        }
        event("target", count, g.target_width, g.target_height, depth != NULL, ds ? ds->width : 0,
              ds ? ds->height : 0);
        dkCmdBufBindRenderTargets(cmd, &color, (uint32_t)count, depth);
        g.target_is_set = 1;
        g.all_dirty = 1;
        return;
    }
    dkCmdBufBindRenderTargets(cmd, &color, 1, depth);
    g.target_is_set = 1;
    g.all_dirty = 1;
}

// ---- state -----------------------------------------------------------------------

static DkCompareOp compare_op(uint32_t gl)
{
    return (DkCompareOp)(gl - 0x0200 + 1); // NEVER..ALWAYS are in the same order
}

static DkBlendFactor blend_factor(uint32_t gl)
{
    switch (gl) {
    case 0: return DkBlendFactor_Zero;
    case 1: return DkBlendFactor_One;
    case 0x0300: return DkBlendFactor_SrcColor;
    case 0x0301: return DkBlendFactor_InvSrcColor;
    case 0x0302: return DkBlendFactor_SrcAlpha;
    case 0x0303: return DkBlendFactor_InvSrcAlpha;
    case 0x0304: return DkBlendFactor_DstAlpha;
    case 0x0305: return DkBlendFactor_InvDstAlpha;
    case 0x0306: return DkBlendFactor_DstColor;
    case 0x0307: return DkBlendFactor_InvDstColor;
    case 0x0308: return DkBlendFactor_SrcAlphaSaturate;
    default: return DkBlendFactor_One;
    }
}

static DkBlendOp blend_op(uint32_t gl)
{
    switch (gl) {
    case 0x800A: return DkBlendOp_Sub;
    case 0x800B: return DkBlendOp_RevSub;
    case 0x8007: return DkBlendOp_Min;
    case 0x8008: return DkBlendOp_Max;
    default: return DkBlendOp_Add;
    }
}

static DkStencilOp stencil_op(uint32_t gl)
{
    switch (gl) {
    case 0: return DkStencilOp_Zero;
    case 0x1E01: return DkStencilOp_Replace;
    case 0x1E02: return DkStencilOp_Incr;
    case 0x1E03: return DkStencilOp_Decr;
    case 0x150A: return DkStencilOp_Invert;
    case 0x8507: return DkStencilOp_IncrWrap;
    case 0x8508: return DkStencilOp_DecrWrap;
    default: return DkStencilOp_Keep;
    }
}

static void apply_scissor(const struct gb_state *s, DkCmdBuf cmd)
{
    DkScissor sc = {0, 0, (uint32_t)g.target_width, (uint32_t)g.target_height};
    if (s->scissor_test) {
        int x = s->scissor[0] < 0 ? 0 : s->scissor[0], y = s->scissor[1] < 0 ? 0 : s->scissor[1];
        sc.x = (uint32_t)x;
        sc.y = (uint32_t)y;
        sc.width = (uint32_t)(s->scissor[2] < 0 ? 0 : s->scissor[2]);
        sc.height = (uint32_t)(s->scissor[3] < 0 ? 0 : s->scissor[3]);
    }
    dkCmdBufSetScissors(cmd, 0, &sc, 1);
}

static void apply_state(const struct gb_state *s)
{
    DkCmdBuf cmd = cur()->cmd;
    struct gb_state *o = &g.bound;
    int all = g.all_dirty;
    g.all_dirty = 0;

    if (all || memcmp(s->viewport, o->viewport, sizeof s->viewport) != 0) {
        DkViewport v = {(float)s->viewport[0], (float)s->viewport[1], (float)s->viewport[2], (float)s->viewport[3], 0.0f, 1.0f};
        dkCmdBufSetViewports(cmd, 0, &v, 1);
    }
    if (all || s->scissor_test != o->scissor_test || memcmp(s->scissor, o->scissor, sizeof s->scissor) != 0)
        apply_scissor(s, cmd);
    if (all || s->cull != o->cull || s->cull_face != o->cull_face || s->front_face != o->front_face) {
        DkRasterizerState r;
        dkRasterizerStateDefaults(&r);
        r.cullMode = !s->cull ? DkFace_None
                     : s->cull_face == 0x0404 ? DkFace_Front
                     : s->cull_face == 0x0408 ? DkFace_FrontAndBack
                                              : DkFace_Back;
        // The device has YAxisPointsDown, which mirrors the image vertically and with it every
        // triangle's winding, so GL's front face is the other one here.  Unflipped, single-sided
        // ground and the sky dome (seen from inside) were culled and closed meshes showed their far
        // walls.  Test switches: sd:/switch/cod_st/no_flip restores the old winding, no_cull draws
        // both sides.
        static int flip = -1, nocull = -1;
        if (flip < 0) {
            flip = access("sdmc:/switch/cod_st/no_flip", F_OK) != 0;
            nocull = access("sdmc:/switch/cod_st/no_cull", F_OK) == 0;
            fprintf(stderr, "deko3d: winding flipped=%d, culling off=%d\n", flip, nocull);
        }
        r.frontFace = (s->front_face == 0x0900) != flip ? DkFrontFace_CW : DkFrontFace_CCW;
        if (nocull)
            r.cullMode = DkFace_None;
        dkCmdBufBindRasterizerState(cmd, &r);
    }
    if (all || s->blend != o->blend) {
        DkColorState c;
        dkColorStateDefaults(&c);
        dkColorStateSetBlendEnable(&c, 0, s->blend);
        dkCmdBufBindColorState(cmd, &c);
    }
    if (all || s->color_mask != o->color_mask) {
        DkColorWriteState w;
        dkColorWriteStateDefaults(&w);
        dkColorWriteStateSetMask(&w, 0, s->color_mask);
        dkCmdBufBindColorWriteState(cmd, &w);
    }
    if (all || s->blend_op_rgb != o->blend_op_rgb || s->blend_op_alpha != o->blend_op_alpha ||
        s->blend_src_rgb != o->blend_src_rgb || s->blend_dst_rgb != o->blend_dst_rgb ||
        s->blend_src_alpha != o->blend_src_alpha || s->blend_dst_alpha != o->blend_dst_alpha) {
        DkBlendState b;
        dkBlendStateDefaults(&b);
        dkBlendStateSetOps(&b, blend_op(s->blend_op_rgb), blend_op(s->blend_op_alpha));
        dkBlendStateSetFactors(&b, blend_factor(s->blend_src_rgb), blend_factor(s->blend_dst_rgb),
                               blend_factor(s->blend_src_alpha), blend_factor(s->blend_dst_alpha));
        dkCmdBufBindBlendState(cmd, 0, &b);
    }
    if (all || s->depth_test != o->depth_test || s->depth_write != o->depth_write || s->depth_func != o->depth_func ||
        s->stencil_test != o->stencil_test || memcmp(s->stencil_func, o->stencil_func, sizeof s->stencil_func) != 0 ||
        memcmp(s->stencil_fail, o->stencil_fail, sizeof s->stencil_fail) != 0 ||
        memcmp(s->stencil_zfail, o->stencil_zfail, sizeof s->stencil_zfail) != 0 ||
        memcmp(s->stencil_zpass, o->stencil_zpass, sizeof s->stencil_zpass) != 0) {
        DkDepthStencilState d;
        dkDepthStencilStateDefaults(&d);
        // GL turns writes off with the test, and a target with no depth must not test.
        d.depthTestEnable = s->depth_test && g.target_has_depth;
        d.depthWriteEnable = s->depth_test && s->depth_write && g.target_has_depth;
        d.depthCompareOp = compare_op(s->depth_func);
        d.stencilTestEnable = s->stencil_test && g.target_has_stencil;
        d.stencilFrontCompareOp = compare_op(s->stencil_func[0]);
        d.stencilBackCompareOp = compare_op(s->stencil_func[1]);
        d.stencilFrontFailOp = stencil_op(s->stencil_fail[0]);
        d.stencilFrontDepthFailOp = stencil_op(s->stencil_zfail[0]);
        d.stencilFrontPassOp = stencil_op(s->stencil_zpass[0]);
        d.stencilBackFailOp = stencil_op(s->stencil_fail[1]);
        d.stencilBackDepthFailOp = stencil_op(s->stencil_zfail[1]);
        d.stencilBackPassOp = stencil_op(s->stencil_zpass[1]);
        dkCmdBufBindDepthStencilState(cmd, &d);
    }
    if (all || memcmp(s->stencil_ref, o->stencil_ref, sizeof s->stencil_ref) != 0 ||
        memcmp(s->stencil_read_mask, o->stencil_read_mask, sizeof s->stencil_read_mask) != 0 ||
        memcmp(s->stencil_write_mask, o->stencil_write_mask, sizeof s->stencil_write_mask) != 0) {
        dkCmdBufSetStencil(cmd, DkFace_Front, (uint8_t)s->stencil_write_mask[0], (uint8_t)s->stencil_ref[0],
                           (uint8_t)s->stencil_read_mask[0]);
        dkCmdBufSetStencil(cmd, DkFace_Back, (uint8_t)s->stencil_write_mask[1], (uint8_t)s->stencil_ref[1],
                           (uint8_t)s->stencil_read_mask[1]);
    }
    *o = *s;
}

void gb_clear(const struct gb_state *s, int color, int depth, int stencil, const float rgba[4], float z, int sv)
{
    if (capture_on())
        fprintf(g_cap, "clear color=%d (%.3f %.3f %.3f %.3f) depth=%d z=%.3f\n", color, rgba[0], rgba[1], rgba[2],
                rgba[3], depth, z);
    begin_frame();
    apply_target();
    event("clear", color, depth, stencil, g.target_width, g.target_height, 0);
    DkCmdBuf cmd = cur()->cmd;
    apply_scissor(s, cmd);
    g.bound.scissor_test = 255; // reapplied at the next draw
    if (color && s->color_mask)
        dkCmdBufClearColor(cmd, 0, s->color_mask, rgba);
    if ((depth && s->depth_write && g.target_has_depth) || (stencil && g.target_has_stencil))
        dkCmdBufClearDepthStencil(cmd, depth && s->depth_write && g.target_has_depth, z,
                                  stencil && g.target_has_stencil ? (uint8_t)s->stencil_write_mask[0] : 0, (uint8_t)sv);
}

// ---- drawing ---------------------------------------------------------------------

static DkVtxAttribSize attrib_size(uint32_t type, int n)
{
    static const DkVtxAttribSize s32[4] = {DkVtxAttribSize_1x32, DkVtxAttribSize_2x32, DkVtxAttribSize_3x32, DkVtxAttribSize_4x32};
    static const DkVtxAttribSize s16[4] = {DkVtxAttribSize_1x16, DkVtxAttribSize_2x16, DkVtxAttribSize_3x16, DkVtxAttribSize_4x16};
    static const DkVtxAttribSize s8[4] = {DkVtxAttribSize_1x8, DkVtxAttribSize_2x8, DkVtxAttribSize_3x8, DkVtxAttribSize_4x8};
    if (n < 1 || n > 4)
        n = 4;
    switch (type) {
    case 0x1400:
    case 0x1401: return s8[n - 1];
    case 0x1402:
    case 0x1403:
    case 0x140B: return s16[n - 1];
    default: return s32[n - 1];
    }
}

static DkVtxAttribType attrib_type(uint32_t gl, int normalized)
{
    switch (gl) {
    case 0x1400:
    case 0x1402: return normalized ? DkVtxAttribType_Snorm : DkVtxAttribType_Sscaled;
    case 0x1401:
    case 0x1403: return normalized ? DkVtxAttribType_Unorm : DkVtxAttribType_Uscaled;
    case 0x140C:
    case 0x1404: return DkVtxAttribType_Sscaled;
    default: return DkVtxAttribType_Float;
    }
}

static int attrib_bytes(const struct gb_attrib *a)
{
    int component = (a->type == 0x1400 || a->type == 0x1401) ? 1 : (a->type == 0x1402 || a->type == 0x1403 || a->type == 0x140B) ? 2 : 4;
    return component * a->components;
}

static uint32_t highest_index(const struct gb_draw *d)
{
    const uint8_t *base = d->indices;
    if (d->index_buffer)
        base = d->index_buffer->data + (uintptr_t)d->indices;
    uint32_t top = 0;
    if (d->index_type == 0x1403) {
        for (int i = 0; i < d->count; i++) {
            uint16_t v;
            memcpy(&v, base + i * 2, 2);
            if (v > top)
                top = v;
        }
    } else if (d->index_type == 0x1401) {
        for (int i = 0; i < d->count; i++)
            if (base[i] > top)
                top = base[i];
    } else if (d->index_type == 0x1405) {
        for (int i = 0; i < d->count; i++) {
            uint32_t v;
            memcpy(&v, base + i * 4, 4);
            if (v > top)
                top = v;
        }
    }
    return top;
}

static DkGpuAddr ring_copy(const void *data, uint32_t size)
{
    DkGpuAddr gpu;
    memcpy(ring_alloc(size, 16, &gpu), data, size);
    return gpu;
}

// The last draws, written to sd:/switch/cod_st/gpu_error.txt when the GPU queue reports an error
// (the faulting draw is among the last frame or two).
#define CRUMBS 1024
static struct crumb {
    uint64_t frame;
    uint32_t record, mode, count, index_type, index_size, index_off, textures, attribs;
    uint32_t vbuf_size[4], vbuf_off[4], stride[4];
    int32_t tex_desc[3], tex_w[3], tex_h[3], tex_fmt[3], tex_cube[3];
} g_crumbs[CRUMBS];
static unsigned g_crumb_next;

// Other GPU work between draws: targets, uploads, blits, clears, images made and dropped.
#define EVENTS 512
static struct event {
    uint64_t frame;
    unsigned draw; // g_crumb_next when it happened
    const char *what;
    int32_t v[6];
} g_events[EVENTS];
static unsigned g_event_next;

static void event(const char *what, int a, int b, int c, int d, int e, int f)
{
    struct event *ev = &g_events[g_event_next++ % EVENTS];
    ev->frame = g.frame_number;
    ev->draw = g_crumb_next;
    ev->what = what;
    ev->v[0] = a, ev->v[1] = b, ev->v[2] = c, ev->v[3] = d, ev->v[4] = e, ev->v[5] = f;
}

static void crumb(const struct gb_draw *d, uint32_t count)
{
    struct crumb *c = &g_crumbs[g_crumb_next++ % CRUMBS];
    const struct gprogram *p = d->program;
    c->frame = g.frame_number;
    c->record = p && p->record ? (uint32_t)((const uint8_t *)p->record - g.pak) : 0;
    c->mode = d->mode;
    c->count = count;
    c->index_type = d->index_type;
    c->index_size = d->index_buffer ? d->index_buffer->size : 0;
    c->index_off = (uint32_t)(uintptr_t)d->indices;
    c->textures = (uint32_t)d->texture_count;
    c->attribs = (uint32_t)d->attrib_count;
    for (int i = 0; i < 4; i++) {
        const struct gb_attrib *a = i < d->attrib_count ? &d->attribs[i] : NULL;
        c->vbuf_size[i] = a && a->enabled ? (a->buffer ? a->buffer->size : 0xFFFFFFFFu) : 0;
        c->vbuf_off[i] = a ? (uint32_t)(uintptr_t)a->pointer : 0;
        c->stride[i] = a ? (uint32_t)a->stride : 0;
    }
    for (int i = 0; i < 3; i++) {
        struct gb_texture *t = i < d->texture_count ? d->textures[i] : NULL;
        struct gtex *x = t ? t->priv : NULL;
        c->tex_desc[i] = !t ? -2 : x && x->realized ? x->descriptor : -1;
        c->tex_w[i] = t ? t->width : 0;
        c->tex_h[i] = t ? t->height : 0;
        c->tex_fmt[i] = t ? (int)t->format : 0;
        c->tex_cube[i] = t ? t->cube : 0;
    }
}

static void gpu_error_report(const char *why)
{
    static int done;
    if (done++)
        return;
    FILE *f = fopen("sdmc:/switch/cod_st/gpu_error.txt", "w");
    if (!f)
        return;
    fprintf(f, "%s at frame %llu; last %u draws (oldest first)\n", why, (unsigned long long)g.frame_number,
            CRUMBS);
    fprintf(f, "frame record mode count idx_type idx_buf idx_off tex attribs | per attrib: buf_size/offset/stride\n");
    for (unsigned i = 0; i < CRUMBS; i++) {
        const struct crumb *c = &g_crumbs[(g_crumb_next + i) % CRUMBS];
        if (!c->frame && !c->count)
            continue;
        fprintf(f, "%llu %x %u %u %x %u %u %u %u |", (unsigned long long)c->frame, c->record, c->mode, c->count,
                c->index_type, c->index_size, c->index_off, c->textures, c->attribs);
        for (int k = 0; k < 4; k++)
            fprintf(f, " %d/%u/%u", (int)c->vbuf_size[k], c->vbuf_off[k], c->stride[k]);
        fprintf(f, " | tex");
        for (int k = 0; k < 3; k++)
            if (c->tex_desc[k] != -2)
                fprintf(f, " d%d:%dx%d:f%d%s", c->tex_desc[k], c->tex_w[k], c->tex_h[k], c->tex_fmt[k],
                        c->tex_cube[k] ? ":cube" : "");
        fputc('\n', f);
    }
    fprintf(f, "\nother GPU work (draw = index of the next draw above, counted from the start)\n");
    for (unsigned i = 0; i < EVENTS; i++) {
        const struct event *ev = &g_events[(g_event_next + i) % EVENTS];
        if (!ev->what)
            continue;
        fprintf(f, "%llu draw#%u %s %d %d %d %d %d %d\n", (unsigned long long)ev->frame, ev->draw, ev->what,
                ev->v[0], ev->v[1], ev->v[2], ev->v[3], ev->v[4], ev->v[5]);
    }
    fprintf(f, "draws recorded in all: %u, descriptors in use up to %d\n", g_crumb_next, g.next_descriptor);
    fclose(f);
}

void gb_draw(const struct gb_draw *d)
{
    if (capture_on())
        capture_draw(d); // before the override, so the game's own values are recorded
    fog_override(d);
    begin_frame();
    struct frame *f = cur();
    if (f->cmd_used + CMD_MARGIN > CMD_SIZE)
        flush_and_wait();

    // Everything that can restart recording happens first, so the addresses gathered below
    // stay valid: uploads, then a reservation big enough for this whole draw.
    for (int i = 0; i < d->texture_count; i++)
        if (d->textures[i])
            realize(d->textures[i], 0);
    if (!g.target_is_set)
        apply_target();

    uint32_t vertices = 0;
    int have_client = 0;
    for (int i = 0; i < d->attrib_count; i++)
        have_client |= d->attribs[i].enabled && !d->attribs[i].buffer;
    if (have_client)
        vertices = d->index_type ? highest_index(d) + 1 : (uint32_t)(d->first + d->count);

    uint32_t need = d->vs_block_size + d->fs_block_size + 1024;
    for (int i = 0; i < d->attrib_count; i++) {
        const struct gb_attrib *a = &d->attribs[i];
        if (!a->enabled)
            continue;
        if (a->buffer)
            need += ring_need(a->buffer);
        else
            need += vertices * (uint32_t)(a->stride ? a->stride : attrib_bytes(a)) + 32;
    }
    uint32_t index_size = d->index_type == 0x1405 ? 4 : d->index_type == 0x1403 ? 2 : 1;
    if (d->index_type)
        need += d->index_buffer ? ring_need(d->index_buffer) : (uint32_t)d->count * index_size + 32;
    if (need > RING_SIZE) {
        g.draws_skipped++;
        if (g_cap)
            fprintf(g_cap, "  SKIPPED: needs %u bytes of ring\n", need);
        return;
    }
    if (align_up(f->ring_used, 256) + need > RING_SIZE) {
        flush_and_wait();
        apply_target();
    }

    DkCmdBuf cmd = f->cmd;
    apply_state(d->state);

    const struct gprogram *p = d->program;
    if (g.bound_program != p) {
        const DkShader *shaders[2] = {&p->shaders[0], &p->shaders[1]};
        dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
        g.bound_program = p;
    }

    if (d->vs_block_size) {
        DkGpuAddr gpu;
        memcpy(ring_alloc(d->vs_block_size, DK_UNIFORM_BUF_ALIGNMENT, &gpu), d->vs_block, d->vs_block_size);
        dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, gpu, d->vs_block_size);
    }
    if (d->fs_block_size) {
        DkGpuAddr gpu;
        memcpy(ring_alloc(d->fs_block_size, DK_UNIFORM_BUF_ALIGNMENT, &gpu), d->fs_block, d->fs_block_size);
        dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, gpu, d->fs_block_size);
    }

    for (int i = 0; i < d->texture_count; i++) {
        struct gb_texture *t = d->textures[i];
        struct gtex *x = t ? gtex_of(t) : NULL;
        DkResHandle handle;
        if (x && x->realized && t->format < GB_D16) {
            handle = dkMakeTextureHandle((uint32_t)x->descriptor, (uint32_t)sampler_id(t, x->levels));
        } else {
            static const struct gb_texture plain = {.min_filter = 0x2600, .mag_filter = 0x2600, .wrap_s = 0x812F, .wrap_t = 0x812F, .anisotropy = 1.0f};
            handle = dkMakeTextureHandle((uint32_t)black_texture(), (uint32_t)sampler_id(&plain, 1));
        }
        dkCmdBufBindTexture(cmd, DkStage_Fragment, (uint32_t)i, handle);
        dkCmdBufBindTexture(cmd, DkStage_Vertex, (uint32_t)i, handle);
    }

    DkVtxAttribState states[GB_MAX_ATTRIBS];
    DkVtxBufferState buffers[GB_MAX_ATTRIBS];
    memset(states, 0, sizeof states);
    memset(buffers, 0, sizeof buffers);
    for (int i = 0; i < d->attrib_count; i++) {
        const struct gb_attrib *a = &d->attribs[i];
        states[i].bufferId = (uint32_t)i;
        // An attribute fed from a buffer with nothing at or after its offset (run 22-23: a 0-byte
        // buffer, offset 12, stride 124, on the campaign's first frames) would be bound with size 0:
        // the hardware's limit is start + size - 1, which wraps, so the fetch is unbounded.  The
        // shader gets the constant 0 instead, as GL gives for a disabled array.
        int empty = a->enabled && a->buffer && (!a->buffer->size || a->buffer->size <= (uint32_t)(uintptr_t)a->pointer);
        if (!a->enabled || empty) {
            states[i].isFixed = 1;
            states[i].size = DkVtxAttribSize_4x32;
            states[i].type = DkVtxAttribType_Float;
            continue;
        }
        states[i].size = attrib_size(a->type, a->components);
        states[i].type = attrib_type(a->type, a->normalized);
        uint32_t stride = (uint32_t)(a->stride ? a->stride : attrib_bytes(a));
        buffers[i].stride = stride;
        if (a->buffer) {
            uint32_t offset = (uint32_t)(uintptr_t)a->pointer;
            DkGpuAddr base = buffer_address(a->buffer);
            uint32_t available = a->buffer->size > offset ? a->buffer->size - offset : 0;
            dkCmdBufBindVtxBuffer(cmd, (uint32_t)i, base + offset, available);
        } else {
            uint32_t size = vertices * stride;
            dkCmdBufBindVtxBuffer(cmd, (uint32_t)i, ring_copy(a->pointer, size), size);
        }
    }
    if (d->attrib_count != g.attrib_count || memcmp(states, g.attrib_states, sizeof states) != 0) {
        dkCmdBufBindVtxAttribState(cmd, states, (uint32_t)d->attrib_count);
        memcpy(g.attrib_states, states, sizeof states);
    }
    if (d->attrib_count != g.attrib_count || memcmp(buffers, g.buffer_states, sizeof buffers) != 0) {
        dkCmdBufBindVtxBufferState(cmd, buffers, (uint32_t)d->attrib_count);
        memcpy(g.buffer_states, buffers, sizeof buffers);
    }
    g.attrib_count = d->attrib_count;

    DkPrimitive prim = (DkPrimitive)d->mode; // POINTS..TRIANGLE_FAN are in the same order
    uint32_t count = (uint32_t)d->count;
    // The index binding has no size, so indices past the buffer's end are read from whatever
    // follows it, unmapped memory included: a GPU fault, then deko3d aborts in the next
    // dkQueueAcquireImage (runs 18-21, campaign start: "2359-0001", no CPU fault).
    if (d->index_type && d->index_buffer) {
        uint32_t off = (uint32_t)(uintptr_t)d->indices, size = d->index_buffer->size;
        if (off >= size || (uint64_t)off + (uint64_t)count * index_size > size) {
            static int shown;
            if (shown++ < 20)
                fprintf(stderr, "deko3d: indexed draw of %u from +%u past a %u byte index buffer: %s\n", count, off,
                        size, off >= size ? "skipped" : "clamped");
            if (off >= size)
                return;
            count = (size - off) / index_size;
        }
    }
    crumb(d, count);
    if (d->index_type) {
        DkIdxFormat format = d->index_type == 0x1405 ? DkIdxFormat_Uint32
                             : d->index_type == 0x1403 ? DkIdxFormat_Uint16
                                                       : DkIdxFormat_Uint8;
        DkGpuAddr address;
        if (d->index_buffer)
            address = buffer_address(d->index_buffer) + (uint32_t)(uintptr_t)d->indices;
        else
            address = ring_copy(d->indices, (uint32_t)d->count * index_size);
        dkCmdBufBindIdxBuffer(cmd, format, address);
        dkCmdBufDrawIndexed(cmd, prim, count, 1, 0, 0, 0);
    } else {
        dkCmdBufDraw(cmd, prim, count, 1, (uint32_t)d->first, 0);
    }
}

// ---- present ---------------------------------------------------------------------

// The virtual mouse's pointer (src/input_pad.c), drawn on the finished screen image: a blue arrow
// whose tip is the point a click touches.  Clears inside a scissor need no shader.
static struct {
    int visible, x, y;
} g_cursor;

void gb_set_cursor(int visible, int x, int y)
{
    g_cursor.visible = visible;
    g_cursor.x = x;
    g_cursor.y = y;
}

static void cursor_rect(DkCmdBuf cmd, int x, int y, int w, int h, float r, float gr, float b)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > RENDERER_WIDTH)
        w = RENDERER_WIDTH - x;
    if (y + h > RENDERER_HEIGHT)
        h = RENDERER_HEIGHT - y;
    if (w <= 0 || h <= 0)
        return;
    DkScissor sc = {(uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h};
    dkCmdBufSetScissors(cmd, 0, &sc, 1);
    dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, r, gr, b, 1.0f);
}

static void draw_cursor(DkCmdBuf cmd, const DkImageView *screen)
{
    const DkImageView *target = screen;
    dkCmdBufBindRenderTargets(cmd, &target, 1, NULL);
    // The two-tone blue arrow the user drew: tip at the click point, a straight left edge, the
    // right edge running down to a point at 71% of the height, then back in to 40% of the width
    // and down to the bottom of the left edge.  Dark facet: tip, inner corner, bottom.  Light
    // facet: tip, right point, inner corner.  Drawn one pixel row at a time.
    const float h = 30.0f, w = 21.0f, yr = h * 0.71f, xi = w * 0.40f;
    int x = g_cursor.x, y = g_cursor.y;
    for (int r = 0; r < (int)h; r++) {
        float fy = (float)r + 0.5f;
        if (fy <= yr) {
            int split = (int)(fy * xi / yr + 0.5f), right = (int)(fy * w / yr + 0.5f);
            if (split > 0)
                cursor_rect(cmd, x, y + r, split, 1, 0.08f, 0.47f, 0.78f);
            if (right > split)
                cursor_rect(cmd, x + split, y + r, right - split, 1, 0.13f, 0.59f, 0.95f);
        } else {
            int edge = (int)(xi * (h - fy) / (h - yr) + 0.5f);
            if (edge > 0)
                cursor_rect(cmd, x, y + r, edge, 1, 0.08f, 0.47f, 0.78f);
        }
    }
    dkCmdBufBarrier(cmd, DkBarrier_Fragments, 0);
    // The game's target and scissor are set again at the next frame's first draw.
    g.target_is_set = 0;
    g.bound.scissor_test = 255;
}

void gb_set_screen_size(int width, int height)
{
    if (width > 0 && height > 0 && width <= RENDERER_WIDTH && height <= RENDERER_HEIGHT) {
        g.screen_width = width;
        g.screen_height = height;
    }
}

void gles_backend_init(void)
{
    extern void gles_state_init(void);
    gles_state_init();
}

void gles_backend_present(void)
{
    begin_frame();
    struct frame *f = cur();
    realize_queued();
    apply_target(); // so a frame that drew nothing still has defined contents
    u64 acquire = armGetSystemTick();
    gb_stats.record_ticks += acquire - g.recording_since;
    gb_stats.stage = "acquiring swapchain image";
    if (dkQueueIsInErrorState(g.queue))
        gpu_error_report("GPU queue in error state before acquiring a swapchain image");
    int slot = dkQueueAcquireImage(g.queue, g.swapchain);
    gb_stats.acquire_ticks += armGetSystemTick() - acquire;
    gb_stats.stage = "presenting";

    DkImageView src, dst;
    dkImageViewDefaults(&src, &g.back_color);
    dkImageViewDefaults(&dst, &g.swap_images[slot]);
    DkImageRect from = {0, 0, 0, (uint32_t)g.screen_width, (uint32_t)g.screen_height, 1};
    DkImageRect to = {0, 0, 0, RENDERER_WIDTH, RENDERER_HEIGHT, 1};
    dkCmdBufBarrier(f->cmd, DkBarrier_Fragments, 0);
    dkCmdBufBlitImage(f->cmd, &src, &from, &dst, &to, DkBlitFlag_FlipY | DkBlitFlag_FilterLinear, 0);
    dkCmdBufBarrier(f->cmd, DkBarrier_Fragments, 0);
    if (g_cursor.visible)
        draw_cursor(f->cmd, &dst);

    dkQueueSubmitCommands(g.queue, dkCmdBufFinishList(f->cmd));
    dkQueueSignalFence(g.queue, &f->fence, false);
    f->fence_pending = 1;
    dkQueuePresentImage(g.queue, g.swapchain, slot);
    g.frame_open = 0;
    g.current = (g.current + 1) % NUM_FRAMES;
    gb_stats.stage = "idle";
    g.frame_ended = armGetSystemTick();

    if (dkQueueIsInErrorState(g.queue)) {
        static int told;
        if (!told++)
            fprintf(stderr, "deko3d: the queue is in an error state\n");
    }
}
