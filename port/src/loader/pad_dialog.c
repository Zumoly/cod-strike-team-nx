// Presses the buttons of the game's message boxes from the pad.  The game only takes them by touch,
// and its C# is compiled ahead of time, so this calls what a touch on the button would call.
// The first button is on the right: "Yes" on a purchase, "No" on a destructive question.
#include "port/pad_dialog.h"
#include "port/so_loader.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define ARRAY_LENGTH_WORD 3 // MonoArray: vtable, sync, bounds, max_length
#define ARRAY_DATA_WORD 4
#define RESULT_OK 1
#define RESULT_CANCEL 2

static void *(*g_invoke)(void *, void *, void **, void **);
static void (*g_field_value)(void *, void *, void *);
static void *(*g_field)(void *, const char *);
static void *(*g_object_class)(void *);
static void *(*g_type_object)(void *, void *);
static void *(*g_class_type)(void *);
static void *(*g_domain)(void);
static void *g_box_class, *g_find, *g_ok, *g_cancel, *g_buttons;
static void *g_continue_class, *g_continue_active, *g_continue_gmg_revive, *g_continue_campaign_revive,
    *g_continue_fail, *g_continue_gmg;
static void *g_hud_instance, *g_hud_zoom, *g_hud_grenade_start, *g_hud_grenade_end, *g_hud_grenade_locked;
static void *g_gc_instance, *g_gc_first_person, *g_gc_paused;

static int resolve(void)
{
    static int done;
    if (done)
        return g_find != NULL;
    so_module *mono = so_find("libmono.so");
    if (!mono)
        return 0;
    void *(*image_loaded)(const char *) = (void *(*)(const char *))so_symbol(mono, "mono_image_loaded");
    void *(*class_from_name)(void *, const char *, const char *) =
        (void *(*)(void *, const char *, const char *))so_symbol(mono, "mono_class_from_name");
    void *(*method_from_name)(void *, const char *, int) =
        (void *(*)(void *, const char *, int))so_symbol(mono, "mono_class_get_method_from_name");
    void *game = image_loaded("Assembly-CSharp"), *engine = image_loaded("UnityEngine");
    if (!game || !engine)
        return 0; // not loaded yet: try again on the next press
    done = 1;
    g_invoke = (void *(*)(void *, void *, void **, void **))so_symbol(mono, "mono_runtime_invoke");
    g_field_value = (void (*)(void *, void *, void *))so_symbol(mono, "mono_field_get_value");
    g_field = (void *(*)(void *, const char *))so_symbol(mono, "mono_class_get_field_from_name");
    g_object_class = (void *(*)(void *))so_symbol(mono, "mono_object_get_class");
    g_type_object = (void *(*)(void *, void *))so_symbol(mono, "mono_type_get_object");
    g_class_type = (void *(*)(void *))so_symbol(mono, "mono_class_get_type");
    g_domain = (void *(*)(void))so_symbol(mono, "mono_domain_get");

    g_box_class = class_from_name(game, "", "MessageBox");
    void *object = class_from_name(engine, "UnityEngine", "Object");
    if (!g_box_class || !object)
        return 0;
    // The revive screen is a screen of its own, not a MessageBox.
    g_continue_class = class_from_name(game, "", "ContinueScreen");
    void *screen = class_from_name(game, "", "FrontEndScreen");
    if (g_continue_class && screen) {
        g_continue_active = method_from_name(screen, "get_IsActive", 0);
        g_continue_gmg_revive = method_from_name(g_continue_class, "GMGRevive", 0);
        g_continue_campaign_revive = method_from_name(g_continue_class, "CampaignReviveAll", 0);
        g_continue_fail = method_from_name(g_continue_class, "FailMission", 0);
        g_continue_gmg = g_field(g_continue_class, "m_bGMG");
    }
    // The HUD's zoom button: drone view <-> first person (a MOGA Pro's L1 calls the same).
    void *hud = class_from_name(game, "", "CommonHudController");
    if (hud) {
        g_hud_instance = method_from_name(hud, "get_Instance", 0);
        g_hud_zoom = method_from_name(hud, "ZoomToggled", 0);
        g_hud_grenade_start = method_from_name(hud, "StartThrowGrenadeFirst", 0);
        g_hud_grenade_end = method_from_name(hud, "EndThrowGrenadeFirst", 0);
        g_hud_grenade_locked = method_from_name(hud, "get_TPPDropGrenadeLocked", 0);
    }
    void *gc = class_from_name(game, "", "GameController");
    if (gc) {
        g_gc_instance = method_from_name(gc, "get_Instance", 0);
        g_gc_first_person = method_from_name(gc, "get_IsFirstPerson", 0);
        g_gc_paused = method_from_name(gc, "get_IsPaused", 0);
    }
    g_ok = method_from_name(g_box_class, "OnOkPressed", 0);
    g_cancel = method_from_name(g_box_class, "OnCancelPressed", 0);
    g_buttons = g_field(g_box_class, "ButtonData");
    if (g_ok && g_cancel && g_buttons)
        g_find = method_from_name(object, "FindObjectOfType", 1);
    return g_find != NULL;
}

// Test for the missing ground and sky: every draw the game makes reaches the renderer, so Unity
// itself leaves them out.  With sd:/switch/cod_st/no_occlusion present, this turns occlusion
// culling off on every camera, once a second, so cameras made later are covered too.
void camera_occlusion_check(void)
{
    static int enabled = -1, calls, last_count = -1;
    static void *all_cameras, *set_occlusion;
    if (enabled < 0)
        enabled = access("sdmc:/switch/cod_st/no_occlusion", F_OK) == 0;
    if (!enabled || calls++ % 60 != 0 || !resolve())
        return;
    if (!set_occlusion) {
        so_module *mono = so_find("libmono.so");
        void *(*image_loaded)(const char *) = (void *(*)(const char *))so_symbol(mono, "mono_image_loaded");
        void *(*class_from_name)(void *, const char *, const char *) =
            (void *(*)(void *, const char *, const char *))so_symbol(mono, "mono_class_from_name");
        void *(*method_from_name)(void *, const char *, int) =
            (void *(*)(void *, const char *, int))so_symbol(mono, "mono_class_get_method_from_name");
        void *camera = class_from_name(image_loaded("UnityEngine"), "UnityEngine", "Camera");
        if (!camera)
            return;
        all_cameras = method_from_name(camera, "get_allCameras", 0);
        set_occlusion = method_from_name(camera, "set_useOcclusionCulling", 1);
        fprintf(stderr, "occlusion test: allCameras %p setter %p\n", all_cameras, set_occlusion);
        if (!all_cameras || !set_occlusion) {
            enabled = 0;
            return;
        }
    }
    void *exception = NULL;
    void *array = g_invoke(all_cameras, NULL, NULL, &exception);
    if (!array || exception)
        return;
    int count = (int)((uint32_t *)array)[ARRAY_LENGTH_WORD];
    uint8_t off = 0;
    void *args[1] = {&off};
    for (int i = 0; i < count; i++) {
        void *cam = ((void **)array)[ARRAY_DATA_WORD + i];
        if (cam)
            g_invoke(set_occlusion, cam, args, &exception);
    }
    if (count != last_count) {
        fprintf(stderr, "occlusion test: culling off on %d cameras\n", count);
        last_count = count;
    }
}

int pad_view_toggle(void)
{
    if (!resolve() || !g_hud_instance || !g_hud_zoom)
        return 0;
    void *exception = NULL;
    void *hud = g_invoke(g_hud_instance, NULL, NULL, &exception);
    if (!hud || exception)
        return 0; // no HUD: a menu
    g_invoke(g_hud_zoom, hud, NULL, &exception);
    return exception ? 0 : 1;
}

// A boxed bool from a getter: MonoObject header (vtable, sync), then the value.  -1 on failure.
static int call_bool(void *method, void *self)
{
    if (!method)
        return -1;
    void *exception = NULL;
    void *r = g_invoke(method, self, NULL, &exception);
    return !r || exception ? -1 : *(uint8_t *)((void **)r + 2) != 0;
}

// Throws a grenade as the game's own pad code does (UpdateIphonePadInput: rightShoulder): only in
// first person, unpaused, and when the mission has not locked grenades.
int pad_throw_grenade(void)
{
    if (!resolve() || !g_hud_instance || !g_hud_grenade_start || !g_hud_grenade_end || !g_gc_instance)
        return 0;
    void *exception = NULL;
    void *gc = g_invoke(g_gc_instance, NULL, NULL, &exception);
    if (!gc || exception || call_bool(g_gc_first_person, gc) != 1 || call_bool(g_gc_paused, gc) == 1)
        return 0;
    void *hud = g_invoke(g_hud_instance, NULL, NULL, &exception);
    if (!hud || exception || call_bool(g_hud_grenade_locked, hud) == 1)
        return 0;
    g_invoke(g_hud_grenade_start, hud, NULL, &exception);
    if (!exception)
        g_invoke(g_hud_grenade_end, hud, NULL, &exception);
    return exception ? 0 : 1;
}

// The revive screen (ContinueScreen): the right stick revives, as the button under the timer does,
// and the left stick gives up, as its "No" does.  Survival (GMG) and the campaign revive differently.
static int press_continue(int right)
{
    if (!g_continue_class || !g_continue_active || !g_continue_fail)
        return 0;
    void *exception = NULL;
    void *type = g_type_object(g_domain(), g_class_type(g_continue_class));
    void *find_args[1] = {type};
    void *screen = g_invoke(g_find, NULL, find_args, &exception);
    if (!screen || exception)
        return 0;
    void *active = g_invoke(g_continue_active, screen, NULL, &exception);
    if (!active || exception || !*(uint8_t *)((void **)active + 2)) // boxed bool: vtable, sync, value
        return 0;
    void *method = g_continue_fail;
    if (right) {
        uint8_t gmg = 0;
        if (g_continue_gmg)
            g_field_value(screen, g_continue_gmg, &gmg);
        method = gmg ? g_continue_gmg_revive : g_continue_campaign_revive;
    }
    if (!method)
        return 0;
    g_invoke(method, screen, NULL, &exception);
    fprintf(stderr, "pad: revive screen %s%s\n", right ? "revive" : "no", exception ? " (threw)" : "");
    return exception ? 0 : 1;
}

int pad_dialog_press(int right)
{
    if (!resolve())
        return 0;
    void *exception = NULL;
    void *type = g_type_object(g_domain(), g_class_type(g_box_class));
    void *find_args[1] = {type};
    void *box = g_invoke(g_find, NULL, find_args, &exception);
    if (!box || exception)
        return press_continue(right);

    void *array = NULL;
    g_field_value(box, g_buttons, &array);
    size_t index = right ? 0 : 1;
    if (!array || index >= ((uint32_t *)array)[ARRAY_LENGTH_WORD])
        return 0;
    void *button = ((void **)array)[ARRAY_DATA_WORD + index];
    void *result_field = button ? g_field(g_object_class(button), "Result") : NULL;
    if (!result_field)
        return 0;
    int32_t result = 0;
    g_field_value(button, result_field, &result);
    if (result != RESULT_OK && result != RESULT_CANCEL)
        return 0; // a share button, which has its own flow
    g_invoke(result == RESULT_OK ? g_ok : g_cancel, box, NULL, &exception);
    return exception ? 0 : result;
}
