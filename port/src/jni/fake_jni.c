#include "port/android_input.h"
#include "port/fake_jni.h"
#include "port/gles_backend.h"
#include "port/so_loader.h"
#include "port/vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006
#define MEDIA_STATE_END 2 // EasyMovieTexture MEDIAPLAYER_STATE
#define MOGA_ACTION_UP 1  // com.bda.controller.ControllerListener: ACTION_DOWN is 0
#define SEEN_BITS 65536
#define DISPLAY_WIDTH 1280
#define DISPLAY_HEIGHT 720
#define DISPLAY_DPI 237.0f // the Switch's 6.2-inch panel

enum { OBJ_CLASS, OBJ_STRING, OBJ_INSTANCE, OBJ_ARRAY };

struct jobj {
    int kind;
    const char *name; // class name, or string text
    int length;       // arrays
    void *data;
};

struct jmethod {
    const char *cls, *name, *sig;
    int is_static;
};

extern char jni_thunks[]; // src/jni/jni_thunks.S
static void *g_env_table[240];
static void *g_vm_table[8];
static void **g_env = g_env_table;
static void **g_vm = g_vm_table;

#define MAX_NATIVES 128
static struct {
    const char *name, *sig;
    void *fn;
} g_natives[MAX_NATIVES];
static int g_native_count;

const char *jni_package_code_path(void) { return VFS_GUEST_APK; }
const char *jni_obb_path(void) { return VFS_GUEST_OBB; }
const char *jni_data_dir(void) { return VFS_GUEST_DATA; }

void *jni_vm(void) { return &g_vm; }
void *jni_env(void) { return &g_env; }

void *jni_string(const char *text)
{
    struct jobj *o = calloc(1, sizeof *o);
    o->kind = OBJ_STRING;
    o->name = strdup(text);
    return o;
}

void *jni_object(const char *class_name)
{
    struct jobj *o = calloc(1, sizeof *o);
    o->kind = OBJ_INSTANCE;
    o->name = class_name;
    return o;
}

static int g_resize_requested;

int jni_take_resize_request(void)
{
    int r = g_resize_requested;
    g_resize_requested = 0;
    return r;
}

// java.nio.ByteBuffer.allocateDirect: native code reads its address back through slot 230.
void *jni_direct_buffer(void *data, size_t size)
{
    struct jobj *o = jni_object("java/nio/ByteBuffer");
    o->data = data;
    o->length = (int)size;
    return o;
}

void *jni_native(const char *name)
{
    for (int i = 0; i < g_native_count; i++)
        if (strcmp(g_natives[i].name, name) == 0)
            return g_natives[i].fn;
    return NULL;
}

void jni_missing(unsigned slot)
{
    fprintf(stderr, "jni: unimplemented JNIEnv slot %u, called from %p\n", slot, __builtin_return_address(0));
    fflush(stderr);
    abort();
}

static struct jobj *make_obj(int kind, const char *name)
{
    struct jobj *o = calloc(1, sizeof *o);
    o->kind = kind;
    o->name = name;
    return o;
}

// Objects the fake Java layer hands out are never freed (DeleteLocalRef is a no-op), and Unity
// polls the controller through it every frame: getDevice, getName, getMotionRange, GetMethodID,
// GetObjectClass...  Each call made new objects and strdup'd names, ~360 a second; after 14 minutes
// the heap was full and returning to the main menu ran out (run 17).  Classes, strings, stateless
// instances and method ids are now made once per distinct value and shared.  Java strings are
// immutable and these instances keep no state, so sharing them changes nothing the game can see.
#define SHARED_SLOTS 4096
static struct shared {
    uint32_t hash;
    int kind;
    int length;
    const void *data;
    void *value; // struct jobj or struct jmethod
} g_shared[SHARED_SLOTS];
static uint32_t g_shared_lock;

static void shared_lock(void)
{
    while (__atomic_exchange_n(&g_shared_lock, 1, __ATOMIC_ACQUIRE))
        ;
}

static void shared_unlock(void) { __atomic_store_n(&g_shared_lock, 0, __ATOMIC_RELEASE); }

static uint32_t hash_str(uint32_t h, const char *s)
{
    for (; s && *s; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return (h ^ 0xFF) * 16777619u; // separates consecutive strings
}

static int same_str(const char *a, const char *b) { return a == b || (a && b && strcmp(a, b) == 0); }

// kind -1 marks a method id: name/sig/cls in the jmethod.
static struct jobj *shared_obj(int kind, const char *name, int length, const void *data)
{
    uint32_t h = hash_str(2166136261u + (uint32_t)kind * 31u + (uint32_t)length, name) ^ (uint32_t)(uintptr_t)data;
    shared_lock();
    unsigned i = h % SHARED_SLOTS;
    for (int n = 0; n < SHARED_SLOTS; n++, i = (i + 1) % SHARED_SLOTS) {
        struct shared *s = &g_shared[i];
        if (!s->value) {
            struct jobj *o = make_obj(kind, strdup(name ? name : ""));
            o->length = length;
            o->data = (void *)data;
            s->hash = h;
            s->kind = kind;
            s->length = length;
            s->data = data;
            s->value = o;
            shared_unlock();
            return o;
        }
        struct jobj *o = s->value;
        if (s->hash == h && s->kind == kind && s->length == length && s->data == data && same_str(o->name, name)) {
            shared_unlock();
            return o;
        }
    }
    shared_unlock();
    struct jobj *o = make_obj(kind, strdup(name ? name : "")); // table full: as before
    o->length = length;
    o->data = (void *)data;
    return o;
}

// ---- JNIEnv -----------------------------------------------------------------

static int j_GetVersion(void *e) { (void)e; return JNI_VERSION_1_6; }

static void *j_FindClass(void *e, const char *name)
{
    (void)e;
    fprintf(stderr, "jni: FindClass %s\n", name);
    return shared_obj(OBJ_CLASS, name, 0, NULL);
}

static void *j_null(void *e) { (void)e; return NULL; }
static void j_void(void *e) { (void)e; }
static int j_zero(void *e) { (void)e; return 0; }
static void *j_self(void *e, void *o) { (void)e; return o; }
static void j_drop(void *e, void *o) { (void)e; (void)o; }

static int j_IsSameObject(void *e, void *a, void *b) { (void)e; return a == b; }

static void *j_GetObjectClass(void *e, struct jobj *o)
{
    (void)e;
    return shared_obj(OBJ_CLASS, o && o->name ? o->name : "java/lang/Object", 0, NULL);
}

static void *make_method(struct jobj *cls, const char *name, const char *sig, int is_static)
{
    const char *cname = cls ? cls->name : "?";
    uint32_t h = hash_str(hash_str(hash_str(0x9E3779B9u + (uint32_t)is_static, cname), name), sig);
    shared_lock();
    unsigned i = h % SHARED_SLOTS;
    for (int n = 0; n < SHARED_SLOTS; n++, i = (i + 1) % SHARED_SLOTS) {
        struct shared *s = &g_shared[i];
        if (s->value && s->kind == -1 && s->hash == h) {
            struct jmethod *m = s->value;
            if (m->is_static == is_static && same_str(m->cls, cname) && same_str(m->name, name) &&
                same_str(m->sig, sig)) {
                shared_unlock();
                return m;
            }
            continue;
        }
        if (!s->value) {
            struct jmethod *m = calloc(1, sizeof *m);
            m->cls = strdup(cname);
            m->name = strdup(name);
            m->sig = strdup(sig);
            m->is_static = is_static;
            s->hash = h;
            s->kind = -1;
            s->value = m;
            shared_unlock();
            return m;
        }
    }
    shared_unlock();
    struct jmethod *m = calloc(1, sizeof *m);
    m->cls = cname;
    m->name = strdup(name);
    m->sig = strdup(sig);
    m->is_static = is_static;
    return m;
}

// True the first time a method is seen.  A hash collision only loses one log line.
static int first_sighting(const struct jmethod *m)
{
    static uint32_t seen[SEEN_BITS / 32];
    uint32_t hash = 2166136261u;
    const char *const parts[3] = {m->cls, m->name, m->sig};
    for (int i = 0; i < 3; i++)
        for (const char *c = parts[i]; *c; c++)
            hash = (hash ^ (uint8_t)*c) * 16777619u;
    uint32_t bit = hash % SEEN_BITS, mask = 1u << (bit % 32);
    return !(__atomic_fetch_or(&seen[bit / 32], mask, __ATOMIC_RELAXED) & mask);
}

static void *j_GetMethodID(void *e, struct jobj *c, const char *n, const char *s)
{
    (void)e;
    return make_method(c, n, s, 0);
}

static void *j_GetStaticMethodID(void *e, struct jobj *c, const char *n, const char *s)
{
    (void)e;
    return make_method(c, n, s, 1);
}

// Answers the Java layer would have given.  Everything else falls to the signature.
static const struct {
    const char *method;
    const char *value;
} g_strings[] = {
    {"getPackageName", VFS_PACKAGE},
    {"getPackageCodePath", VFS_GUEST_APK},
    {"getAbsolutePath", VFS_GUEST_DATA "/files"},
    {"getPath", VFS_GUEST_DATA "/files"},
    {"getCanonicalPath", VFS_GUEST_DATA "/files"},
    {"getLanguage", "en"},
    {"getCountry", "US"},
    {"getDisplayLanguage", "English"},
    {"getExternalStorageState", "mounted"},
    {"getProperty", ""},
    {"getExternalAppStorageDirectory", VFS_GUEST_EXT "/files"},
    {"getAppTempStorageDirectory", VFS_GUEST_DATA "/cache"},
    {"getLanguageSetting", "en"},
    {"getAndroidVersion", "4.0.3"},
    {"getDeviceName", "Nintendo Switch"},
    {"getDeviceCellularCarrierName", ""},
    {"getWifiIPAddress", "0.0.0.0"},
    {"getWifiMacAddress", "02:00:00:00:00:00"},
    {"getInstallerPackageName", ""},
    {"getCookieValue", ""},
};

// Declared return types say only "Object" when C# calls through reflection, so results
// that later calls depend on are typed by member name.
static const struct {
    const char *member;
    const char *cls;
} g_returns[] = {
    {"currentActivity", "android/app/Activity"},
    {"getPackageManager", "android/content/pm/PackageManager"},
    {"getPackageInfo", "android/content/pm/PackageInfo"},
    {"getApplicationContext", "android/content/Context"},
    {"getIntent", "android/content/Intent"},
};

static const struct {
    const char *member;
    const char *path;
} g_files[] = {
    {"getExternalStorageDirectory", "/mnt/sdcard"},
    {"getFilesDir", VFS_GUEST_DATA "/files"},
    {"getCacheDir", VFS_GUEST_DATA "/cache"},
    {"getExternalFilesDir", VFS_GUEST_EXT "/files"},
    {"getExternalCacheDir", VFS_GUEST_EXT "/cache"},
    {"getObbDir", "/mnt/sdcard/Android/obb/" VFS_PACKAGE},
};

static uint64_t ref(void *o)
{
    return (uint32_t)(uintptr_t)o;
}

// The controller Unity is shown.  The axes are chosen for Unity's numbering: with no AXIS_RX on
// the device, X Y Z RZ HAT_X HAT_Y become 0 to 5, and the game reads 4 and 5 as its triggers.
// src/jni/android_input.c puts them there.  D-pad presses arrive as keys.
#define PAD_NAME "Sony Interactive Entertainment Wireless Controller"
#define SOURCE_JOYSTICK 0x01000010
#define PAD_SOURCES 0x01000411
static const int g_pad_axes[] = {0, 1, 11, 14, 15, 16};
#define PAD_AXIS_COUNT ((int)(sizeof g_pad_axes / sizeof *g_pad_axes))

static uint32_t float_bits(float f)
{
    uint32_t v;
    memcpy(&v, &f, sizeof v);
    return v;
}

static void *motion_range(int axis)
{
    return shared_obj(OBJ_INSTANCE, "android/view/InputDevice$MotionRange", axis, NULL);
}

// Returns 1 when the call belonged to the input classes.
static int answer_input(const struct jmethod *m, struct jobj *self, const uint64_t *args, uint64_t *out)
{
    const char *n = m->name;
    if (strcmp(m->cls, "android/view/InputDevice") == 0) {
        if (strcmp(n, "getDeviceIds") == 0) {
            static struct jobj *ids;
            if (!ids) {
                ids = calloc(1, sizeof *ids);
                ids->kind = OBJ_ARRAY;
                ids->length = 1;
                ids->data = calloc(1, sizeof(int));
                *(int *)ids->data = INPUT_DEVICE_ID;
            }
            *out = ref(ids);
        } else if (strcmp(n, "getDevice") == 0) {
            *out = args && (int)args[0] == INPUT_DEVICE_ID
                       ? ref(shared_obj(OBJ_INSTANCE, "android/view/InputDevice", 0, NULL))
                       : 0;
        } else if (strcmp(n, "getName") == 0) {
            *out = ref(shared_obj(OBJ_STRING, PAD_NAME, 0, NULL));
        } else if (strcmp(n, "getDescriptor") == 0) {
            *out = ref(shared_obj(OBJ_STRING, "switch-pad-1", 0, NULL));
        } else if (strcmp(n, "getSources") == 0) {
            *out = PAD_SOURCES;
        } else if (strcmp(n, "getId") == 0) {
            *out = INPUT_DEVICE_ID;
        } else if (strcmp(n, "getMotionRange") == 0) {
            *out = 0;
            for (int i = 0; args && i < PAD_AXIS_COUNT; i++)
                if (g_pad_axes[i] == (int)args[0])
                    *out = ref(motion_range(g_pad_axes[i]));
        } else if (strcmp(n, "getMotionRanges") == 0) {
            *out = ref(shared_obj(OBJ_INSTANCE, "java/util/List", 0, NULL));
        } else {
            return 0;
        }
        return 1;
    }
    if (strcmp(m->cls, "android/view/InputDevice$MotionRange") == 0) {
        if (strcmp(n, "getAxis") == 0)
            *out = self ? (uint32_t)self->length : 0;
        else if (strcmp(n, "getSource") == 0)
            *out = SOURCE_JOYSTICK;
        else if (strcmp(n, "getMin") == 0)
            *out = float_bits(-1.0f);
        else if (strcmp(n, "getMax") == 0)
            *out = float_bits(1.0f);
        else if (strcmp(n, "getRange") == 0)
            *out = float_bits(2.0f);
        else if (strcmp(n, "getFlat") == 0 || strcmp(n, "getFuzz") == 0)
            *out = float_bits(0.0f);
        else
            return 0;
        return 1;
    }
    if (strcmp(n, "iterator") == 0 && strcmp(m->cls, "java/util/List") == 0) {
        // One iterator, restarted by each iterator() call: Unity walks the list in one go.
        struct jobj *it = shared_obj(OBJ_INSTANCE, "java/util/Iterator", 0, NULL);
        it->length = 0;
        *out = ref(it);
        return 1;
    }
    if (strcmp(m->cls, "java/util/Iterator") == 0 && self) {
        if (strcmp(n, "hasNext") == 0) {
            *out = self->length < PAD_AXIS_COUNT;
            return 1;
        }
        if (strcmp(n, "next") == 0) {
            *out = self->length < PAD_AXIS_COUNT ? ref(motion_range(g_pad_axes[self->length++])) : 0;
            return 1;
        }
    }
    return 0;
}

// Every Java call and field read passes through here, so the log lists what Unity needs.
// args is a jvalue array when the caller used one, otherwise NULL.
static uint64_t answer(const char *kind, const struct jmethod *m, struct jobj *self, const uint64_t *args)
{
    // Video playback is skipped for now: reporting END moves the game past each clip.
    if (strcmp(m->name, "GetStatus") == 0 && strstr(m->cls, "EasyMovieTexture"))
        return MEDIA_STATE_END;

    // The MOGA SDK answers ACTION_DOWN, which is 0, for a held button.  The default answer of 0
    // would mean every button of a MOGA pad is held, and the game believes that over Unity.
    if (strcmp(m->name, "getKeyCode") == 0)
        return MOGA_ACTION_UP;

    // TBFUtils.IsHighMemoryAndroidDevice wants more than 1700 MB; with 0 the game first classes the
    // console as AndroidLow and makes start-up choices for a low-end phone.  MB, as Java's reader gives.
    if (strcmp(m->name, "readTotalRam") == 0)
        return 3584;

    // Once each: many of these are polled every frame, and writing to the SD card stalls the game.
    if (first_sighting(m))
        fprintf(stderr, "jni: %s %s.%s %s\n", kind, m->cls, m->name, m->sig);
    const char *ret = strrchr(m->sig, ')');
    ret = ret ? ret + 1 : m->sig;

    uint64_t input_result;
    if (answer_input(m, self, args, &input_result))
        return input_result;

    // Java answers this later, from its own thread, with nativeResize.  Unity ignores a size
    // given any earlier, and its screen stays at a default of 128x128.
    if (strcmp(m->name, "triggerResizeCall") == 0) {
        g_resize_requested = 1;
        return 0;
    }

    // Android resizes the surface and scales it up, and Unity has already adopted the size.
    if (strcmp(m->name, "setScreenSize") == 0 && args) {
        fprintf(stderr, "jni: game screen size %dx%d\n", (int)args[0], (int)args[1]);
        // -1 means "keep the current size".
        if ((int)args[0] > 0 && (int)args[1] > 0) {
            gb_set_screen_size((int)args[0], (int)args[1]);
            input_set_screen_size((int)args[0], (int)args[1]);
        }
        return 0;
    }

    // Android runs a library's JNI_OnLoad here, which is where it keeps the JavaVM.
    if (strcmp(m->name, "loadLibrary") == 0 && args) {
        const struct jobj *wanted = (const struct jobj *)(uintptr_t)(uint32_t)args[0];
        void *library = wanted ? b_dlopen(wanted->name, 0) : NULL;
        int (*on_load)(void *, void *) = library ? (int (*)(void *, void *))b_dlsym(library, "JNI_OnLoad") : NULL;
        fprintf(stderr, "jni: loadLibrary %s: handle %p, JNI_OnLoad %p\n", wanted ? wanted->name : "?", library,
                (void *)on_load);
        if (on_load)
            on_load(jni_vm(), NULL);
        return 0;
    }

    // Nothing is persisted through SharedPreferences, so each lookup finds its default.
    if (args && (strcmp(m->name, "getString") == 0 || strcmp(m->name, "getInt") == 0 ||
                 strcmp(m->name, "getBoolean") == 0 || strcmp(m->name, "getLong") == 0))
        return args[1];

    if (strcmp(m->name, "forName") == 0 && args) {
        const struct jobj *wanted = (const struct jobj *)(uintptr_t)(uint32_t)args[0];
        char *name = strdup(wanted ? wanted->name : "java/lang/Object");
        for (char *c = name; *c; c++)
            if (*c == '.')
                *c = '/';
        struct jobj *cls = shared_obj(OBJ_CLASS, name, 0, NULL);
        free(name);
        return ref(cls);
    }
    if (strcmp(m->name, "getClass") == 0)
        return ref(shared_obj(OBJ_CLASS, self && self->name ? self->name : "java/lang/Object", 0, NULL));
    if (strcmp(m->name, "getName") == 0 && self && self->name) {
        char *name = strdup(self->name);
        for (char *c = name; *c; c++)
            if (*c == '/')
                *c = '.';
        struct jobj *s = shared_obj(OBJ_STRING, name, 0, NULL);
        free(name);
        return ref(s);
    }
    for (size_t i = 0; i < sizeof g_files / sizeof *g_files; i++)
        if (strcmp(m->name, g_files[i].member) == 0)
            return ref(shared_obj(OBJ_INSTANCE, "java/io/File", 0, g_files[i].path));
    if ((strcmp(m->name, "getPath") == 0 || strcmp(m->name, "getAbsolutePath") == 0 ||
         strcmp(m->name, "getCanonicalPath") == 0) && self && self->data)
        return ref(shared_obj(OBJ_STRING, self->data, 0, NULL));
    for (size_t i = 0; i < sizeof g_returns / sizeof *g_returns; i++)
        if (strcmp(m->name, g_returns[i].member) == 0)
            return ref(shared_obj(OBJ_INSTANCE, g_returns[i].cls, 0, NULL));

    if (strcmp(ret, "Ljava/lang/String;") == 0) {
        for (size_t i = 0; i < sizeof g_strings / sizeof *g_strings; i++)
            if (strcmp(m->name, g_strings[i].method) == 0)
                return ref(shared_obj(OBJ_STRING, g_strings[i].value, 0, NULL));
        return ref(shared_obj(OBJ_STRING, strcmp(m->name, "versionName") == 0 ? "1.0.40" : "switch", 0, NULL));
    }
    if (ret[0] == 'L') {
        // Keeps chained calls such as getContext().getResources() going.
        char *cls = strdup(ret + 1);
        cls[strlen(cls) - 1] = '\0';
        struct jobj *o = shared_obj(OBJ_INSTANCE, cls, 0, NULL);
        free(cls);
        return ref(o);
    }
    // Left at zero, these make Unity fall back to a 128x128 screen.
    if (strcmp(m->name, "getWidth") == 0 || strcmp(m->name, "getRawWidth") == 0 ||
        strcmp(m->name, "widthPixels") == 0)
        return DISPLAY_WIDTH;
    if (strcmp(m->name, "getHeight") == 0 || strcmp(m->name, "getRawHeight") == 0 ||
        strcmp(m->name, "heightPixels") == 0)
        return DISPLAY_HEIGHT;
    if (strcmp(m->name, "xdpi") == 0 || strcmp(m->name, "ydpi") == 0)
        return float_bits(DISPLAY_DPI);
    if (strcmp(m->name, "SDK_INT") == 0)
        return 14;
    if (strcmp(m->name, "versionCode") == 0)
        return 15; // must match the main.15 OBB name
    return 0;
}

// The varargs forms carry the same arguments as a jvalue array.  Only the first two are
// ever read: forName and loadLibrary take an object, and preference getters a default.
static uint64_t call_with_list(struct jmethod *m, void *o, va_list a)
{
    uint64_t args[2] = {0, 0};
    if (strncmp(m->sig, "(I", 2) == 0 && (strstr(m->cls, "InputDevice") || strcmp(m->name, "setScreenSize") == 0)) {
        args[0] = (uint32_t)va_arg(a, uint32_t);
        if (m->sig[2] == 'I')
            args[1] = (uint32_t)va_arg(a, uint32_t);
        return answer("call", m, o, args);
    }
    if (strncmp(m->sig, "(L", 2) == 0) {
        args[0] = (uint32_t)(uintptr_t)va_arg(a, void *);
        const char *next = strchr(m->sig, 59); // past the first ";"
        if (next && (next[1] == 76 || next[1] == 73 || next[1] == 90)) // L, I or Z: one word
            args[1] = (uint32_t)va_arg(a, uint32_t);
        return answer("call", m, o, args);
    }
    return answer("call", m, o, NULL);
}

static uint64_t j_call(void *e, void *o, struct jmethod *m, ...)
{
    (void)e;
    va_list a;
    va_start(a, m);
    uint64_t r = call_with_list(m, o, a);
    va_end(a);
    return r;
}

static uint64_t j_callv(void *e, void *o, struct jmethod *m, va_list a)
{
    (void)e;
    return call_with_list(m, o, a);
}

// Called with a jvalue array: 8 bytes per argument, object references in the low word.
// ReflectionHelper is how C# finds Java members, so its lookups must return real ids,
// which come back through FromReflectedMethod/Field (slots 7 and 8).
static uint64_t j_calla(void *e, void *o, struct jmethod *m, const uint64_t *args)
{
    (void)e;
    if (strcmp(m->cls, "com/unity3d/player/ReflectionHelper") == 0 && strncmp(m->name, "get", 3) == 0 && args) {
        struct jobj *cls = (struct jobj *)(uintptr_t)(uint32_t)args[0];
        struct jobj *name = (struct jobj *)(uintptr_t)(uint32_t)args[1];
        int is_ctor = strcmp(m->name, "getConstructorID") == 0;
        struct jobj *sig = is_ctor ? name : (struct jobj *)(uintptr_t)(uint32_t)args[2];
        const char *member = is_ctor ? "<init>" : name ? name->name : "?";
        const char *signature = sig ? sig->name : "()V";
        struct jobj *handle = jni_object("java/lang/reflect/Member");
        handle->data = make_method(cls, member, signature, 0);
        return (uint32_t)(uintptr_t)handle;
    }
    return answer("call", m, o, args);
}

static void *j_FromReflected(void *e, struct jobj *o)
{
    (void)e;
    return o ? o->data : NULL;
}

// The game makes a DisplayMetrics every frame (11,000 log lines and as many leaked objects in a
// five-minute session).  Its fields are answered by name, so one shared object does for all.
// Nothing frees fake objects (DeleteLocalRef is a no-op), so sharing is safe.
static struct jobj *new_instance(const char *cls, const char *sig)
{
    static struct jobj *metrics;
    if (cls && strcmp(cls, "android/util/DisplayMetrics") == 0) {
        if (!metrics) {
            fprintf(stderr, "jni: new %s %s (shared from now on)\n", cls, sig);
            metrics = make_obj(OBJ_INSTANCE, cls);
        }
        return metrics;
    }
    fprintf(stderr, "jni: new %s %s\n", cls ? cls : "?", sig);
    return make_obj(OBJ_INSTANCE, cls ? cls : "?");
}

static void *j_NewObjectA(void *e, struct jobj *c, struct jmethod *m, const uint64_t *a)
{
    (void)e;
    (void)a;
    return new_instance(c ? c->name : NULL, m ? m->sig : "(null method)");
}

static uint64_t j_get_field(void *e, void *o, struct jmethod *f)
{
    (void)e;
    return answer("field", f, o, NULL);
}

static void j_set_field(void *e, void *o, struct jmethod *f)
{
    (void)e;
    (void)o;
    (void)f;
}

static void *j_NewObject(void *e, struct jobj *c, struct jmethod *m, ...)
{
    (void)e;
    return new_instance(c->name, m->sig);
}

static void *j_NewObjectV(void *e, struct jobj *c, struct jmethod *m, va_list a)
{
    (void)e;
    (void)a;
    if (!m) {
        char where[96];
        fprintf(stderr, "jni: new %s with a null method id, from %s\n", c ? c->name : "?",
                so_describe((uintptr_t)__builtin_return_address(0), where, sizeof where));
        return NULL;
    }
    return new_instance(c->name, m->sig);
}

static int j_GetArrayLength(void *e, struct jobj *a)
{
    (void)e;
    return a ? a->length : 0;
}

// One implementation per element width: bool/byte, char/short, int/float, long/double.
static void *new_array(int length, int width)
{
    struct jobj *a = calloc(1, sizeof *a);
    a->kind = OBJ_ARRAY;
    a->length = length;
    a->data = calloc((size_t)(length ? length : 1), (size_t)width);
    return a;
}

static void *j_NewArray1(void *e, int n) { (void)e; return new_array(n, 1); }
static void *j_NewArray2(void *e, int n) { (void)e; return new_array(n, 2); }
static void *j_NewArray4(void *e, int n) { (void)e; return new_array(n, 4); }
static void *j_NewArray8(void *e, int n) { (void)e; return new_array(n, 8); }

static void *j_NewObjectArray(void *e, int n, void *c, void *init)
{
    (void)e;
    (void)c;
    struct jobj *a = new_array(n, sizeof(void *));
    void **slots = a->data;
    for (int i = 0; i < n; i++)
        slots[i] = init;
    return a;
}

// Java would throw for a range outside the array; here it would read or write past the block.
static int region_ok(const struct jobj *a, int at, int n, const char *what)
{
    if (a && a->data && at >= 0 && n >= 0 && (int64_t)at + n <= a->length)
        return 1;
    static int shown;
    if (shown++ < 10)
        fprintf(stderr, "jni: %s %d+%d outside an array of %d: ignored\n", what, at, n, a ? a->length : -1);
    return 0;
}

static void *j_GetObjectArrayElement(void *e, struct jobj *a, int i)
{
    (void)e;
    return region_ok(a, i, 1, "GetObjectArrayElement") ? ((void **)a->data)[i] : NULL;
}

static void j_SetObjectArrayElement(void *e, struct jobj *a, int i, void *v)
{
    (void)e;
    if (region_ok(a, i, 1, "SetObjectArrayElement"))
        ((void **)a->data)[i] = v;
}

static void *j_GetArrayElements(void *e, struct jobj *a, uint8_t *copy)
{
    (void)e;
    if (copy)
        *copy = 0;
    return a->data;
}

static void j_ReleaseArrayElements(void *e, void *a, void *p, int mode)
{
    (void)e;
    (void)a;
    (void)p;
    (void)mode;
}

#define REGION(width)                                                                          \
    static void j_GetRegion##width(void *e, struct jobj *a, int at, int n, void *out)          \
    {                                                                                          \
        (void)e;                                                                               \
        if (region_ok(a, at, n, "Get<Type>ArrayRegion"))                                       \
            memcpy(out, (char *)a->data + at * (width), (size_t)n * (width));                  \
    }                                                                                          \
    static void j_SetRegion##width(void *e, struct jobj *a, int at, int n, const void *in)     \
    {                                                                                          \
        (void)e;                                                                               \
        if (region_ok(a, at, n, "Set<Type>ArrayRegion"))                                       \
            memcpy((char *)a->data + at * (width), in, (size_t)n * (width));                   \
    }

REGION(1)
REGION(2)
REGION(4)
REGION(8)

static int j_IsInstanceOf(void *e, void *o, void *c)
{
    (void)e;
    (void)o;
    (void)c;
    return 1;
}

static void *j_NewStringUTF(void *e, const char *s)
{
    (void)e;
    return s ? shared_obj(OBJ_STRING, s, 0, NULL) : NULL;
}

static int j_GetStringUTFLength(void *e, struct jobj *s)
{
    (void)e;
    return (int)strlen(s->name);
}

static const char *j_GetStringUTFChars(void *e, struct jobj *s, uint8_t *copy)
{
    (void)e;
    if (copy)
        *copy = 0;
    return s->name;
}

static void j_ReleaseStringUTFChars(void *e, void *s, const char *c)
{
    (void)e;
    (void)s;
    (void)c;
}

static int j_RegisterNatives(void *e, struct jobj *c, const void *methods, int n)
{
    (void)e;
    const struct {
        const char *name, *sig;
        void *fn;
    } *m = methods;
    for (int i = 0; i < n && g_native_count < MAX_NATIVES; i++) {
        fprintf(stderr, "jni: native %s.%s %s\n", c ? c->name : "?", m[i].name, m[i].sig);
        g_natives[g_native_count].name = m[i].name;
        g_natives[g_native_count].sig = m[i].sig;
        g_natives[g_native_count++].fn = m[i].fn;
    }
    return JNI_OK;
}

static void *j_GetDirectBufferAddress(void *e, struct jobj *o)
{
    (void)e;
    return o ? o->data : NULL;
}

static int64_t j_GetDirectBufferCapacity(void *e, struct jobj *o)
{
    (void)e;
    return o ? o->length : -1;
}

static int j_GetJavaVM(void *e, void **vm)
{
    (void)e;
    *vm = jni_vm();
    return JNI_OK;
}

// ---- JavaVM -----------------------------------------------------------------

static int v_ok(void *vm) { (void)vm; return JNI_OK; }

static int v_GetEnv(void *vm, void **env, int version)
{
    (void)vm;
    (void)version;
    *env = jni_env();
    return JNI_OK;
}

static int v_Attach(void *vm, void **env, void *args)
{
    (void)vm;
    (void)args;
    *env = jni_env();
    return JNI_OK;
}

#define SET(slot, fn) g_env_table[slot] = (void *)(fn)

// Slot numbers are JNI's, which every JNIEnv* caller is compiled against.
void jni_init(void)
{
    for (int i = 0; i < 240; i++)
        g_env_table[i] = jni_thunks + i * 8;
    for (int i = 0; i < 8; i++)
        g_vm_table[i] = jni_thunks + (232 + i) * 8;

    SET(4, j_GetVersion);
    SET(6, j_FindClass);
    SET(15, j_null); // ExceptionOccurred
    SET(16, j_void); // ExceptionDescribe
    SET(17, j_void); // ExceptionClear
    SET(19, j_zero); // PushLocalFrame
    SET(20, j_self); // PopLocalFrame
    SET(21, j_self); // NewGlobalRef
    SET(22, j_drop); // DeleteGlobalRef
    SET(23, j_drop); // DeleteLocalRef
    SET(24, j_IsSameObject);
    SET(25, j_self); // NewLocalRef
    SET(26, j_zero); // EnsureLocalCapacity
    SET(28, j_NewObject);
    SET(29, j_NewObjectV);
    SET(31, j_GetObjectClass);
    SET(32, j_IsInstanceOf);
    SET(33, j_GetMethodID);
    SET(94, j_GetMethodID); // GetFieldID: same record, typed by its signature
    for (int i = 95; i <= 103; i++)
        SET(i, j_get_field);
    for (int i = 104; i <= 112; i++)
        SET(i, j_set_field);
    SET(144, j_GetStaticMethodID); // GetStaticFieldID
    for (int i = 145; i <= 153; i++)
        SET(i, j_get_field);
    for (int i = 154; i <= 162; i++)
        SET(i, j_set_field);
    SET(171, j_GetArrayLength);
    SET(172, j_NewObjectArray);
    SET(173, j_GetObjectArrayElement);
    SET(174, j_SetObjectArrayElement);
    // Order within each family: boolean byte char short int long float double.
    static void *const news[8] = {j_NewArray1, j_NewArray1, j_NewArray2, j_NewArray2,
                                  j_NewArray4, j_NewArray8, j_NewArray4, j_NewArray8};
    static void *const gets[8] = {j_GetRegion1, j_GetRegion1, j_GetRegion2, j_GetRegion2,
                                  j_GetRegion4, j_GetRegion8, j_GetRegion4, j_GetRegion8};
    static void *const sets[8] = {j_SetRegion1, j_SetRegion1, j_SetRegion2, j_SetRegion2,
                                  j_SetRegion4, j_SetRegion8, j_SetRegion4, j_SetRegion8};
    for (int i = 0; i < 8; i++) {
        SET(175 + i, news[i]);
        SET(183 + i, j_GetArrayElements);
        SET(191 + i, j_ReleaseArrayElements);
        SET(199 + i, gets[i]);
        SET(207 + i, sets[i]);
    }
    SET(222, j_GetArrayElements);     // GetPrimitiveArrayCritical
    SET(223, j_ReleaseArrayElements); // ReleasePrimitiveArrayCritical
    for (int base = 34; base <= 61; base += 3) { // Call<Type>Method, V, A
        SET(base, j_call);
        SET(base + 1, j_callv);
        SET(base + 2, j_calla);
    }
    SET(7, j_FromReflected); // FromReflectedMethod
    SET(8, j_FromReflected); // FromReflectedField
    SET(30, j_NewObjectA);
    SET(113, j_GetStaticMethodID);
    for (int base = 114; base <= 141; base += 3) { // CallStatic<Type>Method, V, A
        SET(base, j_call);
        SET(base + 1, j_callv);
        SET(base + 2, j_calla);
    }
    SET(167, j_NewStringUTF);
    SET(168, j_GetStringUTFLength);
    SET(169, j_GetStringUTFChars);
    SET(170, j_ReleaseStringUTFChars);
    SET(215, j_RegisterNatives);
    SET(217, j_zero); // MonitorEnter
    SET(218, j_zero); // MonitorExit
    SET(219, j_GetJavaVM);
    SET(226, j_self); // NewWeakGlobalRef
    SET(227, j_drop); // DeleteWeakGlobalRef
    SET(228, j_zero); // ExceptionCheck
    SET(230, j_GetDirectBufferAddress);
    SET(231, j_GetDirectBufferCapacity);

    g_vm_table[3] = (void *)v_ok;    // DestroyJavaVM
    g_vm_table[4] = (void *)v_Attach;
    g_vm_table[5] = (void *)v_ok;    // DetachCurrentThread
    g_vm_table[6] = (void *)v_GetEnv;
    g_vm_table[7] = (void *)v_Attach; // AttachCurrentThreadAsDaemon
}
