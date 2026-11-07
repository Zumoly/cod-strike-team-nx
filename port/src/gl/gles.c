// OpenGL ES 2.0 and EGL as libunity sees them.  This file owns API state only; drawing
// is handed to the renderer interface so the logic also runs with no GPU in tests.
#include "port/gles_backend.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GL_NO_ERROR 0
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_ATTACHED_SHADERS 0x8B85
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_ACTIVE_UNIFORM_MAX_LENGTH 0x8B87
#define GL_ACTIVE_ATTRIBUTES 0x8B89
#define GL_ACTIVE_ATTRIBUTE_MAX_LENGTH 0x8B8A
#define GL_SHADER_SOURCE_LENGTH 0x8B88
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893

#define EGL_SUCCESS 0x3000
#define EGL_BUFFER_SIZE 0x3020
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_CONFIG_ID 0x3028
#define EGL_SAMPLES 0x3031
#define EGL_SAMPLE_BUFFERS 0x3032
#define EGL_SURFACE_TYPE 0x3033
#define EGL_RENDERABLE_TYPE 0x3040

#define MAX_OBJECTS 8192
#define MAX_UNIFORMS 96
#define MAX_ATTRIBS GB_MAX_ATTRIBS
#define MAX_COPIES 192

struct shader {
    int used;
    char *source;
};

struct uniform {
    char name[64];
    uint32_t type;
    int count;
    int offset;  // into program->values, in floats
    int size;    // floats per element
    int columns; // vectors per element: 1, or the matrix dimension
    int binding; // samplers: the pak's binding, or -1
};

struct attribute {
    char name[48];
    uint32_t type;
    int location;     // what the game addresses
    int hw_location;  // what the shader was compiled with, or -1
};

// One uniform's move from the packed shadow to a std140 block.
struct block_copy {
    uint16_t dst, src, rows, comps;
};

struct program {
    int used;
    uint32_t shaders[2];
    struct uniform uniforms[MAX_UNIFORMS];
    int uniform_count;
    struct attribute attributes[MAX_ATTRIBS];
    int attribute_count;
    struct {
        char name[48];
        int location;
    } bound[MAX_ATTRIBS];
    int bound_count;
    float *values;
    void *backend;
    uint8_t *blocks[2];
    uint32_t block_size[2];
    struct block_copy copies[2][MAX_COPIES];
    int copy_count[2];
    int dirty;
    int sampler_count;
    int checked; // first-draw diagnostics written
};

struct vertex_attrib {
    int enabled, size, normalized, stride;
    uint32_t type;
    const void *pointer;
    uint32_t buffer;
};

struct framebuffer {
    uint32_t color, depth, stencil;  // object ids
    uint8_t depth_is_rb, stencil_is_rb;
};

static struct {
    uint32_t next_id;
    struct shader shaders[MAX_OBJECTS];
    struct program *programs[MAX_OBJECTS];
    struct gb_buffer buffers[MAX_OBJECTS];
    struct gb_texture *textures[MAX_OBJECTS]; // renderbuffers live here too
    struct framebuffer framebuffers[MAX_OBJECTS];
    uint32_t array_buffer, element_buffer, current_program, framebuffer, renderbuffer;
    uint32_t active_texture;
    uint32_t bound_2d[32], bound_cube[32];
    struct vertex_attrib attribs[MAX_ATTRIBS];
    uint64_t draws, frames, missing;
    int unpack_alignment;
    int target_dirty;
    float clear_color[4], clear_depth;
    int clear_stencil;
    struct gb_state state;
    const uint8_t *pak;
    uint32_t pak_count;
} g;

static void pak_load(void);

// Which ids are taken.  The counter used to wrap to 1 and hand out ids still in use: by survival
// wave 3 (8192 objects made) a new 12-byte buffer took over Unity's 56 KB dynamic vertex buffer,
// and textures and programs collided too: the garbage graphics, the overruns and the wave 5 crash.
static uint8_t g_live[MAX_OBJECTS];

static uint32_t new_id(void)
{
    for (int tries = 0; tries < MAX_OBJECTS; tries++) {
        if (g.next_id == 0 || g.next_id >= MAX_OBJECTS)
            g.next_id = 1;
        uint32_t id = g.next_id++;
        if (!g_live[id]) {
            g_live[id] = 1;
            return id;
        }
    }
    static int shown;
    if (!shown++)
        fprintf(stderr, "gles: all %d object ids are in use\n", MAX_OBJECTS);
    return MAX_OBJECTS - 1;
}

static int valid_id(uint32_t id) { return id > 0 && id < MAX_OBJECTS; }

static void free_id(uint32_t id)
{
    if (valid_id(id))
        g_live[id] = 0;
}

// ---- EGL --------------------------------------------------------------------

void *shim_eglGetDisplay(void *d) { (void)d; return (void *)1; }

int shim_eglInitialize(void *d, int *major, int *minor)
{
    (void)d;
    if (major)
        *major = 1;
    if (minor)
        *minor = 4;
    return 1;
}

// What Unity selects from depends on its player settings: this build asks for 565.  The
// choice only affects what Unity believes; presentation is always 8-bit.
static const struct egl_config {
    int red, green, blue, alpha, depth, stencil;
} g_configs[] = {
    {5, 6, 5, 0, 24, 8},
    {5, 6, 5, 0, 16, 0},
    {8, 8, 8, 8, 24, 8},
    {8, 8, 8, 0, 24, 8},
};
#define CONFIG_COUNT ((int)(sizeof g_configs / sizeof *g_configs))

int shim_eglChooseConfig(void *d, const int *attrs, void **configs, int size, int *count)
{
    (void)d;
    (void)attrs;
    int n = CONFIG_COUNT;
    if (configs) {
        if (n > size)
            n = size;
        for (int i = 0; i < n; i++)
            configs[i] = (void *)(uintptr_t)(i + 1);
    }
    if (count)
        *count = n;
    return 1;
}

int shim_eglGetConfigAttrib(void *d, void *c, int attr, int *value)
{
    (void)d;
    int index = (int)(uintptr_t)c - 1;
    if (index < 0 || index >= CONFIG_COUNT)
        index = 0;
    const struct egl_config *config = &g_configs[index];
    switch (attr) {
    case EGL_RED_SIZE: *value = config->red; break;
    case EGL_GREEN_SIZE: *value = config->green; break;
    case EGL_BLUE_SIZE: *value = config->blue; break;
    case EGL_ALPHA_SIZE: *value = config->alpha; break;
    case EGL_BUFFER_SIZE: *value = config->red + config->green + config->blue + config->alpha; break;
    case EGL_DEPTH_SIZE: *value = config->depth; break;
    case EGL_STENCIL_SIZE: *value = config->stencil; break;
    case EGL_CONFIG_ID: *value = index + 1; break;
    case 0x3027: *value = 0x3038; break;        // EGL_CONFIG_CAVEAT: EGL_NONE
    case 0x302D: *value = 1; break;             // EGL_NATIVE_RENDERABLE
    case 0x3042: *value = 4; break;             // EGL_CONFORMANT: ES2
    case EGL_SURFACE_TYPE: *value = 4; break;   // EGL_WINDOW_BIT
    case EGL_RENDERABLE_TYPE: *value = 4; break; // EGL_OPENGL_ES2_BIT
    default: *value = 0; break;
    }
    return 1;
}

void *shim_eglCreateWindowSurface(void *d, void *c, void *w, const int *a)
{
    (void)d;
    (void)c;
    (void)w;
    (void)a;
    return (void *)1;
}

void *shim_eglCreateContext(void *d, void *c, void *s, const int *a)
{
    (void)d;
    (void)c;
    (void)s;
    (void)a;
    return (void *)1;
}

int shim_eglMakeCurrent(void *d, void *r, void *w, void *c)
{
    (void)d;
    (void)r;
    (void)w;
    if (c) {
        gles_backend_init();
        pak_load();
    }
    return 1;
}

int shim_eglDestroySurface(void *d, void *s) { (void)d; (void)s; return 1; }
int shim_eglDestroyContext(void *d, void *c) { (void)d; (void)c; return 1; }
int shim_eglTerminate(void *d) { (void)d; return 1; }
int shim_eglGetError(void) { return EGL_SUCCESS; }
void *shim_eglGetProcAddress(const char *n) { (void)n; return NULL; }

int shim_eglSwapBuffers(void *d, void *s)
{
    (void)d;
    (void)s;
    g.frames++;
    gles_backend_present();
    return 1;
}

// ---- queries ----------------------------------------------------------------

const char *shim_glGetString(uint32_t name)
{
    switch (name) {
    case GL_VENDOR: return "NVIDIA Corporation";
    case GL_RENDERER: return "NVIDIA Tegra";
    case GL_VERSION: return "OpenGL ES 2.0";
    case GL_SHADING_LANGUAGE_VERSION: return "OpenGL ES GLSL ES 1.00";
    // PVRTC is left out because the Tegra cannot sample it.
    case GL_EXTENSIONS:
        return "GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_rgb8_rgba8 GL_OES_depth_texture "
               "GL_OES_compressed_ETC1_RGB8_texture GL_EXT_texture_compression_s3tc "
               "GL_OES_standard_derivatives GL_EXT_texture_filter_anisotropic";
    default: return "";
    }
}

void shim_glGetIntegerv(uint32_t name, int *v)
{
    switch (name) {
    case 0x0D33: *v = 4096; break;  // MAX_TEXTURE_SIZE
    case 0x851C: *v = 4096; break;  // MAX_CUBE_MAP_TEXTURE_SIZE
    case 0x84E8: *v = 4096; break;  // MAX_RENDERBUFFER_SIZE
    case 0x8869: *v = MAX_ATTRIBS; break; // MAX_VERTEX_ATTRIBS
    case 0x8872: *v = GB_MAX_SAMPLERS; break; // MAX_TEXTURE_IMAGE_UNITS
    case 0x8B4C: *v = 16; break;    // MAX_VERTEX_TEXTURE_IMAGE_UNITS
    case 0x8B4D: *v = 32; break;    // MAX_COMBINED_TEXTURE_IMAGE_UNITS
    case 0x8DFB: *v = 256; break;   // MAX_VERTEX_UNIFORM_VECTORS
    case 0x8DFD: *v = 256; break;   // MAX_FRAGMENT_UNIFORM_VECTORS
    case 0x8DFC: *v = 16; break;    // MAX_VARYING_VECTORS
    case 0x0D52:
    case 0x0D53:
    case 0x0D54:
    case 0x0D55: *v = 8; break;     // RED/GREEN/BLUE/ALPHA_BITS
    case 0x0D56: *v = 24; break;    // DEPTH_BITS
    case 0x0D57: *v = 8; break;     // STENCIL_BITS
    case GL_FRAMEBUFFER_BINDING: *v = (int)g.framebuffer; break;
    case GL_RENDERBUFFER_BINDING: *v = (int)g.renderbuffer; break;
    case 0x8B9A: *v = 0x1401; break; // IMPLEMENTATION_COLOR_READ_TYPE: UNSIGNED_BYTE
    case 0x8B9B: *v = 0x1908; break; // IMPLEMENTATION_COLOR_READ_FORMAT: RGBA
    default: *v = 0; break;
    }
}

uint32_t shim_glGetError(void) { return GL_NO_ERROR; }

// ---- object names -----------------------------------------------------------

#define GEN(fn)                               \
    void fn(int n, uint32_t *out)             \
    {                                         \
        for (int i = 0; i < n; i++)           \
            out[i] = new_id();                \
    }

GEN(shim_glGenFramebuffers)
GEN(shim_glGenRenderbuffers)
GEN(shim_glGenBuffers)

static struct gb_texture *texture_object(uint32_t id)
{
    if (!id)
        return NULL;
    if (!g.textures[id]) {
        struct gb_texture *t = calloc(1, sizeof *t);
        t->min_filter = 0x2702; // NEAREST_MIPMAP_LINEAR, the GL default
        t->mag_filter = 0x2601;
        t->wrap_s = t->wrap_t = 0x2901;
        t->anisotropy = 1.0f;
        g.textures[id] = t;
    }
    return g.textures[id];
}

void shim_glGenTextures(int n, uint32_t *out)
{
    for (int i = 0; i < n; i++)
        out[i] = new_id();
}

static void release_texture(uint32_t id)
{
    if (g.textures[id]) {
        gb_tex_release(g.textures[id]);
        free(g.textures[id]);
        g.textures[id] = NULL;
    }
}

void shim_glDeleteTextures(int n, const uint32_t *ids)
{
    for (int i = 0; i < n; i++) {
        if (!valid_id(ids[i]))
            continue;
        release_texture(ids[i]);
        free_id(ids[i]);
        for (int u = 0; u < 32; u++) {
            if (g.bound_2d[u] == ids[i])
                g.bound_2d[u] = 0;
            if (g.bound_cube[u] == ids[i])
                g.bound_cube[u] = 0;
        }
    }
    g.target_dirty = 1;
}

void shim_glDeleteRenderbuffers(int n, const uint32_t *ids)
{
    for (int i = 0; i < n; i++) {
        if (!valid_id(ids[i]))
            continue;
        release_texture(ids[i]);
        free_id(ids[i]);
    }
    g.target_dirty = 1;
}

void shim_glDeleteFramebuffers(int n, const uint32_t *ids)
{
    for (int i = 0; i < n; i++) {
        if (!valid_id(ids[i]))
            continue;
        free_id(ids[i]);
        memset(&g.framebuffers[ids[i]], 0, sizeof g.framebuffers[0]);
        if (g.framebuffer == ids[i]) {
            g.framebuffer = 0;
            g.target_dirty = 1;
        }
    }
}

void shim_glDeleteBuffers(int n, const uint32_t *ids)
{
    for (int i = 0; i < n; i++) {
        if (!valid_id(ids[i]))
            continue;
        struct gb_buffer *b = &g.buffers[ids[i]];
        gb_buffer_release(b);
        free(b->data);
        memset(b, 0, sizeof *b);
        free_id(ids[i]);
        if (g.array_buffer == ids[i])
            g.array_buffer = 0;
        if (g.element_buffer == ids[i])
            g.element_buffer = 0;
    }
}

// ---- buffers ----------------------------------------------------------------

void shim_glBindBuffer(uint32_t target, uint32_t id)
{
    if (target == GL_ARRAY_BUFFER)
        g.array_buffer = id;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        g.element_buffer = id;
}

static struct gb_buffer *target_buffer(uint32_t target)
{
    return &g.buffers[target == GL_ARRAY_BUFFER ? g.array_buffer : g.element_buffer];
}

static void mark_dirty(struct gb_buffer *b, uint32_t lo, uint32_t hi)
{
    if (b->dirty_lo >= b->dirty_hi) {
        b->dirty_lo = lo;
        b->dirty_hi = hi;
    } else {
        if (lo < b->dirty_lo)
            b->dirty_lo = lo;
        if (hi > b->dirty_hi)
            b->dirty_hi = hi;
    }
    b->last_change = g.frames;
}

void shim_glBufferData(uint32_t target, intptr_t size, const void *data, uint32_t usage)
{
    (void)usage;
    struct gb_buffer *b = target_buffer(target);
    if ((uint32_t)size != b->size) {
        gb_buffer_release(b);
        free(b->data);
        b->data = malloc((size_t)size);
        b->size = (uint32_t)size;
    }
    if (data && b->data)
        memcpy(b->data, data, (size_t)size);
    mark_dirty(b, 0, (uint32_t)size);
}

void shim_glBufferSubData(uint32_t target, intptr_t offset, intptr_t size, const void *data)
{
    struct gb_buffer *b = target_buffer(target);
    if (b->data && (offset < 0 || size < 0 || (uint64_t)offset + (uint64_t)size > b->size)) {
        static int shown;
        if (shown++ < 30)
            fprintf(stderr, "gles: glBufferSubData %d+%d past a %u byte buffer: grown (target 0x%x, buffer %u)\n",
                    (int)offset, (int)size, (unsigned)b->size, (unsigned)target,
                    (unsigned)(target == GL_ARRAY_BUFFER ? g.array_buffer : g.element_buffer));
        if (offset < 0 || size < 0 || (uint64_t)offset + (uint64_t)size > (16u << 20))
            return;
        // Run 13: a rotating pool of Unity's dynamic vertex buffers, 12 or 144 bytes as far as the
        // shim knows, is filled with up to 56 KB.  Before the bounds check that wrote over the heap;
        // clipping it drew garbage.  Grow the buffer to hold the data, keeping what it had.
        uint32_t need = (uint32_t)(offset + size);
        uint8_t *grown = realloc(b->data, need);
        if (!grown)
            return;
        memset(grown + b->size, 0, need - b->size);
        gb_buffer_release(b);
        b->data = grown;
        b->size = need;
        mark_dirty(b, 0, need);
    }
    if (b->data) {
        memcpy(b->data + offset, data, (size_t)size);
        mark_dirty(b, (uint32_t)offset, (uint32_t)(offset + size));
    }
}

// ---- shaders ----------------------------------------------------------------

uint32_t shim_glCreateShader(uint32_t type)
{
    (void)type;
    uint32_t id = new_id();
    g.shaders[id].used = 1;
    return id;
}

void shim_glDeleteShader(uint32_t id)
{
    if (!valid_id(id))
        return;
    free(g.shaders[id].source);
    g.shaders[id].source = NULL;
    g.shaders[id].used = 0;
    free_id(id);
}

void shim_glShaderSource(uint32_t id, int count, const char *const *strings, const int *lengths)
{
    size_t total = 1;
    for (int i = 0; i < count; i++)
        total += lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
    char *text = malloc(total);
    size_t at = 0;
    for (int i = 0; i < count; i++) {
        size_t n = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
        memcpy(text + at, strings[i], n);
        at += n;
    }
    text[at] = '\0';
    free(g.shaders[id].source);
    g.shaders[id].source = text;
}

void shim_glCompileShader(uint32_t id) { (void)id; }

void shim_glGetShaderiv(uint32_t id, uint32_t name, int *v)
{
    switch (name) {
    case GL_COMPILE_STATUS: *v = 1; break;
    case GL_INFO_LOG_LENGTH: *v = 0; break;
    case GL_SHADER_SOURCE_LENGTH: *v = g.shaders[id].source ? (int)strlen(g.shaders[id].source) + 1 : 0; break;
    default: *v = 0; break;
    }
}

void shim_glGetShaderInfoLog(uint32_t id, int size, int *length, char *log)
{
    (void)id;
    if (length)
        *length = 0;
    if (size > 0)
        log[0] = '\0';
}

void shim_glGetShaderSource(uint32_t id, int size, int *length, char *out)
{
    const char *s = g.shaders[id].source ? g.shaders[id].source : "";
    int n = (int)strlen(s);
    if (n >= size)
        n = size - 1;
    memcpy(out, s, (size_t)n);
    out[n] = '\0';
    if (length)
        *length = n;
}

// ---- shaders.pak ------------------------------------------------------------

// Must match tools/translate_shaders.py: whitespace and precision statements are not hashed.
static uint64_t hash_stage(uint64_t h, const char *s)
{
    while (*s) {
        if (strncmp(s, "precision", 9) == 0 && isspace((unsigned char)s[9])) {
            const char *semi = strchr(s, ';');
            if (semi) {
                s = semi + 1;
                continue;
            }
        }
        if (!isspace((unsigned char)*s))
            h = (h ^ (uint8_t)*s) * 0x100000001B3ull;
        s++;
    }
    return h;
}

#define PAK_FALLBACK_KEY 1 // tools/translate_shaders.py

static uint64_t pak_key(const char *vs, const char *fs)
{
    uint64_t h = hash_stage(0xCBF29CE484222325ull, vs);
    h = (h ^ (uint8_t)'|') * 0x100000001B3ull;
    return hash_stage(h, fs);
}

static void pak_load(void)
{
    if (g.pak)
        return;
    FILE *f = fopen(gb_pak_path(), "rb");
    if (!f) {
        fprintf(stderr, "gles: cannot open %s, nothing will draw\n", gb_pak_path());
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size || memcmp(data, "CSP1", 4) != 0) {
        fprintf(stderr, "gles: bad shader pack\n");
        fclose(f);
        free(data);
        return;
    }
    fclose(f);
    uint32_t header[4];
    memcpy(header, data, sizeof header);
    g.pak = data;
    g.pak_count = header[1];
    gb_pak_loaded(data, header[2], header[3]);
    fprintf(stderr, "gles: shader pack has %u programs\n", (unsigned)g.pak_count);
}

static const struct pak_record *pak_find(uint64_t key)
{
    if (!g.pak)
        return NULL;
    uint32_t lo = 0, hi = g.pak_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const uint8_t *entry = g.pak + 16 + mid * 16;
        uint64_t k;
        uint32_t offset;
        memcpy(&k, entry, 8);
        memcpy(&offset, entry + 8, 4);
        if (k == key)
            return (const struct pak_record *)(const void *)(g.pak + offset);
        if (k < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

// ---- programs ---------------------------------------------------------------

uint32_t shim_glCreateProgram(void)
{
    uint32_t id = new_id();
    g.programs[id] = calloc(1, sizeof(struct program));
    g.programs[id]->used = 1;
    return id;
}

static void free_program_data(struct program *p)
{
    free(p->values);
    free(p->blocks[0]);
    free(p->blocks[1]);
    p->values = NULL;
    p->blocks[0] = p->blocks[1] = NULL;
}

void shim_glDeleteProgram(uint32_t id)
{
    if (!valid_id(id))
        return;
    if (g.programs[id]) {
        free_program_data(g.programs[id]);
        free(g.programs[id]);
        g.programs[id] = NULL;
    }
    free_id(id);
}

void shim_glAttachShader(uint32_t p, uint32_t s)
{
    struct program *pr = g.programs[p];
    if (pr)
        pr->shaders[pr->shaders[0] ? 1 : 0] = s;
}

void shim_glBindAttribLocation(uint32_t p, uint32_t index, const char *name)
{
    struct program *pr = g.programs[p];
    if (!pr || pr->bound_count == MAX_ATTRIBS)
        return;
    snprintf(pr->bound[pr->bound_count].name, sizeof pr->bound[0].name, "%s", name);
    pr->bound[pr->bound_count++].location = (int)index;
}

static const struct {
    const char *name;
    uint32_t gl;
    int floats, columns;
} g_types[] = {
    {"float", 0x1406, 1, 1}, {"vec2", 0x8B50, 2, 1}, {"vec3", 0x8B51, 3, 1}, {"vec4", 0x8B52, 4, 1},
    {"int", 0x1404, 1, 1},   {"ivec2", 0x8B53, 2, 1}, {"ivec3", 0x8B54, 3, 1}, {"ivec4", 0x8B55, 4, 1},
    {"bool", 0x8B56, 1, 1},  {"bvec2", 0x8B57, 2, 1}, {"bvec3", 0x8B58, 3, 1}, {"bvec4", 0x8B59, 4, 1},
    {"mat2", 0x8B5A, 4, 2},  {"mat3", 0x8B5B, 9, 3},  {"mat4", 0x8B5C, 16, 4},
    {"sampler2D", 0x8B5E, 1, 1}, {"samplerCube", 0x8B60, 1, 1}, {"sampler2DShadow", 0x8B62, 1, 1},
    {"sampler", 0x8B5E, 1, 1},
};

static int find_type(const char *name)
{
    for (size_t i = 0; i < sizeof g_types / sizeof *g_types; i++)
        if (strcmp(g_types[i].name, name) == 0)
            return (int)i;
    return -1;
}

// Reads "uniform [precision] type name[N];" and "attribute [precision] type name;".
// Unity's compiled shaders always use this one-declaration-per-line form.
static void scan_declarations(struct program *p, const char *text)
{
    for (const char *line = text; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
        while (*line == ' ' || *line == '\t')
            line++;
        int is_uniform = strncmp(line, "uniform ", 8) == 0;
        int is_attr = strncmp(line, "attribute ", 10) == 0;
        if (!is_uniform && !is_attr)
            continue;

        char word[3][64] = {"", "", ""};
        int got = sscanf(line + (is_uniform ? 8 : 10), "%63s %63s %63s", word[0], word[1], word[2]);
        int w = 0;
        if (strcmp(word[0], "lowp") == 0 || strcmp(word[0], "mediump") == 0 || strcmp(word[0], "highp") == 0)
            w = 1;
        if (got < w + 2)
            continue;
        int t = find_type(word[w]);
        if (t < 0)
            continue;
        char *name = word[w + 1];
        char *semi = strchr(name, ';');
        if (semi)
            *semi = '\0';
        int count = 1;
        char *bracket = strchr(name, '[');
        if (bracket) {
            count = atoi(bracket + 1);
            *bracket = '\0';
        }

        if (is_attr) {
            int seen = 0;
            for (int i = 0; i < p->attribute_count; i++)
                seen |= strcmp(p->attributes[i].name, name) == 0;
            if (!seen && p->attribute_count < MAX_ATTRIBS) {
                struct attribute *a = &p->attributes[p->attribute_count];
                snprintf(a->name, sizeof a->name, "%s", name);
                a->type = g_types[t].gl;
                a->location = p->attribute_count++;
                a->hw_location = -1;
            }
            continue;
        }
        int seen = 0;
        for (int i = 0; i < p->uniform_count; i++)
            seen |= strcmp(p->uniforms[i].name, name) == 0;
        if (!seen && p->uniform_count < MAX_UNIFORMS) {
            struct uniform *u = &p->uniforms[p->uniform_count++];
            snprintf(u->name, sizeof u->name, "%s", name);
            u->type = g_types[t].gl;
            u->count = count;
            u->size = g_types[t].floats;
            u->columns = g_types[t].columns;
            u->binding = -1;
        }
    }
}

// Applies the pack's layout: attribute locations, sampler bindings and std140 offsets.
static void bind_record(struct program *p, const struct pak_record *r)
{
    const uint8_t *cursor = (const uint8_t *)(r + 1);
    struct named {
        char name[44];
        uint32_t value;
    };
    for (int i = 0; i < r->attribute_count; i++, cursor += sizeof(struct named)) {
        const struct named *n = (const struct named *)(const void *)cursor;
        for (int a = 0; a < p->attribute_count; a++)
            if (strcmp(p->attributes[a].name, n->name) == 0)
                p->attributes[a].hw_location = (int)n->value;
    }
    p->sampler_count = 0;
    for (int i = 0; i < r->sampler_count; i++, cursor += sizeof(struct named)) {
        const struct named *n = (const struct named *)(const void *)cursor;
        for (int u = 0; u < p->uniform_count; u++)
            if (strcmp(p->uniforms[u].name, n->name) == 0)
                p->uniforms[u].binding = (int)n->value;
        if ((int)n->value >= p->sampler_count)
            p->sampler_count = (int)n->value + 1;
    }
    for (int i = 0; i < r->uniform_count; i++, cursor += sizeof(struct named)) {
        const struct named *n = (const struct named *)(const void *)cursor;
        int stage = (int)(n->value & 0xFFFF), offset = (int)(n->value >> 16);
        for (int u = 0; u < p->uniform_count; u++) {
            const struct uniform *un = &p->uniforms[u];
            if (strcmp(un->name, n->name) != 0)
                continue;
            if (p->copy_count[stage] == MAX_COPIES) {
                fprintf(stderr, "gles: program has too many uniforms\n");
                break;
            }
            struct block_copy *c = &p->copies[stage][p->copy_count[stage]++];
            c->dst = (uint16_t)offset;
            c->src = (uint16_t)un->offset;
            c->rows = (uint16_t)(un->count * un->columns);
            c->comps = (uint16_t)(un->size / un->columns);
        }
    }
    p->block_size[0] = r->vs_block;
    p->block_size[1] = r->fs_block;
    p->blocks[0] = r->vs_block ? calloc(1, r->vs_block) : NULL;
    p->blocks[1] = r->fs_block ? calloc(1, r->fs_block) : NULL;
}

void shim_glLinkProgram(uint32_t id)
{
    struct program *p = g.programs[id];
    if (!p)
        return;
    p->uniform_count = p->attribute_count = 0;
    p->copy_count[0] = p->copy_count[1] = 0;
    free_program_data(p);
    const char *vs = g.shaders[p->shaders[0]].source, *fs = g.shaders[p->shaders[1]].source;
    for (int i = 0; i < 2; i++)
        if (p->shaders[i] && g.shaders[p->shaders[i]].source)
            scan_declarations(p, g.shaders[p->shaders[i]].source);

    for (int i = 0; i < p->attribute_count; i++)
        for (int b = 0; b < p->bound_count; b++)
            if (strcmp(p->bound[b].name, p->attributes[i].name) == 0)
                p->attributes[i].location = p->bound[b].location;

    int total = 0;
    for (int i = 0; i < p->uniform_count; i++) {
        p->uniforms[i].offset = total;
        total += p->uniforms[i].size * p->uniforms[i].count;
    }
    p->values = calloc((size_t)total + 1, sizeof(float));
    p->dirty = 1;

    p->backend = NULL;
    if (vs && fs) {
        uint64_t key = pak_key(vs, fs);
        const struct pak_record *r = pak_find(key);
        if (!r) {
            g.missing++;
            gb_shader_missing(key, vs, fs);
            // Unity builds these per material, so not all can be known in advance.  An
            // approximation beats a hole: one texture times vertex colour.
            // Only when the program feeds what the fallback reads: run 19 died (no CPU fault,
            // so the GPU) on the campaign's first frames with a texture * constant-colour program
            // that has no _glesColor attribute.  Without one, the draw is skipped.
            if (strstr(fs, "u_sampler0") && strstr(vs, "_glesColor") && strstr(vs, "_glesMultiTexCoord0"))
                r = pak_find(PAK_FALLBACK_KEY);
        }
        if (r) {
            bind_record(p, r);
            p->backend = gb_program_create(g.pak, r);
        }
    }
}

void shim_glGetProgramiv(uint32_t id, uint32_t name, int *v)
{
    struct program *p = g.programs[id];
    switch (name) {
    case GL_LINK_STATUS: *v = 1; break;
    case GL_ATTACHED_SHADERS: *v = 2; break;
    case GL_ACTIVE_UNIFORMS: *v = p ? p->uniform_count : 0; break;
    case GL_ACTIVE_ATTRIBUTES: *v = p ? p->attribute_count : 0; break;
    case GL_ACTIVE_UNIFORM_MAX_LENGTH:
    case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH: *v = 64; break;
    default: *v = 0; break;
    }
}

void shim_glGetProgramInfoLog(uint32_t id, int size, int *length, char *log)
{
    (void)id;
    if (length)
        *length = 0;
    if (size > 0)
        log[0] = '\0';
}

void shim_glGetActiveUniform(uint32_t id, uint32_t index, int size, int *length, int *count, uint32_t *type,
                             char *name)
{
    const struct uniform *u = &g.programs[id]->uniforms[index];
    int n = snprintf(name, (size_t)size, "%s", u->name);
    if (length)
        *length = n;
    *count = u->count;
    *type = u->type;
}

void shim_glGetActiveAttrib(uint32_t id, uint32_t index, int size, int *length, int *count, uint32_t *type,
                            char *name)
{
    const struct attribute *a = &g.programs[id]->attributes[index];
    int n = snprintf(name, (size_t)size, "%s", a->name);
    if (length)
        *length = n;
    *count = 1;
    *type = a->type;
}

int shim_glGetAttribLocation(uint32_t id, const char *name)
{
    struct program *p = g.programs[id];
    for (int i = 0; p && i < p->attribute_count; i++)
        if (strcmp(p->attributes[i].name, name) == 0)
            return p->attributes[i].location;
    return -1;
}

int shim_glGetUniformLocation(uint32_t id, const char *name)
{
    struct program *p = g.programs[id];
    if (!p)
        return -1;
    char base[64];
    snprintf(base, sizeof base, "%s", name);
    char *bracket = strchr(base, '[');
    int element = 0;
    if (bracket) {
        element = atoi(bracket + 1);
        *bracket = '\0';
    }
    for (int i = 0; i < p->uniform_count; i++)
        if (strcmp(p->uniforms[i].name, base) == 0)
            return i | (element << 8);
    return -1;
}

void shim_glUseProgram(uint32_t id) { g.current_program = id; }

// Locations carry the array element in their upper bits.
static void set_uniform(int location, int count, const void *v, int floats)
{
    struct program *p = g.programs[g.current_program];
    if (location < 0 || !p)
        return;
    const struct uniform *u = &p->uniforms[location & 0xFF];
    int element = location >> 8;
    if (element >= u->count)
        return;
    if (count > u->count - element)
        count = u->count - element;
    float *out = p->values + u->offset + u->size * element;
    size_t bytes = (size_t)count * (size_t)floats * sizeof(float);
    if (memcmp(out, v, bytes) != 0) {
        memcpy(out, v, bytes);
        p->dirty = 1;
    }
}

void shim_glUniform1fv(int l, int c, const float *v) { set_uniform(l, c, v, 1); }
void shim_glUniform2fv(int l, int c, const float *v) { set_uniform(l, c, v, 2); }
void shim_glUniform3fv(int l, int c, const float *v) { set_uniform(l, c, v, 3); }
void shim_glUniform4fv(int l, int c, const float *v) { set_uniform(l, c, v, 4); }
void shim_glUniform1iv(int l, int c, const int *v) { set_uniform(l, c, v, 1); }
void shim_glUniform2iv(int l, int c, const int *v) { set_uniform(l, c, v, 2); }
void shim_glUniform3iv(int l, int c, const int *v) { set_uniform(l, c, v, 3); }
void shim_glUniform4iv(int l, int c, const int *v) { set_uniform(l, c, v, 4); }
void shim_glUniform1i(int l, int v) { set_uniform(l, 1, &v, 1); }

void shim_glUniformMatrix3fv(int l, int c, uint8_t t, const float *v)
{
    (void)t;
    set_uniform(l, c, v, 9);
}

void shim_glUniformMatrix4fv(int l, int c, uint8_t t, const float *v)
{
    (void)t;
    set_uniform(l, c, v, 16);
}

// ---- vertex state -----------------------------------------------------------

void shim_glEnableVertexAttribArray(uint32_t i) { g.attribs[i].enabled = 1; }
void shim_glDisableVertexAttribArray(uint32_t i) { g.attribs[i].enabled = 0; }

void shim_glVertexAttribPointer(uint32_t i, int size, uint32_t type, uint8_t normalized, int stride, const void *p)
{
    struct vertex_attrib *a = &g.attribs[i];
    a->size = size;
    a->type = type;
    a->normalized = normalized;
    a->stride = stride;
    a->pointer = p;
    a->buffer = g.array_buffer;
}

void shim_glGetVertexAttribiv(uint32_t i, uint32_t name, int *v)
{
    *v = name == 0x8622 ? g.attribs[i].enabled : 0; // VERTEX_ATTRIB_ARRAY_ENABLED
}

void shim_glGetVertexAttribPointerv(uint32_t i, uint32_t name, void **v)
{
    (void)name;
    *v = (void *)g.attribs[i].pointer;
}

// ---- targets ----------------------------------------------------------------

static void apply_target(void)
{
    if (!g.target_dirty)
        return;
    g.target_dirty = 0;
    if (!g.framebuffer) {
        gb_bind_target(NULL, NULL, NULL);
        return;
    }
    const struct framebuffer *f = &g.framebuffers[g.framebuffer];
    gb_bind_target(g.textures[f->color], g.textures[f->depth], g.textures[f->stencil]);
}

// ---- drawing ----------------------------------------------------------------

static void update_blocks(struct program *p)
{
    if (!p->dirty)
        return;
    p->dirty = 0;
    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < p->copy_count[s]; i++) {
            const struct block_copy *c = &p->copies[s][i];
            const float *src = p->values + c->src;
            uint8_t *dst = p->blocks[s] + c->dst;
            for (int r = 0; r < c->rows; r++, src += c->comps, dst += 16)
                memcpy(dst, src, (size_t)c->comps * sizeof(float));
        }
    }
}

// Diagnostics for the grey sky and ground seen on hardware: for each program with fog, once,
// its fog uniforms at its first draw.  All-zero fog parameters would turn a fog-patched
// program's whole output into the fog colour.
static void check_program(uint32_t id, const struct program *p, const struct gb_draw *d)
{
    static int programs;
    for (int i = 0; i < p->uniform_count; i++) {
        const struct uniform *u = &p->uniforms[i];
        if (!strstr(u->name, "FogParams") && !strstr(u->name, "FogColor"))
            continue;
        const float *v = &p->values[u->offset];
        fprintf(stderr, "gles: program %u %s = %g %g %g %g\n", (unsigned)id, u->name, v[0], v[1], v[2], v[3]);
    }
    int missing = 0;
    for (int i = 0; i < d->texture_count; i++)
        missing += !d->textures[i];
    if (missing)
        fprintf(stderr, "gles: program %u draws with %d of %d textures unbound\n", (unsigned)id, missing,
                d->texture_count);
    if (++programs == 400)
        fprintf(stderr, "gles: first-draw checks stop at 400 programs\n");
}

static void draw(uint32_t mode, int first, int count, uint32_t type, const void *indices)
{
    g.draws++;
    struct program *p = g.programs[g.current_program];
    if (!p || !p->backend || count <= 0)
        return;
    apply_target();
    update_blocks(p);

    struct gb_draw d;
    memset(&d, 0, sizeof d);
    d.program = p->backend;
    d.state = &g.state;
    d.vs_block = p->blocks[0];
    d.fs_block = p->blocks[1];
    d.vs_block_size = p->block_size[0];
    d.fs_block_size = p->block_size[1];
    for (int i = 0; i < p->attribute_count; i++) {
        const struct attribute *a = &p->attributes[i];
        if (a->hw_location < 0 || a->location < 0 || a->location >= MAX_ATTRIBS)
            continue;
        const struct vertex_attrib *v = &g.attribs[a->location];
        struct gb_attrib *o = &d.attribs[a->hw_location];
        o->enabled = v->enabled;
        o->components = v->size;
        o->normalized = v->normalized;
        o->stride = v->stride;
        o->type = v->type;
        o->buffer = v->buffer ? &g.buffers[v->buffer] : NULL;
        o->pointer = v->pointer;
        if (a->hw_location >= d.attrib_count)
            d.attrib_count = a->hw_location + 1;
    }
    for (int i = 0; i < p->uniform_count; i++) {
        const struct uniform *u = &p->uniforms[i];
        if (u->binding < 0 || u->binding >= GB_MAX_SAMPLERS)
            continue;
        int unit;
        memcpy(&unit, &p->values[u->offset], sizeof unit);
        if (unit < 0 || unit >= 32)
            continue;
        uint32_t id = u->type == 0x8B60 ? g.bound_cube[unit] : g.bound_2d[unit];
        d.textures[u->binding] = g.textures[id];
        if (u->binding >= d.texture_count)
            d.texture_count = u->binding + 1;
    }
    d.mode = mode;
    d.first = first;
    d.count = count;
    d.index_type = type;
    d.index_buffer = type && g.element_buffer ? &g.buffers[g.element_buffer] : NULL;
    d.indices = indices;
    if (!p->checked) {
        static int checks;
        p->checked = 1;
        if (checks++ < 400)
            check_program(g.current_program, p, &d);
    }
    gb_draw(&d);
}

void shim_glDrawArrays(uint32_t mode, int first, int count) { draw(mode, first, count, 0, NULL); }

void shim_glDrawElements(uint32_t mode, int count, uint32_t type, const void *indices)
{
    draw(mode, 0, count, type, indices);
}

// ---- textures ---------------------------------------------------------------

void shim_glActiveTexture(uint32_t unit)
{
    g.active_texture = (unit - 0x84C0) & 31;
}

void shim_glBindTexture(uint32_t target, uint32_t id)
{
    if (target == 0x8513)
        g.bound_cube[g.active_texture] = id;
    else
        g.bound_2d[g.active_texture] = id;
    struct gb_texture *t = texture_object(id);
    if (t && target == 0x8513)
        t->cube = 1;
}

static struct gb_texture *active_object(uint32_t target)
{
    int cube = target == 0x8513 || (target >= 0x8515 && target <= 0x851A);
    uint32_t id = cube ? g.bound_cube[g.active_texture] : g.bound_2d[g.active_texture];
    return texture_object(id);
}

static int face_of(uint32_t target)
{
    return target >= 0x8515 && target <= 0x851A ? (int)(target - 0x8515) : 0;
}

void shim_glTexParameteri(uint32_t target, uint32_t name, int value)
{
    struct gb_texture *t = active_object(target);
    if (!t)
        return;
    switch (name) {
    case 0x2800: t->mag_filter = (uint32_t)value; break;
    case 0x2801: t->min_filter = (uint32_t)value; break;
    case 0x2802: t->wrap_s = (uint32_t)value; break;
    case 0x2803: t->wrap_t = (uint32_t)value; break;
    case 0x84FE: t->anisotropy = (float)value; break;
    default: break;
    }
}

static int g_pack_alignment = 4;

void shim_glPixelStorei(uint32_t name, int value)
{
    if (name == 0x0CF5) // UNPACK_ALIGNMENT
        g.unpack_alignment = value;
    else if (name == 0x0D05) // PACK_ALIGNMENT, for glReadPixels
        g_pack_alignment = value;
}

// What a client format/type pair becomes, and its size per pixel in client memory.
static enum gb_format client_format(uint32_t format, uint32_t type, int *bytes)
{
    switch (type) {
    case 0x8033: *bytes = 2; return GB_RGBA4;
    case 0x8034: *bytes = 2; return GB_RGB5A1;
    case 0x8363: *bytes = 2; return GB_RGB565;
    default: break;
    }
    switch (format) {
    case 0x1908: *bytes = 4; return GB_RGBA8;
    case 0x80E1: *bytes = 4; return GB_BGRA8;
    case 0x1907: *bytes = 3; return GB_RGBA8; // no 24-bit format: expanded below
    case 0x1906: *bytes = 1; return GB_A8;
    case 0x1909: *bytes = 1; return GB_L8;
    case 0x190A: *bytes = 2; return GB_LA8;
    // Depth textures, which Unity attaches to its render textures.  Taken as RGBA8 they were bound
    // as a depth buffer with a colour format: the GPU queue errored two frames later (the campaign
    // opening's image effect, the sniper scope; runs 18-26).
    case 0x1902: *bytes = type == 0x1403 ? 2 : 4; return type == 0x1403 ? GB_D16 : GB_D24S8; // DEPTH_COMPONENT
    case 0x84F9: *bytes = 4; return GB_D24S8;                                               // DEPTH_STENCIL_OES
    default:
        fprintf(stderr, "gles: unknown texture format 0x%x type 0x%x\n", (unsigned)format, (unsigned)type);
        *bytes = 4;
        return GB_RGBA8;
    }
}

// Tight rows at the destination's size.  Returns NULL when `pixels` can be used as it is.
static void *to_upload(const void *pixels, int w, int h, uint32_t format, int bytes, uint32_t *size)
{
    int alignment = g.unpack_alignment > 0 ? g.unpack_alignment : 4;
    size_t stride = ((size_t)w * (size_t)bytes + (size_t)alignment - 1) & ~((size_t)alignment - 1);
    int rgb = format == 0x1907 && bytes == 3;
    size_t out_bytes = rgb ? 4 : (size_t)bytes;
    *size = (uint32_t)((size_t)w * (size_t)h * out_bytes);
    if (!rgb && stride == (size_t)w * (size_t)bytes)
        return NULL;
    uint8_t *out = malloc(*size);
    if (!out)
        return NULL;
    const uint8_t *src = pixels;
    uint8_t *dst = out;
    for (int y = 0; y < h; y++, src += stride) {
        if (rgb) {
            for (int x = 0; x < w; x++, dst += 4) {
                dst[0] = src[x * 3];
                dst[1] = src[x * 3 + 1];
                dst[2] = src[x * 3 + 2];
                dst[3] = 255;
            }
        } else {
            memcpy(dst, src, (size_t)w * (size_t)bytes);
            dst += (size_t)w * (size_t)bytes;
        }
    }
    return out;
}

void shim_glTexImage2D(uint32_t target, int level, int internal, int w, int h, int border, uint32_t format,
                       uint32_t type, const void *pixels)
{
    (void)internal;
    (void)border;
    struct gb_texture *t = active_object(target);
    if (!t)
        return;
    int bytes;
    enum gb_format f = client_format(format, type, &bytes);
    uint32_t size = 0;
    void *converted = pixels ? to_upload(pixels, w, h, format, bytes, &size) : NULL;
    gb_tex_data(t, face_of(target), level, w, h, f, converted ? converted : pixels, size);
    free(converted);
    g.target_dirty = 1;
}

void shim_glTexSubImage2D(uint32_t target, int level, int x, int y, int w, int h, uint32_t format, uint32_t type,
                          const void *pixels)
{
    struct gb_texture *t = active_object(target);
    if (!t || !pixels)
        return;
    int bytes;
    client_format(format, type, &bytes);
    uint32_t size;
    void *converted = to_upload(pixels, w, h, format, bytes, &size);
    gb_tex_sub(t, face_of(target), level, x, y, w, h, converted ? converted : pixels, size);
    free(converted);
}

static enum gb_format compressed_format(uint32_t format)
{
    switch (format) {
    case 0x8D64: return GB_ETC1;
    case 0x83F0: return GB_BC1;
    case 0x83F1: return GB_BC1A;
    case 0x83F2: return GB_BC2;
    case 0x83F3: return GB_BC3;
    default:
        fprintf(stderr, "gles: unknown compressed format 0x%x\n", (unsigned)format);
        return GB_NONE;
    }
}

void shim_glCompressedTexImage2D(uint32_t target, int level, uint32_t format, int w, int h, int border, int size,
                                 const void *data)
{
    (void)border;
    struct gb_texture *t = active_object(target);
    enum gb_format f = compressed_format(format);
    if (t && f != GB_NONE)
        gb_tex_data(t, face_of(target), level, w, h, f, data, (uint32_t)size);
}

void shim_glCompressedTexSubImage2D(uint32_t target, int level, int x, int y, int w, int h, uint32_t format,
                                    int size, const void *data)
{
    (void)format;
    struct gb_texture *t = active_object(target);
    if (t)
        gb_tex_sub(t, face_of(target), level, x, y, w, h, data, (uint32_t)size);
}

void shim_glCopyTexSubImage2D(uint32_t target, int level, int xoff, int yoff, int x, int y, int w, int h)
{
    struct gb_texture *t = active_object(target);
    if (t) {
        apply_target();
        gb_copy_framebuffer(t, face_of(target), level, xoff, yoff, x, y, w, h);
    }
}

void shim_glGenerateMipmap(uint32_t target)
{
    struct gb_texture *t = active_object(target);
    if (t)
        gb_tex_mipmap(t);
}

// ---- framebuffer objects ----------------------------------------------------

void shim_glBindFramebuffer(uint32_t t, uint32_t id)
{
    (void)t;
    if (g.framebuffer != id) {
        g.framebuffer = id;
        g.target_dirty = 1;
    }
}

void shim_glBindRenderbuffer(uint32_t t, uint32_t id)
{
    (void)t;
    g.renderbuffer = id;
}

#define ATTACH_COLOR 0x8CE0
#define ATTACH_DEPTH 0x8D00
#define ATTACH_STENCIL 0x8D20

static void attach(uint32_t attachment, uint32_t id)
{
    struct framebuffer *f = &g.framebuffers[g.framebuffer];
    if (attachment == ATTACH_COLOR)
        f->color = id;
    else if (attachment == ATTACH_DEPTH)
        f->depth = id;
    else if (attachment == ATTACH_STENCIL)
        f->stencil = id;
    g.target_dirty = 1;
}

void shim_glFramebufferTexture2D(uint32_t t, uint32_t a, uint32_t tt, uint32_t id, int l)
{
    (void)t;
    (void)tt;
    (void)l;
    attach(a, id);
}

void shim_glFramebufferRenderbuffer(uint32_t t, uint32_t a, uint32_t rt, uint32_t id)
{
    (void)t;
    (void)rt;
    attach(a, id);
}

void shim_glRenderbufferStorage(uint32_t t, uint32_t format, int w, int h)
{
    (void)t;
    struct gb_texture *b = texture_object(g.renderbuffer);
    if (!b)
        return;
    enum gb_format f;
    switch (format) {
    case 0x81A5: f = GB_D16; break;   // DEPTH_COMPONENT16
    case 0x81A6:                      // DEPTH_COMPONENT24
    case 0x88F0: f = GB_D24S8; break; // DEPTH24_STENCIL8
    case 0x8D48: f = GB_S8; break;    // STENCIL_INDEX8
    default: f = GB_RGBA8; break;
    }
    gb_tex_data(b, 0, 0, w, h, f, NULL, 0);
    g.target_dirty = 1;
}

uint32_t shim_glCheckFramebufferStatus(uint32_t t)
{
    (void)t;
    return GL_FRAMEBUFFER_COMPLETE;
}

void shim_glGetFramebufferAttachmentParameteriv(uint32_t t, uint32_t a, uint32_t n, int *v)
{
    (void)t; (void)a; (void)n;
    *v = 0;
}

// Fills the caller's buffer with zeros (nothing is read back yet), writing exactly the bytes GL
// defines for this format, type and GL_PACK_ALIGNMENT.  It used to write w*h*4 whatever the
// format: an RGB, alpha or 16-bit read then zeroed up to w*h bytes past Unity's buffer.
void shim_glReadPixels(int x, int y, int w, int h, uint32_t f, uint32_t t, void *p)
{
    (void)x; (void)y;
    if (!p || w <= 0 || h <= 0)
        return;
    int bpp;
    if (t == 0x8363 || t == 0x8033 || t == 0x8034) // 565, 4444, 5551: one 16-bit word
        bpp = 2;
    else if (f == 0x1908) // RGBA
        bpp = 4;
    else if (f == 0x1907) // RGB
        bpp = 3;
    else if (f == 0x190A) // LUMINANCE_ALPHA
        bpp = 2;
    else // ALPHA, LUMINANCE
        bpp = 1;
    if (t == 0x1406) // FLOAT
        bpp *= 4;
    size_t align = g_pack_alignment == 1 || g_pack_alignment == 2 || g_pack_alignment == 8 ? (size_t)g_pack_alignment : 4;
    size_t row = (size_t)w * (size_t)bpp;
    size_t stride = (row + align - 1) & ~(align - 1);
    static uint32_t seen[8];
    uint32_t key = f << 16 ^ t;
    for (int i = 0; i < 8; i++) {
        if (seen[i] == key)
            break;
        if (!seen[i]) {
            seen[i] = key;
            fprintf(stderr, "gles: glReadPixels %dx%d format 0x%x type 0x%x, %d bytes a pixel\n", w, h, (unsigned)f,
                    (unsigned)t, bpp);
            break;
        }
    }
    memset(p, 0, stride * (size_t)(h - 1) + row);
}

// ---- state ------------------------------------------------------------------

void gles_state_init(void)
{
    static int done;
    if (done++)
        return; // Unity can make the context current again, and GL state survives that
    struct gb_state *s = &g.state;
    memset(s, 0, sizeof *s);
    s->color_mask = 15;
    s->depth_write = 1;
    s->blend_op_rgb = s->blend_op_alpha = 0x8006; // FUNC_ADD
    s->blend_src_rgb = s->blend_src_alpha = 1;    // ONE
    s->blend_dst_rgb = s->blend_dst_alpha = 0;    // ZERO
    s->depth_func = 0x0201;                       // LESS
    s->cull_face = 0x0405;                        // BACK
    s->front_face = 0x0901;                       // CCW
    for (int i = 0; i < 2; i++) {
        s->stencil_func[i] = 0x0207; // ALWAYS
        s->stencil_read_mask[i] = s->stencil_write_mask[i] = 0xFF;
        s->stencil_fail[i] = s->stencil_zfail[i] = s->stencil_zpass[i] = 0x1E00; // KEEP
    }
    s->viewport[2] = s->scissor[2] = 1280;
    s->viewport[3] = s->scissor[3] = 720;
    g.clear_depth = 1.0f;
    g.unpack_alignment = 4;
    g.target_dirty = 1;
}

void shim_glClear(uint32_t mask)
{
    apply_target();
    gb_clear(&g.state, (mask & 0x4000) != 0, (mask & 0x100) != 0, (mask & 0x400) != 0, g.clear_color, g.clear_depth,
             g.clear_stencil);
}

void shim_glClearColor(float r, float g_, float b, float a)
{
    g.clear_color[0] = r;
    g.clear_color[1] = g_;
    g.clear_color[2] = b;
    g.clear_color[3] = a;
}

void shim_glClearDepthf(float d) { g.clear_depth = d; }
void shim_glClearStencil(int s) { g.clear_stencil = s; }

void shim_glColorMask(uint8_t r, uint8_t g_, uint8_t b, uint8_t a)
{
    g.state.color_mask = (uint8_t)((r ? 1 : 0) | (g_ ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0));
}

static void set_capability(uint32_t c, uint8_t on)
{
    switch (c) {
    case 0x0BE2: g.state.blend = on; break;
    case 0x0B71: g.state.depth_test = on; break;
    case 0x0B44: g.state.cull = on; break;
    case 0x0C11: g.state.scissor_test = on; break;
    case 0x0B90: g.state.stencil_test = on; break;
    default: break;
    }
}

void shim_glEnable(uint32_t c) { set_capability(c, 1); }
void shim_glDisable(uint32_t c) { set_capability(c, 0); }

void shim_glBlendEquationSeparate(uint32_t a, uint32_t b)
{
    g.state.blend_op_rgb = a;
    g.state.blend_op_alpha = b;
}

void shim_glBlendFuncSeparate(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    g.state.blend_src_rgb = a;
    g.state.blend_dst_rgb = b;
    g.state.blend_src_alpha = c;
    g.state.blend_dst_alpha = d;
}

void shim_glDepthFunc(uint32_t f) { g.state.depth_func = f; }
void shim_glDepthMask(uint8_t m) { g.state.depth_write = m ? 1 : 0; }
void shim_glCullFace(uint32_t m) { g.state.cull_face = m; }
void shim_glFrontFace(uint32_t m) { g.state.front_face = m; }

void shim_glViewport(int x, int y, int w, int h)
{
    int *v = g.state.viewport;
#ifdef PORT_QEMU
    if (v[2] != w || v[3] != h)
        fprintf(stderr, "viewport %d,%d %dx%d fb=%u draws=%llu\n", x, y, w, h, (unsigned)g.framebuffer,
                (unsigned long long)g.draws);
#endif
    v[0] = x;
    v[1] = y;
    v[2] = w;
    v[3] = h;
}

void shim_glScissor(int x, int y, int w, int h)
{
    int *v = g.state.scissor;
    v[0] = x;
    v[1] = y;
    v[2] = w;
    v[3] = h;
}

#define GL_FRONT 0x0404
#define GL_BACK 0x0405

void shim_glStencilFuncSeparate(uint32_t s, uint32_t f, int r, uint32_t m)
{
    for (int i = 0; i < 2; i++)
        if (s == 0x0408 || s == (i ? GL_BACK : GL_FRONT)) {
            g.state.stencil_func[i] = f;
            g.state.stencil_ref[i] = (uint32_t)r;
            g.state.stencil_read_mask[i] = m;
        }
}

void shim_glStencilFunc(uint32_t f, int r, uint32_t m) { shim_glStencilFuncSeparate(0x0408, f, r, m); }

void shim_glStencilMask(uint32_t m) { g.state.stencil_write_mask[0] = g.state.stencil_write_mask[1] = m; }

void shim_glStencilOpSeparate(uint32_t s, uint32_t a, uint32_t b, uint32_t c)
{
    for (int i = 0; i < 2; i++)
        if (s == 0x0408 || s == (i ? GL_BACK : GL_FRONT)) {
            g.state.stencil_fail[i] = a;
            g.state.stencil_zfail[i] = b;
            g.state.stencil_zpass[i] = c;
        }
}

void shim_glStencilOp(uint32_t a, uint32_t b, uint32_t c) { shim_glStencilOpSeparate(0x0408, a, b, c); }

void shim_glFlush(void) {}
void shim_glFinish(void) {}

uint64_t gles_draw_count(void) { return g.draws; }
uint64_t gles_frame_count(void) { return g.frames; }
uint64_t gles_missing_program_count(void) { return g.missing; }
