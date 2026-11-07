// Console startup: the same sequence tests/qemu/main.c proves under emulation.
#ifdef __SWITCH__

#include "port/android_input.h"
#include "port/bionic_thread.h"
#include "port/cpu_clock.h"

extern int port_log_fd;
volatile int port_frame;
b_pthread_t port_main_thread;

#define LOG_BUFFER_SIZE 0x8000
#define LOG_FLUSH_NS 2000000000ll
#define LOG_FLUSH_PRIORITY 0x2C
void watchdog_start(void);
#include <unistd.h>
#include "port/audio.h"
#include "port/fake_jni.h"
#include "port/gles_backend.h"
#include "port/renderer.h"
#include "port/so_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <switch.h>
#include <unistd.h>

#define LIB_DIR "sdmc:/switch/cod_st/lib"

void b_libc_init(void);

static void sync_log(FILE *log)
{
    fflush(log);
    fsync(fileno(log));
}

static void flush_log(void *arg)
{
    (void)arg;
    for (;;) {
        svcSleepThread(LOG_FLUSH_NS);
        fflush(stderr);
        fsync(port_log_fd);
    }
}

// Until now every line reached the card at once.  A write to the card takes long enough to cost the
// game frames, so from here the log waits in memory and a thread of its own writes it out.
static void log_flusher_start(void)
{
    static char buffer[LOG_BUFFER_SIZE];
    static Thread thread;
    setvbuf(stderr, buffer, _IOFBF, sizeof buffer);
    if (R_SUCCEEDED(threadCreate(&thread, flush_log, NULL, NULL, 0x4000, LOG_FLUSH_PRIORITY, -2)))
        threadStart(&thread);
}

static so_module *load(const char *name)
{
    char path[128];
    snprintf(path, sizeof path, LIB_DIR "/%s", name);
    so_module *m = so_load(name, path);
    if (m && so_relocate(m) == 0) {
        so_patch_kuser(m);
        return m;
    }
    return NULL;
}

#define STEP(...)                 \
    do {                          \
        fprintf(log, __VA_ARGS__); \
        fputc('\n', log);         \
        sync_log(log);            \
    } while (0)

// Stands in for com.unity3d.player.UnityPlayer: the entry points come from RegisterNatives.
// Mono runs in full-AOT mode because Horizon gives it no writable executable memory, and
// the precompiled code tests for null itself, so it needs no fault delivery either.
int game_run_unity(FILE *log, int frames)
{
    so_module *unity = so_find("libunity.so");
    so_module *mono = so_find("libmono.so");
    if (!unity || !mono)
        return -1;

    void (*set_aot_only)(int) = (void (*)(int))so_symbol(mono, "mono_jit_set_aot_only");
    setenv("MONO_DEBUG", "explicit-null-checks", 1);
    set_aot_only(1);

    jni_init();
    void *env = jni_env();
    int (*jni_onload)(void *, void *) = (int (*)(void *, void *))so_symbol(unity, "JNI_OnLoad");
    STEP("unity: JNI_OnLoad returned 0x%x", jni_onload(jni_vm(), NULL));

    void *player = jni_object("com/unity3d/player/UnityPlayer");
    void (*native_file)(void *, void *, void *) = jni_native("nativeFile");
    void (*init_jni)(void *, void *, void *) = jni_native("initJni");
    void (*resize)(void *, void *, int, int, int, int) = jni_native("nativeResize");
    void (*gfx)(void *, void *, void *) = jni_native("nativeRecreateGfxState");
    void (*focus)(void *, void *, int) = jni_native("nativeFocusChanged");
    void (*resume)(void *, void *) = jni_native("nativeResume");
    int (*render)(void *, void *) = (int (*)(void *, void *))jni_native("nativeRender");

    native_file(env, player, jni_string(jni_package_code_path()));
    native_file(env, player, jni_string(jni_obb_path()));
    init_jni(env, player, jni_object("android/app/Activity"));
    resize(env, player, RENDERER_WIDTH, RENDERER_HEIGHT, RENDERER_WIDTH, RENDERER_HEIGHT);
    gfx(env, player, jni_object("android/view/Surface"));
    focus(env, player, 1);
    resume(env, player);
    STEP("unity: started");
    log_flusher_start();
    port_main_thread = b_pthread_self();
    cpu_clock_start();
    watchdog_start();
    STEP("audio: start %d", audio_start());

    for (int frame = 0; (frames <= 0 || frame < frames) && appletMainLoop(); frame++) {
        port_frame = frame;
        input_poll();
        if (jni_take_resize_request())
            resize(env, player, RENDERER_WIDTH, RENDERER_HEIGHT, RENDERER_WIDTH, RENDERER_HEIGHT);
        render(env, player);
        if (frame < 5 || frame % 120 == 0)
            STEP("unity: frame %d, draws=%llu swaps=%llu", frame, (unsigned long long)gles_draw_count(),
                 (unsigned long long)gles_frame_count());
    }
    cpu_clock_stop();
    return 0;
}

// Loads and initializes the libraries.  Does not start Unity yet.
int game_load(FILE *log)
{
    b_libc_init();
    so_set_lib_dir(LIB_DIR);

    so_module *libmain = load("libmain.so");
    so_module *mono = load("libmono.so");
    so_module *unity = load("libunity.so");
    if (!libmain || !mono || !unity) {
        fprintf(log, "game_load: library failed to load\n");
        return -1;
    }
    if (so_patch_mono(mono) != 0) {
        fprintf(log, "game_load: libmono patch refused\n");
        return -1;
    }
    fprintf(log, "game_load: unresolved imports %u\n", so_unresolved_count());
    sync_log(log);
    so_module *mono_module = mono;

    if (so_finalize(libmain) || so_finalize(mono) || so_finalize(unity)) {
        fprintf(log, "game_load: finalize failed\n");
        return -1;
    }
    fprintf(log, "game_load: segments protected\n");
    sync_log(log);
    so_run_init(libmain);
    fprintf(log, "game_load: libmain initializers ran\n");
    sync_log(log);
    so_run_init(mono);
    fprintf(log, "game_load: libmono initializers ran\n");
    sync_log(log);
    so_run_init(unity);
    fprintf(log, "game_load: libunity initializers ran\n");
    sync_log(log);
    if (b_gc_bridge_init(mono_module) != 0) {
        fprintf(log, "game_load: GC bridge refused this libmono\n");
        return -1;
    }
    return 0;
}

#endif
