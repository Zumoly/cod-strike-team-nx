// Draw capture for diagnosing missing geometry, shared by the console renderer.  Included once,
// after the file defines CAP_PAK (the loaded shaders.pak) and CAP_RECORD(draw) (its pak_record).
//
// Off unless sd:/switch/cod_st/capture exists.  Then frames 1800, 3600, ... (every 30 s at 60 fps)
// are written, up to 6, to sd:/switch/cod_st/capture_<n>.txt: each draw's state,
// textures and attributes, with its vertices run through glstate_matrix_mvp and the game's
// _DepthBand remap on the CPU, counting what the GPU would clip.
#define CAP_FLAG "sdmc:/switch/cod_st/capture"
static FILE *g_cap;
static int g_cap_enabled = -1, g_cap_taken;
static uint64_t g_cap_seen_frame, g_cap_last, g_cap_draws_at, g_cap_prev_draws;

static int capture_on(void)
{
    if (g_cap_enabled < 0)
        g_cap_enabled = access(CAP_FLAG, F_OK) == 0;
    if (!g_cap_enabled)
        return 0;
    uint64_t now = gles_frame_count();
    if (now != g_cap_seen_frame) { // first call of a new frame
        uint64_t draws = gles_draw_count();
        g_cap_prev_draws = draws - g_cap_draws_at;
        g_cap_draws_at = draws;
        g_cap_seen_frame = now;
        if (g_cap) {
            fclose(g_cap);
            g_cap = NULL;
            fprintf(stderr, "capture: frame %llu written\n", (unsigned long long)g_cap_last);
        }
        // Mission frames have only about 80 draws, so frames are picked by time: every 30 s at 60 fps.
        if (g_cap_taken < 6 && now >= 1800u * (uint64_t)(g_cap_taken + 1)) {
            char path[64];
            snprintf(path, sizeof path, "sdmc:/switch/cod_st/capture_%d.txt", g_cap_taken);
            g_cap = fopen(path, "w");
            if (g_cap) {
                g_cap_taken++;
                g_cap_last = now;
                fprintf(g_cap, "frame %llu, previous frame had %llu draws\n", (unsigned long long)now,
                        (unsigned long long)g_cap_prev_draws);
            }
        }
    }
    return g_cap != NULL;
}

struct cap_named {
    char name[44];
    uint32_t value;
};

static uint64_t record_key(const struct pak_record *r)
{
    if (!CAP_PAK)
        return 0;
    uint32_t n;
    memcpy(&n, CAP_PAK + 4, 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off;
        memcpy(&off, CAP_PAK + 16 + i * 16 + 8, 4);
        if (CAP_PAK + off == (const uint8_t *)r) {
            uint64_t k;
            memcpy(&k, CAP_PAK + 16 + i * 16, 8);
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

// Test switch: with sd:/switch/cod_st/no_fog present, the game's own fog (_FogRange, used by the
// Corona shaders as clamp(clip.z * x + y + 1, z, 1)) is forced to "no fog" on every draw.
static void fog_override(const struct gb_draw *d)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = access("sdmc:/switch/cod_st/no_fog", F_OK) == 0;
    if (!enabled)
        return;
    const struct pak_record *r = CAP_RECORD(d);
    for (int stage = 0; stage < 2; stage++) {
        int o = uniform_offset(r, "_FogRange", stage);
        uint32_t size = stage ? d->fs_block_size : d->vs_block_size;
        uint8_t *block = (uint8_t *)(stage ? d->fs_block : d->vs_block);
        if (o >= 0 && (uint32_t)o + 16 <= size) {
            static const float none[4] = {0.0f, 0.0f, 1.0f, 0.0f};
            memcpy(block + o, none, sizeof none);
        }
    }
}

static void capture_draw(const struct gb_draw *d)
{
    const struct pak_record *r = CAP_RECORD(d);
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
    // Every uniform, as four floats from its block offset (matrices show their first column).
    const struct cap_named *un = (const struct cap_named *)(const void *)(r + 1);
    un += r->attribute_count + r->sampler_count;
    for (int i = 0; i < r->uniform_count; i++) {
        int stage = (int)(un[i].value & 0xFFFF), o = (int)(un[i].value >> 16);
        uint32_t size = stage ? d->fs_block_size : d->vs_block_size;
        const uint8_t *block = stage ? d->fs_block : d->vs_block;
        if ((uint32_t)o + 16 <= size) {
            const float *v = (const float *)(const void *)(block + o);
            fprintf(g_cap, "  u %s[%s] = %g %g %g %g\n", un[i].name, stage ? "fs" : "vs", v[0], v[1], v[2], v[3]);
        }
    }
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
