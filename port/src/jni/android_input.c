// The Android input calls libunity imports.  Unity polls an AInputQueue once per frame on the
// main thread, so a plain ring needs no locking.  An AInputEvent is one slot of that ring, valid
// until the next event is taken.
#include "port/android_input.h"
#include "port/bionic_thread.h"

#include <string.h>

#define RING 256

#define TYPE_KEY 1
#define TYPE_MOTION 2
#define SOURCE_GAMEPAD 0x401
#define SOURCE_JOYSTICK 0x01000010
#define SOURCE_TOUCH 0x1002
#define ACTION_MOVE 2
#define POLL_CALLBACK 1
#define POLL_TIMEOUT (-3)

struct event {
    int type, source, device, action, keycode;
    int64_t time;
    int count;
    int ids[INPUT_MAX_POINTERS];
    float x[INPUT_MAX_POINTERS], y[INPUT_MAX_POINTERS];
    float axes[AXIS_COUNT];
};

static struct event g_ring[RING];
static unsigned g_head, g_tail;
static int g_screen_width = 1280, g_screen_height = 720;

void input_set_screen_size(int width, int height)
{
    g_screen_width = width;
    g_screen_height = height;
}

int input_screen_width(void) { return g_screen_width; }
int input_screen_height(void) { return g_screen_height; }

static struct event *next_slot(void)
{
    if (g_head - g_tail == RING)
        return NULL;
    struct event *e = &g_ring[g_head++ % RING];
    memset(e, 0, sizeof *e);
    e->time = b_monotonic_ns();
    e->device = INPUT_DEVICE_ID;
    return e;
}

void input_key(int keycode, int down)
{
    struct event *e = next_slot();
    if (!e)
        return;
    e->type = TYPE_KEY;
    e->source = SOURCE_GAMEPAD;
    e->keycode = keycode;
    e->action = down ? 0 : 1;
}

void input_joystick(const float axes[AXIS_COUNT])
{
    struct event *e = next_slot();
    if (!e)
        return;
    e->type = TYPE_MOTION;
    e->source = SOURCE_JOYSTICK;
    e->action = ACTION_MOVE;
    e->count = 1;
    memcpy(e->axes, axes, sizeof e->axes);
}

void input_touch(int action, int count, const int *ids, const float *x, const float *y)
{
    struct event *e = next_slot();
    if (!e)
        return;
    if (count > INPUT_MAX_POINTERS)
        count = INPUT_MAX_POINTERS;
    e->type = TYPE_MOTION;
    e->source = SOURCE_TOUCH;
    e->device = INPUT_DEVICE_ID + 1;
    e->action = action;
    e->count = count;
    for (int i = 0; i < count; i++) {
        e->ids[i] = ids[i];
        e->x[i] = x[i];
        e->y[i] = y[i];
    }
}

// ---- what libunity imports ------------------------------------------------------

void *b_input_queue(void) { return g_ring; }

int b_looper_poll(int timeout, int *fd, int *events, void **data)
{
    (void)timeout;
    (void)fd;
    (void)events;
    (void)data;
    return g_head != g_tail ? POLL_CALLBACK : POLL_TIMEOUT;
}

int b_queue_get_event(void *queue, struct event **out)
{
    (void)queue;
    if (g_head == g_tail)
        return -1;
    *out = &g_ring[g_tail++ % RING];
    return 0;
}

int b_queue_pre_dispatch(void *queue, struct event *e)
{
    (void)queue;
    (void)e;
    return 0;
}

void b_queue_finish(void *queue, struct event *e, int handled)
{
    (void)queue;
    (void)e;
    (void)handled;
}

void b_queue_attach(void *queue, void *looper, int ident, void *callback, void *data)
{
    (void)queue;
    (void)looper;
    (void)ident;
    (void)callback;
    (void)data;
}

void b_queue_detach(void *queue) { (void)queue; }

void b_watchdog(double now) { (void)now; }
void b_forward_to_dalvik(void *event, void *env) { (void)event; (void)env; }

int b_event_type(const struct event *e) { return e->type; }
int b_event_source(const struct event *e) { return e->source; }
int b_event_device(const struct event *e) { return e->device; }

int b_key_code(const struct event *e) { return e->keycode; }
int b_key_meta(const struct event *e) { (void)e; return 0; }
int b_key_action(const struct event *e) { return e->action; }

int b_motion_action(const struct event *e) { return e->action; }
int64_t b_motion_time(const struct event *e) { return e->time; }
int b_motion_count(const struct event *e) { return e->count; }
int b_motion_pointer_id(const struct event *e, int i) { return e->ids[i]; }
float b_motion_x(const struct event *e, int i) { return e->x[i]; }
float b_motion_y(const struct event *e, int i) { return e->y[i]; }
int b_motion_history(const struct event *e) { (void)e; return 0; }
float b_motion_hist(const struct event *e, int i, int h) { (void)e; (void)i; (void)h; return 0.0f; }
int64_t b_motion_hist_time(const struct event *e, int h) { (void)h; return e->time; }

static float axis_value(const struct event *e, int axis, int pointer)
{
    (void)pointer;
    switch (axis) {
    case 0: return e->axes[AXIS_LEFT_X];
    case 1: return e->axes[AXIS_LEFT_Y];
    case 11: return e->axes[AXIS_RIGHT_X];
    case 14: return e->axes[AXIS_RIGHT_Y];
    case 15: return e->axes[AXIS_L2];
    case 16: return e->axes[AXIS_R2];
    default: return 0.0f;
    }
}

// libunity imports this as a variable holding a function pointer: on Android, libmain fills it
// with dlsym because the call only exists on newer systems.
float (*b_get_axis_value)(const struct event *, int, int) = axis_value;
