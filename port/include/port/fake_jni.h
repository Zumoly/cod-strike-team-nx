// A stand-in for the Dalvik VM: libunity and the managed side reach Java through JNI,
// and no Java runs here.  Classes, methods and strings are small host records.
#ifndef PORT_FAKE_JNI_H
#define PORT_FAKE_JNI_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

void jni_init(void); // before the first JNI_OnLoad
void *jni_vm(void);  // JavaVM*
void *jni_env(void); // JNIEnv*

void *jni_string(const char *text);
void *jni_object(const char *class_name);

// Entry registered through RegisterNatives, or NULL.
void *jni_native(const char *name);
void *jni_direct_buffer(void *data, size_t size);
int jni_take_resize_request(void); // nonzero once for each triggerResizeCall

// Runtime values the Java layer would have supplied.
const char *jni_package_code_path(void);
const char *jni_obb_path(void);
const char *jni_data_dir(void);

#endif
