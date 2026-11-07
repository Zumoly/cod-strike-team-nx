// Entry point.  Currently a bring-up shell: proves the AArch32 process starts, the SD
// card mounts and deko3D presents, then records what the kernel allows and loads the
// game's libraries.  The Unity startup sequence attaches here in later work.
#include <stdio.h>
#include <switch.h>
#include <unistd.h>

#include "port/bionic_thread.h"
#include "port/gl_shim.h"
#include "port/renderer.h"
#include "port/vfs.h"

void probe_run(const char *path);
void probe_late(void);
int game_load(FILE *log);
int game_run_unity(FILE *log, int frames);
extern int port_log_fd;

// fflush alone can leave the file empty if the process dies afterwards.
static void sync_log(FILE *log)
{
    if (log) {
        fflush(log);
        fsync(fileno(log));
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    b_thread_init();
    FILE *log = fopen("sdmc:/switch/cod_st/boot.log", "w");
    if (log) {
        extern volatile uint32_t __dcr_reloc_path __attribute__((visibility("hidden")));
        fprintf(log, "main reached, relocation path %u (1 alias, 2 emulator)\n", (unsigned)__dcr_reloc_path);
        s32 priority = -1;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        fprintf(log, "main thread priority %d (want 59)\n", (int)priority);
        sync_log(log);
    }

    // The libraries report through stderr, which libnx otherwise discards.
    freopen("sdmc:/switch/cod_st/game.log", "w", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);
    port_log_fd = fileno(stderr);

    int vfs = vfs_init("sdmc:/switch/cod_st");
    if (log) {
        fprintf(log, "vfs_init: %d, gl imports: %u\n", vfs, gl_import_count);
        sync_log(log);
    }

    int gfx = renderer_init();
    if (log) {
        fprintf(log, "renderer_init: %d\n", gfx);
        sync_log(log);
    }
    if (gfx == 0) {
        // One black frame, so the screen is defined while the game loads (it was a second of green,
        // the first proof on hardware that graphics worked).
        renderer_present_clear(0.0f, 0.0f, 0.0f);
        if (log) {
            fprintf(log, "first frame presented\n");
            sync_log(log);
        }
    }

    // After the green screen, so a probe crash cannot hide the graphics result.
    probe_run("sdmc:/switch/cod_st/probe.log");

    if (log) {
        int loaded = game_load(log);
        fprintf(log, "game_load: %d\n", loaded);
        sync_log(log);
        if (loaded == 0) {
            fprintf(log, "game_run_unity: %d\n", game_run_unity(log, 0));
            sync_log(log);
        }
    }

    // Last: either of these can end the process.
    probe_late();

    if (log)
        fclose(log);
    fsdevUnmountAll();
    return 0;
}
