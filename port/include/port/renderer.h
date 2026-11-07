// Boundary between the GLES emulation layer and deko3D.
// The GL layer tracks API state; only renderer_deko3d.c may include <deko3d.h>.
// That keeps the GL logic testable on a host with a recording backend.
#ifndef PORT_RENDERER_H
#define PORT_RENDERER_H

#define RENDERER_WIDTH  1280
#define RENDERER_HEIGHT 720

int renderer_init(void);
void renderer_shutdown(void);

// Clears the next swapchain image and presents it.
void renderer_present_clear(float r, float g, float b);

#endif
