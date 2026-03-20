#pragma once
#include <stdint.h>
#include <stddef.h>

// Minimal JNI type definitions (no jni.h from NDK needed)
typedef void*    jobject;
typedef jobject  jclass;
typedef jobject  jstring;
typedef jobject  jarray;
typedef jobject  jbyteArray;
typedef jobject  jlongArray;
typedef jobject  jintArray;
typedef jobject  jfloatArray;
typedef jobject  jbooleanArray;
typedef jobject  jthrowable;
typedef int8_t   jbyte;
typedef int16_t  jshort;
typedef int32_t  jint;
typedef int64_t  jlong;
typedef float    jfloat;
typedef double   jdouble;
typedef uint8_t  jboolean;
typedef uint16_t jchar;
typedef jint     jsize;
typedef void*    jmethodID;
typedef void*    jfieldID;
typedef void*    jweak;

#define JNI_TRUE  1
#define JNI_FALSE 0
#define JNI_OK    0
#define JNI_ERR  -1

typedef union {
    jboolean z; jbyte b; jchar c; jshort s;
    jint i; jlong j; jfloat f; jdouble d; jobject l;
} jvalue;

typedef struct JNINativeInterface_ JNINativeInterface_;
typedef const JNINativeInterface_ *JNIEnv;
typedef struct JavaVM_            JavaVM_;
typedef const struct JNIInvokeInterface_ *JavaVM;

// Create a fake JNIEnv suitable for calling HMM JNI functions.
JNIEnv *jni_env_create(void);
JavaVM  *jni_vm_get(void);
void     jni_env_destroy(JNIEnv *env);

// Helper: unwrap a jstring back to C string (works with our fake strings)
const char *jni_get_string(jstring s);
jstring     jni_new_string(JNIEnv *env, const char *s);

// JNI call helpers (avoid needing struct definition)
jstring     jni_NewStringUTF(JNIEnv *e, const char *s);
jbyteArray  jni_NewByteArray(JNIEnv *e, jsize n);
jlongArray  jni_NewLongArray(JNIEnv *e, jsize n);
void        jni_SetByteArrayRegion(JNIEnv *e, jbyteArray a, jsize start, jsize len, const jbyte *buf);
jlong      *jni_GetLongArrayElements(JNIEnv *e, jlongArray a);
void        jni_ReleaseLongArrayElements(JNIEnv *e, jlongArray a, jlong *elems);

// Lookup a native method registered via RegisterNatives by name substring.
// Returns the function pointer, or NULL if not found.
void       *jni_find_registered_native(const char *name_substring);
void       *jni_find_registered_native_exact(const char *name);
void       *jni_find_registered_native_by_sig(const char *name, const char *sig_substring);

// Fake ScoredInput object for nativeAppend
jobject     jni_create_scored_input(JNIEnv *env, const char *input, float score);
void        jni_scored_input_set_vertices(jobject si, int start, int end);

// Object array helpers (also available via JNI table)
jobject     jni_NewObjectArray(JNIEnv *e, jsize n, jclass c, jobject init);
void        jni_SetObjectArrayElement(JNIEnv *e, jobject arr, jsize i, jobject val);

// Fake FileDescriptor for nativeEnrollDataFd
jobject     jni_create_file_descriptor(JNIEnv *env, int fd);

// Fake Range object for fillCandidateList etc.
jobject     jni_create_range(JNIEnv *env, int start, int end);
