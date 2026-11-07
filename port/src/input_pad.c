// The Switch's controllers and touchscreen, presented as a PS5 pad on an Android touchscreen.
// Face buttons map by position, so B, the bottom button, is Cross.
#include "port/android_input.h"
#include "port/pad_dialog.h"

#include <stdio.h>
#include <string.h>
#include <switch.h>

#define KEY_ENTER 66
#define KEY_BACK 4
#define ACTION_DOWN 0
#define ACTION_UP 1
#define ACTION_MOVE 2
#define ACTION_POINTER_DOWN 5
#define ACTION_POINTER_UP 6
#define POINTER_SHIFT 8
#define NO_KEY (-1)

static const struct {
    u64 button;
    int key, also;
} g_buttons[] = {
    {HidNpadButton_B, 96, NO_KEY},           // Cross: BUTTON_A
    {HidNpadButton_A, 97, NO_KEY},           // Circle: BUTTON_B
    {HidNpadButton_Y, 99, NO_KEY},           // Square: BUTTON_X
    {HidNpadButton_X, 100, NO_KEY},          // Triangle: BUTTON_Y
    {HidNpadButton_L, NO_KEY, NO_KEY},       // drone view / first person (pad_view_toggle), not L1
    {HidNpadButton_R, NO_KEY, NO_KEY},       // grenade (pad_throw_grenade); not R1, which fires
    {HidNpadButton_ZL, 104, NO_KEY},         // L2, which is also an axis: aims down sights
    {HidNpadButton_ZR, 105, NO_KEY},         // R2
    {HidNpadButton_StickL, 106, 21},         // L3, and D-pad left: target to the left
    {HidNpadButton_StickR, 107, 22},         // R3, and D-pad right: target to the right
    {HidNpadButton_Plus, 108, KEY_ENTER},    // Options: the game reads Return
    {HidNpadButton_Minus, 109, KEY_BACK},    // Create: the game reads Escape
    {HidNpadButton_Up, 19, NO_KEY},          // the game reads the D-pad as arrow keys
    {HidNpadButton_Down, 20, NO_KEY},
    {HidNpadButton_Left, 21, NO_KEY},
    {HidNpadButton_Right, 22, NO_KEY},
};

static PadState g_pad;
static int g_ready;
static u64 g_held;
static float g_axes[AXIS_COUNT];
static int g_axes_sent;

static struct {
    int count;
    int ids[INPUT_MAX_POINTERS];
    float x[INPUT_MAX_POINTERS], y[INPUT_MAX_POINTERS];
} g_touch;

static float stick(s32 value)
{
    return (float)value / (float)JOYSTICK_MAX;
}

void camera_occlusion_check(void);
void scene_report_check(void);
void unlock_missions_check(void);
void gb_set_cursor(int visible, int x, int y);

// Virtual mouse for the menus, which the game only takes by touch.  Minus turns it on and off; the
// left stick moves the pointer and A touches the screen where it points (hold A to drag).  While it
// is on, the left stick and A go to the pointer only.
#define MOUSE_POINTER_ID 100
#define MOUSE_SPEED 16.0f // pixels a frame at full tilt
#define MOUSE_DEADZONE 0.12f

static struct {
    int on, touching;
    float x, y; // 1280x720 screen pixels
} g_mouse = {0, 0, 640.0f, 360.0f};

static void mouse_touch(int action)
{
    int id = MOUSE_POINTER_ID;
    float x = g_mouse.x * (float)input_screen_width() / 1280.0f;
    float y = g_mouse.y * (float)input_screen_height() / 720.0f;
    input_touch(action, 1, &id, &x, &y);
}

static void mouse_update(u64 held, u64 changed, HidAnalogStickState left)
{
    if ((changed & held & HidNpadButton_Minus)) {
        g_mouse.on = !g_mouse.on;
        if (!g_mouse.on && g_mouse.touching) {
            mouse_touch(ACTION_UP);
            g_mouse.touching = 0;
        }
    }
    if (!g_mouse.on) {
        gb_set_cursor(0, 0, 0);
        return;
    }
    float sx = stick(left.x), sy = -stick(left.y);
    float moved = 0;
    if (sx * sx + sy * sy > MOUSE_DEADZONE * MOUSE_DEADZONE) {
        // Squared for fine control near the centre.
        g_mouse.x += sx * (sx < 0 ? -sx : sx) * MOUSE_SPEED;
        g_mouse.y += sy * (sy < 0 ? -sy : sy) * MOUSE_SPEED;
        g_mouse.x = g_mouse.x < 0 ? 0 : g_mouse.x > 1279 ? 1279 : g_mouse.x;
        g_mouse.y = g_mouse.y < 0 ? 0 : g_mouse.y > 719 ? 719 : g_mouse.y;
        moved = 1;
    }
    int a = (held & HidNpadButton_A) != 0;
    if (a && !g_mouse.touching) {
        mouse_touch(ACTION_DOWN);
        g_mouse.touching = 1;
    } else if (!a && g_mouse.touching) {
        mouse_touch(ACTION_UP);
        g_mouse.touching = 0;
    } else if (a && moved) {
        mouse_touch(ACTION_MOVE);
    }
    gb_set_cursor(1, (int)g_mouse.x, (int)g_mouse.y);
}

static void poll_pad(void)
{
    camera_occlusion_check();
    scene_report_check();
    unlock_missions_check();
    padUpdate(&g_pad);
    u64 held = padGetButtons(&g_pad);
    u64 changed = held ^ g_held;
    g_held = held;
    mouse_update(held, changed, padGetStickPos(&g_pad, 0));
    for (size_t i = 0; i < sizeof g_buttons / sizeof *g_buttons; i++) {
        if (!(changed & g_buttons[i].button))
            continue;
        if (g_buttons[i].button == HidNpadButton_Minus)
            continue; // the virtual mouse's switch
        if (g_mouse.on && g_buttons[i].button == HidNpadButton_A)
            continue; // the pointer's touch
        int down = (held & g_buttons[i].button) != 0;
        if (g_buttons[i].key != NO_KEY)
            input_key(g_buttons[i].key, down);
        // Prompts (purchase, revive) only take touches: L3 presses the left button, R3 and A the
        // right one.  Nothing happens when no prompt is up, so in play these keep their own jobs.
        if (down && g_buttons[i].button == HidNpadButton_StickL)
            pad_dialog_press(0);
        if (down && (g_buttons[i].button == HidNpadButton_StickR || g_buttons[i].button == HidNpadButton_A))
            pad_dialog_press(1);
        if (down && g_buttons[i].button == HidNpadButton_L)
            pad_view_toggle();
        if (down && g_buttons[i].button == HidNpadButton_R)
            pad_throw_grenade();
        if (g_buttons[i].also != NO_KEY)
            input_key(g_buttons[i].also, down);
    }

    HidAnalogStickState left = padGetStickPos(&g_pad, 0), right = padGetStickPos(&g_pad, 1);
    float axes[AXIS_COUNT];
    axes[AXIS_LEFT_X] = g_mouse.on ? 0.0f : stick(left.x);
    axes[AXIS_LEFT_Y] = g_mouse.on ? 0.0f : -stick(left.y); // Android reports up as negative
    axes[AXIS_RIGHT_X] = stick(right.x);
    axes[AXIS_RIGHT_Y] = -stick(right.y);
    axes[AXIS_L2] = (held & HidNpadButton_ZL) ? 1.0f : 0.0f;
    axes[AXIS_R2] = (held & HidNpadButton_ZR) ? 1.0f : 0.0f;
    if (!g_axes_sent || memcmp(axes, g_axes, sizeof axes) != 0) {
        memcpy(g_axes, axes, sizeof axes);
        g_axes_sent = 1;
        input_joystick(axes);
    }
}

static int find_id(const int *ids, int count, int id)
{
    for (int i = 0; i < count; i++)
        if (ids[i] == id)
            return i;
    return -1;
}

static void send_touch(int action)
{
    input_touch(action, g_touch.count, g_touch.ids, g_touch.x, g_touch.y);
}

// Android reports each finger going down or up as its own event, naming its index.
static void poll_touch(void)
{
    HidTouchScreenState state;
    if (hidGetTouchScreenStates(&state, 1) == 0)
        return;
    int count = state.count > INPUT_MAX_POINTERS ? INPUT_MAX_POINTERS : (int)state.count;
    int ids[INPUT_MAX_POINTERS];
    float x[INPUT_MAX_POINTERS], y[INPUT_MAX_POINTERS];
    float sx = (float)input_screen_width() / 1280.0f, sy = (float)input_screen_height() / 720.0f;
    for (int i = 0; i < count; i++) {
        ids[i] = (int)state.touches[i].finger_id;
        x[i] = (float)state.touches[i].x * sx;
        y[i] = (float)state.touches[i].y * sy;
    }

    for (int i = g_touch.count - 1; i >= 0; i--) {
        if (find_id(ids, count, g_touch.ids[i]) >= 0)
            continue;
        send_touch(g_touch.count == 1 ? ACTION_UP : ACTION_POINTER_UP | (i << POINTER_SHIFT));
        g_touch.count--;
        memmove(&g_touch.ids[i], &g_touch.ids[i + 1], (size_t)(g_touch.count - i) * sizeof g_touch.ids[0]);
        memmove(&g_touch.x[i], &g_touch.x[i + 1], (size_t)(g_touch.count - i) * sizeof g_touch.x[0]);
        memmove(&g_touch.y[i], &g_touch.y[i + 1], (size_t)(g_touch.count - i) * sizeof g_touch.y[0]);
    }

    int moved = 0;
    for (int i = 0; i < count; i++) {
        int at = find_id(g_touch.ids, g_touch.count, ids[i]);
        if (at >= 0) {
            moved |= g_touch.x[at] != x[i] || g_touch.y[at] != y[i];
            g_touch.x[at] = x[i];
            g_touch.y[at] = y[i];
            continue;
        }
        at = g_touch.count++;
        g_touch.ids[at] = ids[i];
        g_touch.x[at] = x[i];
        g_touch.y[at] = y[i];
        send_touch(at == 0 ? ACTION_DOWN : ACTION_POINTER_DOWN | (at << POINTER_SHIFT));
    }
    if (moved)
        send_touch(ACTION_MOVE);
}

void input_poll(void)
{
    if (!g_ready) {
        g_ready = 1;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&g_pad);
        hidInitializeTouchScreen();
    }
    poll_pad();
    poll_touch();
}
