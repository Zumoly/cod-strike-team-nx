#ifndef PORT_PAD_DIALOG_H
#define PORT_PAD_DIALOG_H

// Presses the right or left button of the message box on screen, from the main thread.  Returns the
// result it gave the box, 1 or 2, or 0 when there was no such button.
int pad_dialog_press(int right);

// The HUD's zoom button: switches between the drone view and first person.  Returns 1 if called.
int pad_view_toggle(void);

// Throws a grenade in first person, as the game's pad code does.  Returns 1 if thrown.
int pad_throw_grenade(void);

#endif
