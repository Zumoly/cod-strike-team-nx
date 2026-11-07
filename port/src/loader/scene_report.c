// Scene report, for the missing ground and sky.  Every draw the game makes reaches the renderer, so
// Unity leaves those objects out itself; this asks Unity why.  Off unless sd:/switch/cod_st/scene_report
// exists.  Then every 30 s (up to 4 times) it writes sd:/switch/cod_st/scene_<n>.txt: each camera
// (culling mask, clip planes, clear flags) and each renderer (active, enabled, Unity's isVisible,
// layer, lightmap index, bounds, mesh size, materials and their shaders).
// Runs on the main thread from the pad poll, like pad_dialog.c.
#include "port/so_loader.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FLAG "sdmc:/switch/cod_st/scene_report"
#define ARRAY_LENGTH_WORD 3 // MonoArray: vtable, sync, bounds, max_length
#define ARRAY_DATA_WORD 4
#define INTERVAL_POLLS 1800 // the pad is polled once a frame: 30 s at 60 fps
#define MAX_REPORTS 4

typedef void *(*method_fn)(void *, const char *, int);
static void *(*m_invoke)(void *, void *, void **, void **);
static void *(*m_unbox)(void *);
static char *(*m_utf8)(void *);
static void (*m_free)(void *);
static void *(*m_type_object)(void *, void *);
static void *(*m_class_type)(void *);
static void *(*m_domain)(void);
static void *(*m_object_class)(void *);
static void *(*m_class_name)(void *);

static void *c_renderer, *c_meshfilter, *c_skinned;
static void *f_find, *f_name, *f_all_cameras;
static void *r_enabled, *r_visible, *r_material, *r_materials, *r_bounds, *r_lightmap, *r_gameobject;
static void *r_get_component;
static void *g_layer, *g_active;
static void *mat_shader, *mf_mesh, *sk_mesh, *mesh_vertices, *mesh_submeshes;
static void *cam_mask, *cam_far, *cam_near, *cam_clear, *cam_depth, *cam_enabled, *cam_occlusion;

static int resolve(void)
{
    static int state; // 0 not tried, 1 ready, -1 failed
    if (state)
        return state > 0;
    so_module *mono = so_find("libmono.so");
    if (!mono)
        return 0;
    void *(*image_loaded)(const char *) = (void *(*)(const char *))so_symbol(mono, "mono_image_loaded");
    void *(*class_from_name)(void *, const char *, const char *) =
        (void *(*)(void *, const char *, const char *))so_symbol(mono, "mono_class_from_name");
    method_fn method = (method_fn)so_symbol(mono, "mono_class_get_method_from_name");
    void *engine = image_loaded("UnityEngine");
    if (!engine)
        return 0; // not loaded yet
    state = -1;
    m_invoke = (void *(*)(void *, void *, void **, void **))so_symbol(mono, "mono_runtime_invoke");
    m_unbox = (void *(*)(void *))so_symbol(mono, "mono_object_unbox");
    m_utf8 = (char *(*)(void *))so_symbol(mono, "mono_string_to_utf8");
    m_free = (void (*)(void *))so_symbol(mono, "mono_free");
    if (!m_free)
        m_free = (void (*)(void *))so_symbol(mono, "g_free");
    m_type_object = (void *(*)(void *, void *))so_symbol(mono, "mono_type_get_object");
    m_class_type = (void *(*)(void *))so_symbol(mono, "mono_class_get_type");
    m_domain = (void *(*)(void))so_symbol(mono, "mono_domain_get");
    m_object_class = (void *(*)(void *))so_symbol(mono, "mono_object_get_class");
    m_class_name = (void *(*)(void *))so_symbol(mono, "mono_class_get_name");

    void *object = class_from_name(engine, "UnityEngine", "Object");
    void *component = class_from_name(engine, "UnityEngine", "Component");
    void *gameobject = class_from_name(engine, "UnityEngine", "GameObject");
    void *material = class_from_name(engine, "UnityEngine", "Material");
    void *mesh = class_from_name(engine, "UnityEngine", "Mesh");
    void *camera = class_from_name(engine, "UnityEngine", "Camera");
    c_renderer = class_from_name(engine, "UnityEngine", "Renderer");
    c_meshfilter = class_from_name(engine, "UnityEngine", "MeshFilter");
    c_skinned = class_from_name(engine, "UnityEngine", "SkinnedMeshRenderer");
    if (!object || !component || !gameobject || !material || !mesh || !camera || !c_renderer || !c_meshfilter)
        return 0;

    f_find = method(object, "FindObjectsOfType", 1);
    f_name = method(object, "get_name", 0);
    f_all_cameras = method(camera, "get_allCameras", 0);
    r_enabled = method(c_renderer, "get_enabled", 0);
    r_visible = method(c_renderer, "get_isVisible", 0);
    r_material = method(c_renderer, "get_sharedMaterial", 0);
    r_materials = method(c_renderer, "get_sharedMaterials", 0);
    r_bounds = method(c_renderer, "get_bounds", 0);
    r_lightmap = method(c_renderer, "get_lightmapIndex", 0);
    r_gameobject = method(component, "get_gameObject", 0);
    r_get_component = method(component, "GetComponent", 1);
    g_layer = method(gameobject, "get_layer", 0);
    g_active = method(gameobject, "get_activeInHierarchy", 0);
    mat_shader = method(material, "get_shader", 0);
    mf_mesh = method(c_meshfilter, "get_sharedMesh", 0);
    sk_mesh = c_skinned ? method(c_skinned, "get_sharedMesh", 0) : NULL;
    mesh_vertices = method(mesh, "get_vertexCount", 0);
    mesh_submeshes = method(mesh, "get_subMeshCount", 0);
    cam_mask = method(camera, "get_cullingMask", 0);
    cam_far = method(camera, "get_farClipPlane", 0);
    cam_near = method(camera, "get_nearClipPlane", 0);
    cam_clear = method(camera, "get_clearFlags", 0);
    cam_depth = method(camera, "get_depth", 0);
    void *behaviour = class_from_name(engine, "UnityEngine", "Behaviour");
    cam_enabled = behaviour ? method(behaviour, "get_enabled", 0) : NULL;
    cam_occlusion = method(camera, "get_useOcclusionCulling", 0);
    if (!f_find || !f_name || !r_enabled || !r_gameobject || !g_layer || !m_unbox || !m_utf8)
        return 0;
    state = 1;
    return 1;
}

static void *call(void *method, void *self, void **args)
{
    if (!method || !self)
        return NULL;
    void *exception = NULL;
    void *r = m_invoke(method, self, args, &exception);
    return exception ? NULL : r;
}

static int call_bool(void *method, void *self, int fallback)
{
    void *r = call(method, self, NULL);
    return r ? *(uint8_t *)m_unbox(r) : fallback;
}

static int call_int(void *method, void *self, int fallback)
{
    void *r = call(method, self, NULL);
    return r ? *(int32_t *)m_unbox(r) : fallback;
}

static float call_float(void *method, void *self)
{
    void *r = call(method, self, NULL);
    return r ? *(float *)m_unbox(r) : -1.0f;
}

static void name_of(void *obj, char *out, size_t size)
{
    snprintf(out, size, "%s", obj ? "?" : "null");
    void *s = call(f_name, obj, NULL);
    if (!s)
        return;
    char *text = m_utf8(s);
    if (text) {
        snprintf(out, size, "%s", text);
        if (m_free)
            m_free(text);
    }
}

static void *type_of(void *klass) { return m_type_object(m_domain(), m_class_type(klass)); }

static void report(FILE *f)
{
    char name[96], shader[96];
    void *cams = NULL;
    if (f_all_cameras) {
        void *exception = NULL;
        cams = m_invoke(f_all_cameras, NULL, NULL, &exception);
    }
    int ncam = cams ? (int)((uint32_t *)cams)[ARRAY_LENGTH_WORD] : 0;
    for (int i = 0; i < ncam; i++) {
        void *cam = ((void **)cams)[ARRAY_DATA_WORD + i];
        name_of(cam, name, sizeof name);
        fprintf(f, "camera '%s' enabled=%d depth=%.1f mask=%08x near=%g far=%g clear=%d occlusion=%d\n", name,
                call_bool(cam_enabled, cam, -1), call_float(cam_depth, cam), (unsigned)call_int(cam_mask, cam, 0),
                call_float(cam_near, cam), call_float(cam_far, cam), call_int(cam_clear, cam, -1),
                call_bool(cam_occlusion, cam, -1));
    }

    void *exception = NULL;
    void *args[1] = {type_of(c_renderer)};
    void *list = m_invoke(f_find, NULL, args, &exception);
    int n = list && !exception ? (int)((uint32_t *)list)[ARRAY_LENGTH_WORD] : 0;
    fprintf(f, "renderers %d\n", n);
    void *mf_type = type_of(c_meshfilter);
    for (int i = 0; i < n; i++) {
        void *r = ((void **)list)[ARRAY_DATA_WORD + i];
        if (!r)
            continue;
        void *go = call(r_gameobject, r, NULL);
        name_of(go, name, sizeof name);
        const char *kind = m_class_name ? (const char *)m_class_name(m_object_class(r)) : "?";

        int vertices = -1, submeshes = -1;
        void *mesh = NULL;
        if (sk_mesh && c_skinned && strcmp(kind, "SkinnedMeshRenderer") == 0)
            mesh = call(sk_mesh, r, NULL);
        else {
            void *cargs[1] = {mf_type};
            void *mf = call(r_get_component, r, cargs);
            mesh = call(mf_mesh, mf, NULL);
        }
        if (mesh) {
            vertices = call_int(mesh_vertices, mesh, -1);
            submeshes = call_int(mesh_submeshes, mesh, -1);
        }

        float b[6] = {0, 0, 0, 0, 0, 0}; // center, extents
        void *boxed = call(r_bounds, r, NULL);
        if (boxed)
            memcpy(b, m_unbox(boxed), sizeof b);

        fprintf(f, "%s '%s' active=%d enabled=%d visible=%d layer=%d lightmap=%d verts=%d sub=%d "
                   "center=%.1f,%.1f,%.1f ext=%.1f,%.1f,%.1f",
                kind, name, call_bool(g_active, go, -1), call_bool(r_enabled, r, -1), call_bool(r_visible, r, -1),
                call_int(g_layer, go, -1), call_int(r_lightmap, r, -99), vertices, submeshes, b[0], b[1], b[2],
                b[3], b[4], b[5]);
        void *mats = call(r_materials, r, NULL);
        int nm = mats ? (int)((uint32_t *)mats)[ARRAY_LENGTH_WORD] : 0;
        fprintf(f, " mats=%d", nm);
        for (int m = 0; m < nm && m < 4; m++) {
            void *mat = ((void **)mats)[ARRAY_DATA_WORD + m];
            name_of(mat, name, sizeof name);
            name_of(call(mat_shader, mat, NULL), shader, sizeof shader);
            fprintf(f, " ['%s' %s]", name, shader);
        }
        fputc('\n', f);
    }
}

void scene_report_check(void)
{
    static int enabled = -1, polls, taken;
    if (enabled < 0)
        enabled = access(FLAG, F_OK) == 0;
    if (!enabled || taken >= MAX_REPORTS || ++polls % INTERVAL_POLLS != 0 || !resolve())
        return;
    char path[64];
    snprintf(path, sizeof path, "sdmc:/switch/cod_st/scene_%d.txt", taken++);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    report(f);
    fclose(f);
    fprintf(stderr, "scene report: %s written\n", path);
}
