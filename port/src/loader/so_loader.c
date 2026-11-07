#include "port/so_loader.h"
#include "port/vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 0x1000u
#define PAGE_DOWN(x) ((uintptr_t)(x) & ~(uintptr_t)(PAGE - 1))
#define PAGE_UP(x) PAGE_DOWN((uintptr_t)(x) + PAGE - 1)

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_ARM_EXIDX 0x70000001

#define DT_NULL 0
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_INIT 12
#define DT_REL 17
#define DT_RELSZ 18
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27

#define R_ARM_ABS32 2
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23

#define STB_WEAK 2
#define STT_OBJECT 1

#define KUSER_MEMORY_BARRIER 0xFFFF0FA0u
#define KUSER_CMPXCHG 0xFFFF0FC0u

#define MONO_FLUSH_ENTRY 0x115768u
#define MONO_FLUSH_SVC 0x1157ACu

#define MAX_THUNKS 2048

typedef struct {
    uint32_t st_name, st_value, st_size;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
} Sym;

typedef struct {
    uint8_t e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Ehdr;

typedef struct {
    uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} Phdr;

typedef struct {
    uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize;
} Shdr;

extern char so_thunks[]; // src/loader/so_thunks.S: 8 bytes per entry
void port_kuser_cmpxchg(void);
void port_kuser_barrier(void);
void port_icache_flush(void *start, int size);

static so_module *g_modules;
static char g_lib_dir[256];
static const char *g_thunk_names[MAX_THUNKS];
static unsigned g_thunks;
static uint8_t g_zero_data[256] __attribute__((aligned(16)));

void so_set_lib_dir(const char *dir)
{
    snprintf(g_lib_dir, sizeof g_lib_dir, "%s", dir);
}

unsigned so_unresolved_count(void)
{
    return g_thunks;
}

const char *so_unresolved_name(unsigned i)
{
    return i < g_thunks ? g_thunk_names[i] : NULL;
}

// Every import without an implementation lands here, named, instead of jumping
// to address 0.  That turns bring-up into a list of what to write next.
void so_missing(unsigned index)
{
    fprintf(stderr, "so_loader: called unimplemented import '%s' from %p\n",
            index < g_thunks ? g_thunk_names[index] : "?", __builtin_return_address(0));
    fflush(stderr);
    abort();
}

static uintptr_t thunk_for(const char *name)
{
    for (unsigned i = 0; i < g_thunks; i++)
        if (strcmp(g_thunk_names[i], name) == 0)
            return (uintptr_t)so_thunks + i * 8;
    if (g_thunks == MAX_THUNKS)
        abort();
    g_thunk_names[g_thunks] = name;
    return (uintptr_t)so_thunks + g_thunks++ * 8;
}

static uint32_t elf_hash(const char *s)
{
    uint32_t h = 0;
    for (; *s; s++) {
        h = (h << 4) + (uint8_t)*s;
        uint32_t g = h & 0xF0000000u;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

void *so_symbol(const so_module *m, const char *name)
{
    if (!m->hash)
        return NULL;
    uint32_t nbucket = m->hash[0];
    const uint32_t *bucket = m->hash + 2;
    const uint32_t *chain = bucket + nbucket;
    for (uint32_t i = bucket[elf_hash(name) % nbucket]; i; i = chain[i]) {
        const Sym *s = (const Sym *)(m->symtab + i * sizeof(Sym));
        if (s->st_shndx != 0 && strcmp(m->strtab + s->st_name, name) == 0)
            return (void *)(m->base + s->st_value);
    }
    return NULL;
}

so_module *so_find(const char *name)
{
    for (so_module *m = g_modules; m; m = m->next)
        if (strcmp(m->name, name) == 0)
            return m;
    return NULL;
}

static void parse_dynamic(so_module *m, uintptr_t dyn_addr)
{
    size_t array_size = 0;
    for (const uint32_t *d = (const uint32_t *)dyn_addr; d[0] != DT_NULL; d += 2) {
        uintptr_t ptr = m->load + d[1];
        switch (d[0]) {
        case DT_HASH: m->hash = (const uint32_t *)ptr; break;
        case DT_STRTAB: m->strtab = (const char *)ptr; break;
        case DT_SYMTAB: m->symtab = (const uint8_t *)ptr; break;
        case DT_REL: m->rel = (const uint8_t *)ptr; break;
        case DT_RELSZ: m->relsz = d[1]; break;
        case DT_JMPREL: m->jmprel = (const uint8_t *)ptr; break;
        case DT_PLTRELSZ: m->pltrelsz = d[1]; break;
        case DT_INIT: m->init = (void (*)(void))(m->base + d[1]); break; // runs at its final address
        case DT_INIT_ARRAY: m->init_array_offset = d[1]; break;
        case DT_INIT_ARRAYSZ: array_size = d[1]; break;
        default: break;
        }
    }
    m->init_count = array_size / sizeof(void *);
}

// Section headers are optional in ELF, so this leaves an empty range if they are missing.
static void find_text_section(so_module *m, const uint8_t *file, size_t size)
{
    const Ehdr *eh = (const Ehdr *)file;
    if (!eh->e_shoff || eh->e_shoff + (size_t)eh->e_shnum * eh->e_shentsize > size)
        return;
    const Shdr *sh = (const Shdr *)(file + eh->e_shoff);
    const char *names = (const char *)file + sh[eh->e_shstrndx].sh_offset;
    for (unsigned i = 0; i < eh->e_shnum; i++) {
        if (strcmp(names + sh[i].sh_name, ".text") == 0) {
            m->text_offset = sh[i].sh_addr;
            m->text_size = sh[i].sh_size;
        }
    }
}

so_module *so_load(const char *name, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "so_loader: cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)size);
    so_module *m = calloc(1, sizeof *m);
    if (!file || !m || fread(file, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(file);
        free(m);
        return NULL;
    }
    fclose(f);

    const Ehdr *eh = (const Ehdr *)file;
    if (memcmp(eh->e_ident, "\177ELF", 4) != 0 || eh->e_ident[4] != 1 || eh->e_machine != 40) {
        fprintf(stderr, "so_loader: %s is not ELF32 ARM\n", path);
        free(file);
        free(m);
        return NULL;
    }
    const Phdr *ph = (const Phdr *)(file + eh->e_phoff);

    uintptr_t end = 0;
    for (unsigned i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > end)
            end = ph[i].p_vaddr + ph[i].p_memsz;
    m->span = PAGE_UP(end);
    void *stage = NULL;
    m->base = (uintptr_t)so_code_reserve(m->span, &stage);
    m->load = (uintptr_t)stage;
    if (!m->base || !m->load) {
        fprintf(stderr, "so_loader: no code memory for %s (%zu bytes)\n", path, m->span);
        free(file);
        free(m);
        return NULL;
    }
    memset(stage, 0, m->span);

    uintptr_t dyn = 0;
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        const Phdr *p = &ph[i];
        if (p->p_type == PT_LOAD) {
            memcpy((void *)(m->load + p->p_vaddr), file + p->p_offset, p->p_filesz);
            if (m->nsegs == SO_MAX_SEGS) {
                fprintf(stderr, "so_loader: too many segments in %s\n", path);
                abort();
            }
            struct so_seg *s = &m->segs[m->nsegs++];
            s->addr = PAGE_DOWN(m->base + p->p_vaddr);
            s->size = PAGE_UP(m->base + p->p_vaddr + p->p_memsz) - s->addr;
            s->exec = (p->p_flags & 1) != 0;
            if (!s->exec && !m->rw_start)
                m->rw_start = m->base + p->p_vaddr;
        } else if (p->p_type == PT_DYNAMIC) {
            dyn = m->load + p->p_vaddr;
        } else if (p->p_type == PT_ARM_EXIDX) {
            m->exidx = m->base + p->p_vaddr;
            m->exidx_count = p->p_memsz / 8;
        }
    }
    find_text_section(m, file, (size_t)size);
    free(file);
    if (!dyn) {
        fprintf(stderr, "so_loader: %s has no dynamic section\n", path);
        abort();
    }
    parse_dynamic(m, dyn);
    snprintf(m->name, sizeof m->name, "%s", name);

    // Appended, so lookups search in load order like the Android linker's scope.
    so_module **tail = &g_modules;
    while (*tail)
        tail = &(*tail)->next;
    *tail = m;
    return m;
}

static uintptr_t resolve(so_module *m, const Sym *sym, int rel_type)
{
    const char *name = m->strtab + sym->st_name;
    if (sym->st_shndx != 0)
        return m->base + sym->st_value;

    // Boehm treats data_start as the base of its static roots.  On Android it comes
    // from the executable; here the matching range is the library's own data.
    if (strcmp(name, "data_start") == 0)
        return m->rw_start;

    void *p = so_import_lookup(name);
    if (p)
        return (uintptr_t)p;
    for (so_module *o = g_modules; o; o = o->next) {
        if (o != m && (p = so_symbol(o, name)) != NULL)
            return (uintptr_t)p;
    }
    if ((sym->st_info >> 4) == STB_WEAK)
        return 0;

    m->unresolved++;
    if ((sym->st_info & 15) == STT_OBJECT && rel_type != R_ARM_JUMP_SLOT) {
        fprintf(stderr, "so_loader: %s: unresolved data symbol %s\n", m->name, name);
        return (uintptr_t)g_zero_data;
    }
    return thunk_for(name);
}

static int apply_rel(so_module *m, const uint8_t *table, size_t size)
{
    for (size_t i = 0; i + 8 <= size; i += 8) {
        uint32_t offset, info;
        memcpy(&offset, table + i, 4);
        memcpy(&info, table + i + 4, 4);
        uint32_t type = info & 0xFF;
        uint32_t sym_index = info >> 8;
        uint32_t *where = (uint32_t *)(m->load + offset);

        switch (type) {
        case R_ARM_RELATIVE:
            *where += (uint32_t)m->base;
            break;
        case R_ARM_ABS32:
            *where += (uint32_t)resolve(m, (const Sym *)(m->symtab + sym_index * sizeof(Sym)), type);
            break;
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT:
            *where = (uint32_t)resolve(m, (const Sym *)(m->symtab + sym_index * sizeof(Sym)), type);
            break;
        default:
            fprintf(stderr, "so_loader: %s: unsupported relocation %u at 0x%x\n", m->name, (unsigned)type,
                    (unsigned)offset);
            return -1;
        }
    }
    return 0;
}

int so_relocate(so_module *m)
{
    if (apply_rel(m, m->rel, m->relsz) != 0)
        return -1;
    return apply_rel(m, m->jmprel, m->pltrelsz);
}

// libgcc's __sync_* reach Linux kernel helpers through literal words.  Those
// addresses do not exist on Horizon, so the words are repointed at host versions.
// .text only: the executable segment also holds .rodata, where the same bytes can be
// real data.  Patching there corrupted a PNG embedded in libunity.
void so_patch_kuser(so_module *m)
{
    uint32_t *w = (uint32_t *)(m->load + m->text_offset);
    size_t n = m->text_size / 4;
    for (size_t i = 0; i < n; i++) {
        if (w[i] == KUSER_CMPXCHG)
            w[i] = (uint32_t)(uintptr_t)port_kuser_cmpxchg;
        else if (w[i] == KUSER_MEMORY_BARRIER)
            w[i] = (uint32_t)(uintptr_t)port_kuser_barrier;
        // ARMv6-style barriers in libmono's Boehm: a real core may trap them, so each
        // becomes its ARMv7 equivalent.
        else if ((w[i] & 0xFFFF0FFFu) == 0xEE070FBAu)
            w[i] = 0xF57FF05Bu; // dmb ish
        else if ((w[i] & 0xFFFF0FFFu) == 0xEE070F9Au)
            w[i] = 0xF57FF04Bu; // dsb ish
        else if ((w[i] & 0xFFFF0FFFu) == 0xEE070F95u)
            w[i] = 0xF57FF06Fu; // isb
    }
}

// Mono's icache flush is the only Linux syscall issued inline, so it cannot be
// bound as an import.  Replaces the function entry with an absolute jump.  The
// instruction checks refuse any libmono build other than the analysed one.
int so_patch_mono(so_module *m)
{
    uint32_t *entry = (uint32_t *)(m->load + MONO_FLUSH_ENTRY);
    uint32_t *svc = (uint32_t *)(m->load + MONO_FLUSH_SVC);
    if (entry[0] != 0xE92D0890u || *svc != 0xEF000000u) {
        fprintf(stderr, "so_loader: libmono does not match the analysed build\n");
        return -1;
    }
    entry[0] = 0xE51FF004u; // ldr pc, [pc, #-4]
    entry[1] = (uint32_t)(uintptr_t)port_icache_flush;
    return 0;
}

// Staging becomes the code mapping here and stops being readable at its old address,
// so every pointer into the image moves to the final address first.
static void rebase_tables(so_module *m)
{
    uintptr_t delta = m->base - m->load;
    m->symtab += delta;
    m->strtab += delta;
    m->hash = (const uint32_t *)((uintptr_t)m->hash + delta);
    if (m->rel)
        m->rel += delta;
    if (m->jmprel)
        m->jmprel += delta;
    m->load = m->base;
}

// Executable first: the kernel refuses W to X, so a shared page must be decided as X.
int so_finalize(so_module *m)
{
    if (so_code_commit((void *)m->base, (void *)m->load, m->span) != 0)
        return -1;
    rebase_tables(m);

    uintptr_t exec_end = 0;
    for (int i = 0; i < m->nsegs; i++) {
        const struct so_seg *s = &m->segs[i];
        if (s->exec) {
            if (so_code_protect((void *)s->addr, s->size, 1) != 0)
                return -1;
            if (s->addr + s->size > exec_end)
                exec_end = s->addr + s->size;
        }
    }
    for (int i = 0; i < m->nsegs; i++) {
        const struct so_seg *s = &m->segs[i];
        if (s->exec)
            continue;
        uintptr_t start = s->addr < exec_end ? exec_end : s->addr;
        uintptr_t stop = s->addr + s->size;
        if (stop > start && so_code_protect((void *)start, stop - start, 0) != 0)
            return -1;
    }
    return 0;
}

static void sqlite_log(void *unused, int code, const char *message)
{
    (void)unused;
    fprintf(stderr, "sqlite: error %d: %s\n", code, message);
}

void so_run_init(so_module *m)
{
    if (m->init)
        m->init();
    void (**array)(void) = (void (**)(void))(m->base + m->init_array_offset);
    for (size_t i = 0; i < m->init_count; i++) {
        void (*fn)(void) = array[i];
        if (fn && fn != (void (*)(void))(intptr_t)-1)
            fn();
    }
    // libbedrock's SQLite reports its failures nowhere else.  This must run before its first use.
    int (*sqlite_config)(int, ...) = (int (*)(int, ...))so_symbol(m, "sqlite3_config");
    if (sqlite_config)
        sqlite_config(16 /* SQLITE_CONFIG_LOG */, sqlite_log, NULL);
}

// Renders an address as library+offset, which is what disassembly can be searched for.
const char *so_describe(uintptr_t addr, char *out, size_t size)
{
    for (so_module *m = g_modules; m; m = m->next) {
        if (addr >= m->base && addr < m->base + m->span) {
            snprintf(out, size, "%s+0x%x", m->name, (unsigned)(addr - m->base));
            return out;
        }
    }
    snprintf(out, size, "0x%x", (unsigned)addr);
    return out;
}

// ---- dlfcn ----------------------------------------------------------------

#define RTLD_DEFAULT_HANDLE ((void *)-1) // bionic 32-bit value
#define HOST_HANDLE ((void *)-3)

static const char *g_dlerror;

// Mono probes names such as "bedrock", "libbedrock.so" and "./bedrock.so".
static void canonical_name(const char *in, char *out, size_t size)
{
    const char *base = strrchr(in, '/');
    base = base ? base + 1 : in;
    size_t len = strlen(base);
    int has_so = len > 3 && strcmp(base + len - 3, ".so") == 0;
    snprintf(out, size, "%s%.*s.so", strncmp(base, "lib", 3) == 0 ? "" : "lib",
             (int)(has_so ? len - 3 : len), base);
}

// Mono loads precompiled code from "<assembly path>.so", a guest path.
static void *load_aot_module(const char *guest)
{
    const char *base = strrchr(guest, '/');
    base = base ? base + 1 : guest;
    so_module *m = so_find(base);
    if (m)
        return m;

    // Unity addresses assemblies inside the zip, where no real .so can live, so the
    // precompiled libraries are looked up by name in one folder.
    char aot_path[VFS_PATH_MAX], host[VFS_PATH_MAX];
    snprintf(aot_path, sizeof aot_path, VFS_GUEST_DATA "/managed/%s", base);
    if (vfs_resolve(aot_path, host, sizeof host, 1) != 0) {
        g_dlerror = "AOT module not found";
        return NULL;
    }
    m = so_load(base, host);
    if (!m || so_relocate(m) != 0 || so_finalize(m) != 0) {
        g_dlerror = "AOT module failed to load";
        return NULL;
    }
    so_run_init(m);
    return m;
}

void *b_dlopen(const char *name, int flags)
{
    (void)flags;
    if (!name)
        return RTLD_DEFAULT_HANDLE;

    size_t name_len = strlen(name);
    if (name_len > 7 && strcmp(name + name_len - 7, ".dll.so") == 0)
        return load_aot_module(name);

    char canon[64];
    canonical_name(name, canon, sizeof canon);
    so_module *m = so_find(canon);
    if (m)
        return m;

    static const char *const system_libs[] = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so",
                                              "libEGL.so", "libGLESv2.so", "libGLESv1_CM.so", "libandroid.so",
                                              "libstdc++.so"};
    for (size_t i = 0; i < sizeof system_libs / sizeof *system_libs; i++)
        if (strcmp(canon, system_libs[i]) == 0)
            return HOST_HANDLE;

    char path[320];
    snprintf(path, sizeof path, "%s/%s", g_lib_dir, canon);
    FILE *probe = fopen(path, "rb");
    if (!probe) {
        g_dlerror = "library not found";
        return NULL;
    }
    fclose(probe);
    m = so_load(canon, path);
    if (!m || so_relocate(m) != 0) {
        g_dlerror = "library failed to load";
        return NULL;
    }
    so_patch_kuser(m);
    so_finalize(m);
    so_run_init(m);
    return m;
}

void *b_dlsym(void *handle, const char *name)
{
    void *p = NULL;
    if (handle == RTLD_DEFAULT_HANDLE || handle == HOST_HANDLE) {
        p = so_import_lookup(name);
        for (so_module *m = g_modules; !p && handle == RTLD_DEFAULT_HANDLE && m; m = m->next)
            p = so_symbol(m, name);
    } else if (handle) {
        p = so_symbol(handle, name);
    }
    if (!p)
        g_dlerror = "symbol not found";
    return p;
}

int b_dlclose(void *handle)
{
    (void)handle; // libraries stay mapped for the life of the process
    return 0;
}

const char *b_dlerror(void)
{
    const char *e = g_dlerror;
    g_dlerror = NULL;
    return e;
}

uintptr_t b_unwind_find_exidx(uintptr_t pc, int *count)
{
    for (so_module *m = g_modules; m; m = m->next) {
        if (pc >= m->base && pc < m->base + m->span) {
            *count = (int)m->exidx_count;
            return m->exidx;
        }
    }
    *count = 0;
    return 0;
}
