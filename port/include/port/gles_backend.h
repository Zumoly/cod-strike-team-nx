// What src/gl/gles.c needs from whichever backend is linked: deko3D on the console,
// stubs in the emulated harness.  gles.c owns API state and format conversion; a backend
// only ever sees tightly packed data, resolved state and hardware attribute locations.
#ifndef PORT_GLES_BACKEND_H
#define PORT_GLES_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#define GB_MAX_ATTRIBS 16
#define GB_MAX_SAMPLERS 16
#define GB_MAX_LEVELS 14

enum gb_format {
    GB_NONE,
    GB_RGBA8,
    GB_BGRA8,
    GB_RGBA4,
    GB_RGB5A1,
    GB_RGB565,
    GB_A8,
    GB_L8,
    GB_LA8,
    GB_ETC1,
    GB_BC1,
    GB_BC1A,
    GB_BC2,
    GB_BC3,
    GB_D16,
    GB_D24S8,
    GB_S8,
};

struct gb_buffer {
    uint8_t *data; // CPU shadow, always current
    uint32_t size;
    uint32_t dirty_lo, dirty_hi; // lo >= hi means clean
    uint64_t last_change;        // frame number
    void *priv;
    uint64_t ring_addr;
    uint8_t *ring_cpu;
    uint32_t ring_epoch, ring_written;
};

struct gb_texture {
    enum gb_format format;
    int cube;
    int width, height, levels;
    uint32_t min_filter, mag_filter, wrap_s, wrap_t;
    float anisotropy;
    void *priv;
};

struct gb_attrib {
    int enabled, components, normalized, stride;
    uint32_t type;
    struct gb_buffer *buffer; // NULL: client memory
    const void *pointer;      // offset into buffer, or client address
};

struct gb_state {
    uint8_t blend, depth_test, depth_write, cull, scissor_test, stencil_test;
    uint8_t color_mask; // bit 0 red .. bit 3 alpha
    uint32_t blend_op_rgb, blend_op_alpha, blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha;
    uint32_t depth_func, cull_face, front_face;
    int viewport[4], scissor[4];
    uint32_t stencil_func[2], stencil_ref[2], stencil_read_mask[2], stencil_write_mask[2];
    uint32_t stencil_fail[2], stencil_zfail[2], stencil_zpass[2];
};

struct gb_draw {
    void *program;
    const struct gb_state *state;
    const void *vs_block, *fs_block;
    uint32_t vs_block_size, fs_block_size;
    struct gb_attrib attribs[GB_MAX_ATTRIBS]; // by hardware location
    int attrib_count;
    struct gb_texture *textures[GB_MAX_SAMPLERS]; // by binding
    int texture_count;
    uint32_t mode;
    int first, count;              // first is unused when indexed
    uint32_t index_type;           // 0: not indexed
    struct gb_buffer *index_buffer; // NULL: client memory
    const void *indices;
};

// One program as stored in shaders.pak; see tools/translate_shaders.py.
struct pak_record {
    uint32_t vs_control, vs_control_size, vs_code;
    uint32_t fs_control, fs_control_size, fs_code;
    uint32_t vs_block, fs_block;
    uint16_t attribute_count, sampler_count, uniform_count, pad;
};

// Read by src/watchdog.c.  `stage` names what the renderer is doing, or waiting for.
struct gb_stats_t {
    const char *volatile stage;
    volatile uint64_t image_bytes, buffer_bytes, upload_bytes;
    volatile uint32_t textures, flushes;
    // Where a frame's time goes: the game before its first GL call, waiting for the GPU, the draw
    // calls, and waiting for a swapchain image.
    volatile uint64_t update_ticks, fence_ticks, record_ticks, acquire_ticks;
};
extern struct gb_stats_t gb_stats;

void gles_backend_init(void);
void gles_backend_present(void);

// The game can ask for less than the display.  On Android the system scales that up.
void gb_set_screen_size(int width, int height);
void gb_set_cursor(int visible, int x, int y); // virtual mouse pointer, screen pixels

const char *gb_pak_path(void);
void gb_pak_loaded(const uint8_t *pak, uint32_t code_offset, uint32_t code_size);
void *gb_program_create(const uint8_t *pak, const struct pak_record *record);
void gb_shader_missing(uint64_t key, const char *vs, const char *fs);

void gb_buffer_release(struct gb_buffer *b);

void gb_tex_data(struct gb_texture *t, int face, int level, int w, int h, enum gb_format f, const void *data,
                 uint32_t size);
void gb_tex_sub(struct gb_texture *t, int face, int level, int x, int y, int w, int h, const void *data,
                uint32_t size);
void gb_tex_mipmap(struct gb_texture *t);
void gb_tex_release(struct gb_texture *t);
void gb_copy_framebuffer(struct gb_texture *t, int face, int level, int xoff, int yoff, int x, int y, int w, int h);

// NULL color and depth select the default framebuffer.
void gb_bind_target(struct gb_texture *color, struct gb_texture *depth, struct gb_texture *stencil);
void gb_clear(const struct gb_state *s, int color, int depth, int stencil, const float rgba[4], float z, int sv);
void gb_draw(const struct gb_draw *d);

uint64_t gles_draw_count(void);
uint64_t gles_frame_count(void);
uint64_t gles_missing_program_count(void);

#endif
