// Table the .so loader uses to bind libunity's GLES/EGL imports.
#ifndef PORT_GL_SHIM_H
#define PORT_GL_SHIM_H

typedef void (*gl_fn)(void);

struct gl_import {
    const char *name;
    gl_fn fn;
};

extern const struct gl_import gl_imports[];
extern const unsigned gl_import_count;

#endif
