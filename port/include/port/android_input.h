// Android input as libunity reads it: a fake AInputQueue, fed once per frame.
#ifndef PORT_ANDROID_INPUT_H
#define PORT_ANDROID_INPUT_H

#include <stdint.h>

#define INPUT_DEVICE_ID 1
#define INPUT_MAX_POINTERS 10

// Order of the axes passed to input_joystick.  Triggers travel on the HAT axes on purpose:
// Unity numbers them 4 and 5, which is what the game reads as AndAxis5 and AndAxis6.
enum { AXIS_LEFT_X, AXIS_LEFT_Y, AXIS_RIGHT_X, AXIS_RIGHT_Y, AXIS_L2, AXIS_R2, AXIS_COUNT };

void input_key(int keycode, int down);
void input_joystick(const float axes[AXIS_COUNT]);
void input_touch(int action, int count, const int *ids, const float *x, const float *y);

// Unity's own size, from setScreenSize.  Touches must arrive in these units.
void input_set_screen_size(int width, int height);
int input_screen_width(void);
int input_screen_height(void);

// Reads the real controllers (src/input_pad.c).
void input_poll(void);

#endif
