// ELF32 ARM shared-object loader for the game's Android libraries.
// Loading has separate phases because TEXTREL libraries need writable code until
// relocation ends, and because initializers must run after all imports are bound.
#ifndef PORT_SO_LOADER_H
#define PORT_SO_LOADER_H

#include <stddef.h>
#include <stdint.h>

#define SO_MAX_SEGS 4

struct so_seg {
    uintptr_t addr; // page aligned
    size_t size;    // page aligned
    int exec;
};

typedef struct so_module {
    struct so_module *next;
    char name[64];
    uintptr_t base; // final address: what relocations and symbols are computed against
    uintptr_t load; // where the image can be read now: staging, then base after finalize
    size_t span;
    struct so_seg segs[SO_MAX_SEGS];
    int nsegs;
    uintptr_t rw_start;
    uint32_t text_offset, text_size;

    const uint8_t *symtab; // Elf32_Sym
    const char *strtab;
    const uint32_t *hash;
    const uint8_t *rel;
    size_t relsz;
    const uint8_t *jmprel;
    size_t pltrelsz;
    void (*init)(void);
    uint32_t init_array_offset;
    size_t init_count;
    uintptr_t exidx;
    size_t exidx_count;

    unsigned unresolved;
} so_module;

// Backend: Horizon code memory, or plain mmap under emulation.
void *so_code_reserve(size_t size, void **stage);         // returns the final address
int so_code_commit(void *virt, void *stage, size_t size); // staging becomes code at virt
int so_code_protect(void *addr, size_t size, int exec);   // exec: RX, otherwise RW

so_module *so_load(const char *name, const char *path);
int so_relocate(so_module *m);
void so_patch_kuser(so_module *m);
int so_patch_mono(so_module *m);
int so_finalize(so_module *m);
void so_run_init(so_module *m);

void *so_symbol(const so_module *m, const char *name);
so_module *so_find(const char *name);
void so_set_lib_dir(const char *dir);

const char *so_describe(uintptr_t addr, char *out, size_t size);
int b_gc_bridge_init(const so_module *mono);

// Host import table (src/loader/imports.c).
void *so_import_lookup(const char *name);
unsigned so_unresolved_count(void);
const char *so_unresolved_name(unsigned i);

// Guest-visible loader services.
void *b_dlopen(const char *name, int flags);
void *b_dlsym(void *handle, const char *name);
int b_dlclose(void *handle);
const char *b_dlerror(void);
uintptr_t b_unwind_find_exidx(uintptr_t pc, int *count);

#endif
