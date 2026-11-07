// Unlocks every campaign mission section, for trying missions past the opening one.
// The game keeps a debug ActStructure.UnlockAll() that nothing calls; this calls it once, 30 s after
// the pad is first polled (the main menu), then SecureStorage.SaveActStructure() so it is kept.
// Only when sd:/switch/cod_st/unlock_missions exists.  Runs on the main thread from the pad poll.
#include "port/so_loader.h"

#include <stdio.h>
#include <unistd.h>

#define FLAG "sdmc:/switch/cod_st/unlock_missions"
#define WAIT_POLLS 1800 // 30 s at 60 fps

typedef void *(*method_fn)(void *, const char *, int);

static void *invoke(void *(*run)(void *, void *, void **, void **), void *method, void *self, const char *what)
{
    if (!method) {
        fprintf(stderr, "unlock: %s not found\n", what);
        return NULL;
    }
    void *exception = NULL;
    void *r = run(method, self, NULL, &exception);
    fprintf(stderr, "unlock: %s %s\n", what, exception ? "threw" : "done");
    return exception ? NULL : r;
}

void unlock_missions_check(void)
{
    static int state = -1; // -1 unknown, 0 off or done, >0 polls counted
    if (state == 0)
        return;
    if (state < 0) {
        state = access(FLAG, F_OK) == 0 ? 1 : 0;
        return;
    }
    if (++state < WAIT_POLLS)
        return;
    state = 0;

    so_module *mono = so_find("libmono.so");
    if (!mono)
        return;
    void *(*image_loaded)(const char *) = (void *(*)(const char *))so_symbol(mono, "mono_image_loaded");
    void *(*class_from_name)(void *, const char *, const char *) =
        (void *(*)(void *, const char *, const char *))so_symbol(mono, "mono_class_from_name");
    method_fn method = (method_fn)so_symbol(mono, "mono_class_get_method_from_name");
    void *(*run)(void *, void *, void **, void **) =
        (void *(*)(void *, void *, void **, void **))so_symbol(mono, "mono_runtime_invoke");
    void *game = image_loaded ? image_loaded("Assembly-CSharp") : NULL;
    if (!game || !class_from_name || !method || !run) {
        fprintf(stderr, "unlock: Assembly-CSharp not loaded\n");
        return;
    }
    void *act = class_from_name(game, "", "ActStructure");
    void *storage = class_from_name(game, "", "SecureStorage");
    if (!act || !storage) {
        fprintf(stderr, "unlock: ActStructure or SecureStorage class not found\n");
        return;
    }
    void *act_instance = invoke(run, method(act, "get_Instance", 0), NULL, "ActStructure.Instance");
    if (!act_instance)
        return;
    invoke(run, method(act, "UnlockAll", 0), act_instance, "ActStructure.UnlockAll");
    void *storage_instance = invoke(run, method(storage, "get_Instance", 0), NULL, "SecureStorage.Instance");
    if (storage_instance)
        invoke(run, method(storage, "SaveActStructure", 0), storage_instance, "SecureStorage.SaveActStructure");
    fflush(stderr);
}
