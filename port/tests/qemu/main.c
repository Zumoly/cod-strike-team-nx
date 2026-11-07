// Loads the real game libraries on emulated ARM, the same sequence as the console,
// then boots Mono on the game's own mscorlib and runs JIT-compiled managed code.
#include "port/pad_dialog.h"
#include "port/profiler.h"
#include "port/android_input.h"
#include "port/audio.h"
#include "port/bionic_thread.h"
#include "port/fake_jni.h"
#include "port/gles_backend.h"
#include "port/so_loader.h"
#include "port/vfs.h"

#include <unistd.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOT "host:/sd"
#define MANAGED VFS_GUEST_DATA "/managed"

void b_libc_init(void);
void qemu_pause_init(void);
extern int qemu_refuse_exec;

#define STEP(...)                     \
    do {                              \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr);          \
    } while (0)

static so_module *load(const char *name)
{
    char path[128];
    snprintf(path, sizeof path, ROOT "/lib/%s", name);
    so_module *m = so_load(name, path);
    if (!m) {
        STEP("FAIL load %s", name);
        return NULL;
    }
    if (so_relocate(m) != 0) {
        STEP("FAIL relocate %s", name);
        return NULL;
    }
    so_patch_kuser(m);
    STEP("loaded %-20s base=%p init_array=%zu unresolved_refs=%u", name, (void *)m->base, m->init_count,
         m->unresolved);
    return m;
}

#define SYM(ret, name, ...) ret (*name)(__VA_ARGS__) = (ret(*)(__VA_ARGS__))so_symbol(mono, #name)

// Host-side tool mode: the game's own libmono compiling an assembly ahead of time.
// /sd/aot_job names the DLL.  Nothing in this path ships.
static int run_aot_job(so_module *mono)
{
    FILE *job = fopen(ROOT "/aot_job", "r");
    if (!job)
        return -1;
    char name[64] = "";
    fscanf(job, "%63s", name);
    fclose(job);

    SYM(void, mono_set_dirs, const char *, const char *);
    SYM(void, mono_set_assemblies_path, const char *);
    SYM(int, mono_main, int, char **);

    // Collection needs the suspend bridge, and a compile job can simply not collect.
    setenv("GC_DONT_GC", "1", 1);
    // Compiled in, so the output tests for null itself instead of relying on SIGSEGV.
    setenv("MONO_DEBUG", "explicit-null-checks", 1);
    mono_set_dirs(MANAGED, MANAGED);
    mono_set_assemblies_path(MANAGED);

    char dll[192], opts[256];
    snprintf(dll, sizeof dll, MANAGED "/%s", name);
    // Full AOT cannot create trampolines at runtime, so the tables are sized up front.
    snprintf(opts, sizeof opts,
             "--aot=full,asmonly,nodebug,ntrampolines=8192,nrgctx-trampolines=8192,nimt-trampolines=2048,"
             "outfile=" MANAGED "/%s.s",
             name);
    char *argv[] = {(char *)"mono", opts, dll, NULL};
    STEP("aot: compiling %s", name);
    int r = mono_main(3, argv);
    STEP("aot: mono_main returned %d", r);
    return r;
}

// Reports where the render thread is when frames stop arriving.
static volatile int g_frame;
static b_pthread_t g_render_thread;
void b_thread_pause(b_pthread_t t, uint32_t regs[16]);
void b_thread_unpause(b_pthread_t t);
void b_sleep_ns(int64_t ns);
void qemu_gl_report(void);

static void *stall_watch(void *unused)
{
    (void)unused;
    int seen = -1;
    for (;;) {
        b_sleep_ns(25000000000ll);
        if (g_frame != seen) {
            seen = g_frame;
            fprintf(stderr, "watch: frame %d, heap in use %u MB\n", seen, (unsigned)(mallinfo().uordblks >> 20));
            continue;
        }
        uint32_t regs[16];
        char pc[96], lr[96];
        b_thread_pause(g_render_thread, regs);
        fprintf(stderr, "watch: STALLED at frame %d pc=%s lr=%s sp=%08x heap %u MB\n", seen,
                so_describe(regs[15], pc, sizeof pc), so_describe(regs[14], lr, sizeof lr), (unsigned)regs[13],
                (unsigned)(mallinfo().uordblks >> 20));
        const uint32_t *stack = (const uint32_t *)(uintptr_t)(regs[13] & ~3u);
        int shown = 0;
        for (int i = 0; i < 1024 && shown < 16; i++) {
            char where[96];
            so_describe(stack[i], where, sizeof where);
            if (strchr(where, '+') && (stack[i] & 2) == 0) {
                fprintf(stderr, "watch:   %s\n", where);
                shown++;
            }
        }
        b_thread_unpause(g_render_thread);
        qemu_gl_report();
        b_pthread_t self = b_pthread_self();
        for (uint32_t id = 0; id < 64; id++) {
            b_pthread_t t = b_thread_by_id(id);
            if (!t || t == self || t == g_render_thread)
                continue;
            uint32_t r[16];
            b_thread_pause(t, r);
            char a[96], b[96];
            fprintf(stderr, "watch: thread %u pc=%s lr=%s\n", (unsigned)id, so_describe(r[15], a, sizeof a),
                    so_describe(r[14], b, sizeof b));
            const uint32_t *s = (const uint32_t *)(uintptr_t)(r[13] & ~3u);
            int n = 0;
            for (int i = 0; i < 512 && n < 8; i++) {
                char w[96];
                so_describe(s[i], w, sizeof w);
                if (strchr(w, '+') && (s[i] & 2) == 0) {
                    fprintf(stderr, "watch:     %s\n", w);
                    n++;
                }
            }
            b_thread_unpause(t);
        }
    }
    return NULL;
}

// Asks Unity's Input class and the game's own Controller what they believe about a button or axis.
static void *(*g_invoke)(void *, void *, void **, void **);
static void *(*g_string)(const char *);
static void *(*g_unbox)(void *);
static void *(*g_field)(void *, const char *);
static int (*g_field_offset)(void *);
static void *(*g_field_class)(void *);
static void *(*g_field_type)(void *);
static void *(*g_type_class)(void *);
static void *(*g_object_class)(void *);
static void *g_get_key, *g_get_key_down, *g_get_key_up, *g_get_axis, *g_moga_connected, *g_get_state;
#define OBJECT_HEADER 8

static void probe_init(so_module *mono)
{
    void *(*image_loaded)(const char *) = (void *(*)(const char *))so_symbol(mono, "mono_image_loaded");
    void *(*class_from_name)(void *, const char *, const char *) =
        (void *(*)(void *, const char *, const char *))so_symbol(mono, "mono_class_from_name");
    void *(*method_from_name)(void *, const char *, int) =
        (void *(*)(void *, const char *, int))so_symbol(mono, "mono_class_get_method_from_name");
    g_invoke = (void *(*)(void *, void *, void **, void **))so_symbol(mono, "mono_runtime_invoke");
    g_string = (void *(*)(const char *))so_symbol(mono, "mono_string_new_wrapper");
    g_unbox = (void *(*)(void *))so_symbol(mono, "mono_object_unbox");
    g_field = (void *(*)(void *, const char *))so_symbol(mono, "mono_class_get_field_from_name");
    g_field_offset = (int (*)(void *))so_symbol(mono, "mono_field_get_offset");
    g_field_type = (void *(*)(void *))so_symbol(mono, "mono_field_get_type");
    g_type_class = (void *(*)(void *))so_symbol(mono, "mono_class_from_mono_type");
    g_object_class = (void *(*)(void *))so_symbol(mono, "mono_object_get_class");
    void *input = class_from_name(image_loaded("UnityEngine"), "UnityEngine", "Input");
    g_get_key = method_from_name(input, "GetKeyInt", 1);
    g_get_key_down = method_from_name(input, "GetKeyDownInt", 1);
    g_get_key_up = method_from_name(input, "GetKeyUpInt", 1);
    g_get_axis = method_from_name(input, "GetAxis", 1);
    void *game = image_loaded("Assembly-CSharp");
    void *moga = game ? class_from_name(game, "", "Moga_Input") : NULL;
    void *controller = game ? class_from_name(game, "", "Controller") : NULL;
    g_moga_connected = moga ? method_from_name(moga, "GetControllerConnected", 0) : NULL;
    g_get_state = controller ? method_from_name(controller, "GetState", 0) : NULL;
    STEP("probe: key %p down %p up %p axis %p moga %p state %p", g_get_key, g_get_key_down, g_get_key_up, g_get_axis,
         g_moga_connected, g_get_state);
}

static int key_query(void *method, int key)
{
    void *exception = NULL, *args[1] = {&key};
    void *r = g_invoke(method, NULL, args, &exception);
    return r ? *(unsigned char *)g_unbox(r) : -1;
}

// Reads a Button field (up, down, pressed) out of the game's boxed Controller state.
static void state_button(void *boxed, const char *name, int out[3])
{
    static const char *const parts[3] = {"up", "down", "pressed"};
    void *cls = g_object_class(boxed);
    void *f = g_field(cls, name);
    out[0] = out[1] = out[2] = -1;
    if (!f)
        return;
    void *inner = g_type_class(g_field_type(f));
    const unsigned char *base = (const unsigned char *)boxed + g_field_offset(f);
    for (int i = 0; i < 3; i++) {
        void *p = g_field(inner, parts[i]);
        if (p)
            out[i] = base[g_field_offset(p) - OBJECT_HEADER];
    }
}

static void probe_input(int frame)
{
    if (!g_get_key || !g_get_axis)
        return;
    void *exception = NULL;
    float axis[6];
    static const char *const names[6] = {"AndAxis1", "AndAxis2", "AndAxis3", "AndAxis4", "AndAxis5", "AndAxis6"};
    for (int i = 0; i < 6; i++) {
        void *name[1] = {g_string(names[i])};
        void *r = g_invoke(g_get_axis, NULL, name, &exception);
        axis[i] = -9;
        if (r)
            memcpy(&axis[i], g_unbox(r), 4);
    }
    int live = -1, y[3] = {-1, -1, -1}, x[3] = {-1, -1, -1}, rs[3] = {-1, -1, -1}, rt[3] = {-1, -1, -1};
    int connected = g_moga_connected ? *(unsigned char *)g_unbox(g_invoke(g_moga_connected, NULL, NULL, &exception)) : -1;
    void *state = g_get_state ? g_invoke(g_get_state, NULL, NULL, &exception) : NULL;
    if (state) {
        void *c = g_field(g_object_class(state), "connected");
        live = c ? ((unsigned char *)state)[g_field_offset(c)] : -2;
        state_button(state, "y", y);
        state_button(state, "x", x);
        state_button(state, "rightShoulder", rs);
        state_button(state, "rightTrigger", rt);
    }
    STEP("probe: f%d key333 held=%d down=%d up=%d | axes %.2f %.2f %.2f %.2f %.2f %.2f | conn=%d live=%d y=%d%d%d x=%d%d%d rs=%d%d%d rt=%d%d%d",
         frame, key_query(g_get_key, 333), key_query(g_get_key_down, 333), key_query(g_get_key_up, 333),
         (double)axis[0], (double)axis[1], (double)axis[2], (double)axis[3], (double)axis[4], (double)axis[5], connected, live,
         y[0], y[1], y[2], x[0], x[1], x[2], rs[0], rs[1], rs[2], rt[0], rt[1], rt[2]);
}

// Called on a frame's first draw: Unity's one-frame flags can be gone once the frame has ended.
extern void (*qemu_draw_hook)(void);
static int g_probed = -1;
static int g_frame_now;
// With a level to load, the scripted presses wait until it is playing.
#define LEVEL_SCRIPT_SHIFT 1000
static int g_script_shift;

static void mid_frame(void)
{
    if (g_probed == g_frame_now)
        return;
    g_probed = g_frame_now;
    probe_input(g_frame_now);
}

// Takes samples of the render thread once the level is playing.
#define SAMPLE_NS 20000000ll

static void put_error(const char *line) { fputs(line, stderr); }

static void *sampler(void *arg)
{
    (void)arg;
    for (;;) {
        b_sleep_ns(SAMPLE_NS);
        profiler_sample(g_render_thread);
    }
    return NULL;
}

// Opens the two-button box the game asks before quitting, as the purchase box is opened.
static void open_dialog(so_module *mono)
{
    void *(*image_loaded)(const char *) = (void *(*)(const char *))so_symbol(mono, "mono_image_loaded");
    void *(*class_from_name)(void *, const char *, const char *) =
        (void *(*)(void *, const char *, const char *))so_symbol(mono, "mono_class_from_name");
    void *(*method_from_name)(void *, const char *, int) =
        (void *(*)(void *, const char *, int))so_symbol(mono, "mono_class_get_method_from_name");
    void *controller = class_from_name(image_loaded("Assembly-CSharp"), "", "MessageBoxController");
    void *get = method_from_name(controller, "get_Instance", 0);
    void *open = method_from_name(controller, "DoConfirmQuitDialogue", 3);
    void *exception = NULL, *args[3] = {NULL, NULL, NULL};
    void *instance = g_invoke(get, NULL, NULL, &exception);
    STEP("dialog: controller %p instance %p open %p", controller, instance, open);
    if (instance && open)
        g_invoke(open, instance, args, &exception);
    STEP("dialog: opened, exception %p", exception);
}

// A scripted player: sticks and triggers, Cross, the D-pad, and a tap.
static void script_input(int frame)
{
    static const float idle[AXIS_COUNT] = {0, 0, 0, 0, 0, 0};
    static const float held[AXIS_COUNT] = {0.5f, -0.5f, 0.25f, 0.25f, 1.0f, 0.0f};
    static const float trigger[AXIS_COUNT] = {0, 0, 0, 0, 0, 1.0f};
    int id = 0;
    float x = 100.0f, y = 100.0f;
    switch (frame) {
    case 20: input_joystick(idle); break;
    case 60: input_joystick(held); break;
    case 70: input_joystick(idle); break;
    case 80: input_key(96, 1); break;
    case 85: input_key(96, 0); break;
    case 90: input_key(19, 1); break;
    case 95: input_key(19, 0); break;
    case 100: input_key(66, 1); break;
    case 105: input_key(66, 0); break;
    case 300: input_key(100, 1); break;                // Triangle tapped
    case 303: input_key(100, 0); break;
    case 310: input_key(103, 1); break;                // R1 tapped
    case 313: input_key(103, 0); break;
    case 320: input_joystick(trigger); break;          // R2 fully in
    case 323: input_joystick(idle); break;
    case 330: case 332: case 334: case 336: case 338: case 340: {
        float one[AXIS_COUNT] = {0, 0, 0, 0, 0, 0};    // each axis alone, to see where it lands
        one[(frame - 330) / 2] = 1.0f;
        input_joystick(one);
        break;
    }
    case 342: input_joystick(idle); break;
    case 110: input_touch(0, 1, &id, &x, &y); break;
    case 112: x = 110.0f; input_touch(2, 1, &id, &x, &y); break;
    case 114: input_touch(1, 1, &id, &x, &y); break;
    default: break;
    }
}

// Stands in for com.unity3d.player.UnityPlayer: the slots below come from RegisterNatives.
static int boot_unity(so_module *unity)
{
    so_module *mono = so_find("libmono.so");
    SYM(void, mono_jit_set_aot_only, int);
    qemu_refuse_exec = 1;
    setenv("MONO_DEBUG", "explicit-null-checks", 1);
    mono_jit_set_aot_only(1);

    jni_init();
    void *env = jni_env();
    int (*jni_onload)(void *, void *) = (int (*)(void *, void *))so_symbol(unity, "JNI_OnLoad");
    STEP("unity: JNI_OnLoad");
    int version = jni_onload(jni_vm(), NULL);
    STEP("unity: JNI_OnLoad returned 0x%x", version);

    void *player = jni_object("com/unity3d/player/UnityPlayer");
    void *activity = jni_object("android/app/Activity");

    void (*native_file)(void *, void *, void *) = jni_native("nativeFile");
    void (*init_jni)(void *, void *, void *) = jni_native("initJni");
    void (*resize)(void *, void *, int, int, int, int) = jni_native("nativeResize");
    void (*gfx)(void *, void *, void *) = jni_native("nativeRecreateGfxState");
    void (*focus)(void *, void *, int) = jni_native("nativeFocusChanged");
    void (*resume)(void *, void *) = jni_native("nativeResume");
    int (*render)(void *, void *) = (int (*)(void *, void *))jni_native("nativeRender");

    native_file(env, player, jni_string(jni_package_code_path()));
    native_file(env, player, jni_string(jni_obb_path()));
    STEP("unity: files registered");
    init_jni(env, player, activity);
    STEP("unity: initJni done");
    resize(env, player, 1280, 720, 1280, 720);
    gfx(env, player, jni_object("android/view/Surface"));
    STEP("unity: surface set");
    focus(env, player, 1);
    resume(env, player);
    STEP("unity: resumed, rendering");
    STEP("audio: start %d", audio_start());
    g_render_thread = b_pthread_self();
    b_pthread_t watcher, sampling;
    b_pthread_create(&watcher, NULL, stall_watch, NULL);
    void *(*image_loaded)(const char *) = (void *(*)(const char *))so_symbol(mono, "mono_image_loaded");
    void *(*class_from_name)(void *, const char *, const char *) =
        (void *(*)(void *, const char *, const char *))so_symbol(mono, "mono_class_from_name");
    void *(*method_from_name)(void *, const char *, int) =
        (void *(*)(void *, const char *, int))so_symbol(mono, "mono_class_get_method_from_name");
    void *(*runtime_invoke)(void *, void *, void **, void **) =
        (void *(*)(void *, void *, void **, void **))so_symbol(mono, "mono_runtime_invoke");
    void *(*string_new)(const char *) = (void *(*)(const char *))so_symbol(mono, "mono_string_new_wrapper");
    char level[64] = "";
    FILE *wanted = fopen(ROOT "/load_level", "r");
    if (wanted) {
        fscanf(wanted, "%63s", level);
        fclose(wanted);
        g_script_shift = LEVEL_SCRIPT_SHIFT;
    }

    for (int frame = 0; frame < 1500; frame++) {
        if (frame == 250 && level[0]) {
            void *application = class_from_name(image_loaded("UnityEngine"), "UnityEngine", "Application");
            void *load = method_from_name(application, "LoadLevelAsync", 4);
            int index = -1;
            unsigned char additive = 0, complete = 1;
            void *args[4] = {string_new(level), &index, &additive, &complete};
            void *exception = NULL;
            STEP("harness: loading level %s, method %p", level, load);
            if (load)
                runtime_invoke(load, NULL, args, &exception);
            STEP("harness: load call returned, exception %p", exception);
        }
        g_frame = frame;
        g_frame_now = frame;
        if (g_script_shift && frame == g_script_shift)
            b_pthread_create(&sampling, NULL, sampler, NULL);
        script_input(frame >= g_script_shift ? frame - g_script_shift : 0);
        if (g_script_shift) { // a box from the game itself may be up first, so clear that
            int at = frame - g_script_shift;
            if (at == 350)
                STEP("dialog: right stick on what was there gave %d", pad_dialog_press(1));
            if (at == 400 || at == 450)
                open_dialog(mono);
            if (at == 420)
                STEP("dialog: left stick gave %d, expected 2", pad_dialog_press(0));
            if (at == 425 || at == 480)
                STEP("dialog: stick with the box closing gave %d", pad_dialog_press(1));
            if (at == 470)
                STEP("dialog: right stick gave %d, expected 1", pad_dialog_press(1));
        }
        if (frame == 296 + g_script_shift)
            probe_init(mono);
        qemu_draw_hook = frame - g_script_shift >= 298 && frame - g_script_shift < 346 ? mid_frame : NULL;
        g_probed = -1;
        if (jni_take_resize_request())
            resize(env, player, 1280, 720, 1280, 720);
        int r = render(env, player);
        if (frame < 5 || frame % 50 == 0)
            STEP("unity: nativeRender %d returned %d, draws=%llu swaps=%llu", frame, r,
                 (unsigned long long)gles_draw_count(), (unsigned long long)gles_frame_count());
    }
    profiler_report(put_error);
    STEP("unity: %llu programs were not in the pack", (unsigned long long)gles_missing_program_count());
    qemu_gl_report();
    return 0;
}

static int boot_mono(so_module *mono)
{
    SYM(void, mono_set_dirs, const char *, const char *);
    SYM(void, mono_set_assemblies_path, const char *);
    SYM(void *, mono_jit_init_version, const char *, const char *);
    SYM(void *, mono_get_corlib, void);
    SYM(void *, mono_class_from_name, void *, const char *, const char *);
    SYM(void *, mono_class_get_method_from_name, void *, const char *, int);
    SYM(void *, mono_string_new_wrapper, const char *);
    SYM(void *, mono_runtime_invoke, void *, void *, void **, void **);
    SYM(void, mono_gc_collect, int);
    SYM(void *, mono_domain_assembly_open, void *, const char *);
    SYM(void *, mono_assembly_get_image, void *);

    SYM(void, mono_jit_set_aot_only, int);
    SYM(unsigned, mono_gchandle_new, void *, int);
    SYM(void *, mono_gchandle_get_target, unsigned);
    SYM(char *, mono_string_to_utf8, void *);

    FILE *marker = fopen(ROOT "/aot_only", "r");
    int aot_only = marker != NULL;
    if (marker)
        fclose(marker);
    if (aot_only) {
        // The console's rules: no executable mappings, so no JIT can be hiding in here.
        qemu_refuse_exec = 1;
        setenv("MONO_DEBUG", "explicit-null-checks", 1);
        mono_jit_set_aot_only(1);
    }

    mono_set_dirs(MANAGED, MANAGED);
    mono_set_assemblies_path(MANAGED);
    STEP("mono: starting %s", aot_only ? "in full-AOT mode, executable memory refused" : "JIT");
    void *domain = mono_jit_init_version("Unity Root Domain", "v2.0.50727");
    STEP("mono: domain %p", domain);
    if (!domain)
        return -1;

    void *corlib = mono_get_corlib();
    void *int32_class = mono_class_from_name(corlib, "System", "Int32");
    void *parse = mono_class_get_method_from_name(int32_class, "Parse", 1);
    STEP("mono: corlib=%p Int32=%p Int32.Parse=%p", corlib, int32_class, parse);
    if (!parse)
        return -1;

    // Compiling and running this needs the JIT, trampolines and the runtime together.
    void *args[1] = {mono_string_new_wrapper("1234")};
    void *exc = NULL;
    void *boxed = mono_runtime_invoke(parse, NULL, args, &exc);
    if (exc || !boxed) {
        STEP("FAIL Int32.Parse raised exception %p", exc);
        return -1;
    }
    int value = *(int *)((char *)boxed + 8); // past vtable and sync words
    STEP("mono: managed Int32.Parse(\"1234\") = %d", value);
    if (value != 1234)
        return -1;

    // Thrown by managed code: proves exception unwinding, with no fault involved.
    void *bad[1] = {NULL};
    exc = NULL;
    void *result = mono_runtime_invoke(parse, NULL, bad, &exc);
    STEP("mono: Int32.Parse(null) returned %p, exception %p (want an exception)", result, exc);
    if (!exc)
        return -1;

    if (!aot_only) {
        // Unsafe managed code with no checks: a null destination faults inside compiled
        // code, and Mono must turn that SIGSEGV into NullReferenceException.
        void *string_class = mono_class_from_name(corlib, "System", "String");
        void *char_copy = mono_class_get_method_from_name(string_class, "CharCopy", 3);
        STEP("mono: String.CharCopy=%p", char_copy);
        if (!char_copy)
            return -1;
        uint16_t source[8] = {'a', 'b', 'c', 'd'};
        void *source_ptr = source;
        int count = 4;
        // NULL itself, not a pointer to NULL: whether the invoke wrapper uses this slot as
        // the value or dereferences it, the access lands on address 0 in generated code.
        void *fault_args[3] = {NULL, &source_ptr, &count};
        exc = NULL;
        result = mono_runtime_invoke(char_copy, NULL, fault_args, &exc);
        STEP("mono: write through null returned %p, exception %p (want an exception)", result, exc);
        if (!exc)
            return -1;
    }

    void *game = mono_domain_assembly_open(domain, MANAGED "/Assembly-CSharp.dll");
    void *image = game ? mono_assembly_get_image(game) : NULL;
    STEP("mono: Assembly-CSharp loaded, image %p", image);
    if (!image)
        return -1;

    // Stops and restarts every managed thread.  The handle proves collecting did not
    // lose live data, and the churn forces collections beyond the explicit ones.
    unsigned keep = mono_gchandle_new(mono_string_new_wrapper("survivor"), 0);
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 20000; i++)
            mono_string_new_wrapper("garbage to be collected");
        mono_gc_collect(0);
    }
    char *text = mono_string_to_utf8(mono_gchandle_get_target(keep));
    STEP("mono: 5 collections finished, held object = \"%s\"", text);
    if (strcmp(text, "survivor") != 0)
        return -1;
    return 0;
}

int main(void)
{
    b_thread_init();
    b_libc_init();
    int r = vfs_init(ROOT);
    if (r) {
        STEP("FAIL vfs_init %d", r);
        return 1;
    }
    so_set_lib_dir(ROOT "/lib");

    // Dependency order: libunity needs symbols from both of these.
    so_module *libmain = load("libmain.so");
    so_module *mono = load("libmono.so");
    so_module *unity = load("libunity.so");
    so_module *bedrock = load("libbedrock.so");
    if (!libmain || !mono || !unity || !bedrock)
        return 1;
    if (so_patch_mono(mono) != 0)
        return 1;
    qemu_pause_init();

    unsigned missing = 0;
    for (unsigned i = 0; i < so_unresolved_count(); i++) {
        const char *n = so_unresolved_name(i);
        if (n[0] == 'A' && n[1] >= 'A' && n[1] <= 'Z')
            missing++;
        else
            STEP("  missing %s", n);
    }
    STEP("unimplemented imports: %u (%u are Android input/sensor APIs)", so_unresolved_count(), missing);

    if (so_finalize(libmain) || so_finalize(mono) || so_finalize(unity) || so_finalize(bedrock)) {
        STEP("FAIL finalize");
        return 1;
    }
    so_run_init(libmain);
    so_run_init(mono);
    so_run_init(unity);
    so_run_init(bedrock);
    if (b_gc_bridge_init(mono) != 0)
        return 1;
    STEP("LOADER OK");

    int aot = run_aot_job(mono);
    if (aot != -1)
        return aot;

    FILE *unity_marker = fopen(ROOT "/unity_boot", "r");
    if (unity_marker) {
        fclose(unity_marker);
        return boot_unity(unity);
    }

    if (boot_mono(mono) != 0)
        return 1;
    STEP("MONO OK");
    return 0;
}
