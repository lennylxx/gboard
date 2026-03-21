// Fake JNIEnv — uses explicit field assignment to avoid struct layout issues.
#include "jni_env.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include <unistd.h>
#include <fcntl.h>
#include <os/log.h>
#if DEBUG
static os_log_t jni_os_log(void) {
    static os_log_t log;
    static int once;
    if (!once) { log = os_log_create(BUNDLE_ID, "jni"); once = 1; }
    return log;
}
#define JLOG(fmt, ...) do { \
    char _b[1024]; snprintf(_b, sizeof(_b), "[JNI] " fmt, ##__VA_ARGS__); \
    os_log_debug(jni_os_log(), "%{public}s", _b); \
} while(0)
#else
#define JLOG(...) ((void)0)
#endif

#define KIND_STRING  0xFE  // unique tag to distinguish from engine-internal objects
#define KIND_BARRAY  2
#define KIND_LARRAY  3
#define KIND_IARRAY  4
#define KIND_FARRAY  5
#define KIND_OBJARRAY 6
#define KIND_SCORED_INPUT 7
#define KIND_FILE_DESCRIPTOR 8
#define KIND_RANGE 9

typedef struct { uint8_t kind; uint32_t len; char data[]; } FakeObj;

static FakeObj *alloc_obj(uint8_t kind, size_t sz) {
    FakeObj *o = calloc(1, sizeof(FakeObj) + sz);
    o->kind = kind; o->len = (uint32_t)sz;
    return o;
}

// ScoredInput fake object: stores a string + float score + vertex indices
typedef struct {
    uint8_t kind;  // = KIND_SCORED_INPUT
    uint32_t len;
    jstring input;
    float score;
    jint startVertexIndex;
    jint endVertexIndex;
} FakeScoredInput;

jobject jni_create_scored_input(JNIEnv *env, const char *input, float score) {
    FakeScoredInput *si = calloc(1, sizeof(FakeScoredInput));
    si->kind = KIND_SCORED_INPUT;
    si->input = jni_new_string(env, input);
    si->score = score;
    si->startVertexIndex = 0;
    si->endVertexIndex = 1;  // default: single char span
    return (jobject)si;
}

void jni_scored_input_set_vertices(jobject si, int start, int end) {
    if (si && ((FakeObj*)si)->kind == KIND_SCORED_INPUT) {
        ((FakeScoredInput*)si)->startVertexIndex = start;
        ((FakeScoredInput*)si)->endVertexIndex = end;
    }
}

// FileDescriptor fake object: stores an int fd
typedef struct {
    uint8_t kind;  // = KIND_FILE_DESCRIPTOR
    uint32_t len;
    int fd;
} FakeFileDescriptor;

jobject jni_create_file_descriptor(JNIEnv *env, int fd) {
    (void)env;
    FakeFileDescriptor *f = calloc(1, sizeof(FakeFileDescriptor));
    f->kind = KIND_FILE_DESCRIPTOR;
    f->fd = fd;
    return (jobject)f;
}

// Range fake object: stores start and end ints
typedef struct {
    uint8_t kind;  // = KIND_RANGE
    uint32_t len;
    int start;
    int end;
} FakeRange;

jobject jni_create_range(JNIEnv *env, int start, int end) {
    (void)env;
    FakeRange *r = calloc(1, sizeof(FakeRange));
    r->kind = KIND_RANGE;
    r->start = start;
    r->end = end;
    return (jobject)r;
}

void jni_get_range(jobject range, int *start, int *end) {
    if (!range) { if (start) *start = 0; if (end) *end = 0; return; }
    FakeRange *r = (FakeRange *)range;
    if (start) *start = r->start;
    if (end)   *end   = r->end;
}

// Forward declarations for object array exports (implemented below)

const char *jni_get_string(jstring s) {
    if (!s) return "";
    FakeObj *o = (FakeObj*)s;
    if (o->kind != KIND_STRING) return "";
    return o->data;
}
jstring jni_new_string(JNIEnv *e, const char *s) {
    (void)e; if (!s) s = "";
    size_t n = strlen(s)+1;
    FakeObj *o = alloc_obj(KIND_STRING, n);
    memcpy(o->data, s, n); return (jstring)o;
}

// All functions use void* parameter types to avoid mismatch errors
// They're accessed through the function table which the engine uses as void*
#define E(t) t

static jint     fn_GetVersion(JNIEnv *e)                        { (void)e; return 0x10006; }
static jclass   fn_FindClass(JNIEnv *e, const char *n)          { (void)e;(void)n; return (jclass)0xDEAD1; }
// Return the name string pointer as methodID so we can identify which method is called later
// Store method IDs as composite "name\tsig" to distinguish overloads
static char s_mid_pool[128][128];
static int s_mid_count = 0;
static jmethodID fn_GetMethodID(JNIEnv *e, jclass c, const char *n, const char *s) {
    (void)e;(void)c;
    { char _b[256]; int _n = snprintf(_b,sizeof(_b),"[JNI] GetMethodID: %s %s\n",n?n:"?",s?s:"?"); write(STDERR_FILENO,_b,_n>0?(size_t)_n:0); }
    // For <init>, encode sig to distinguish String vs Range constructors
    if (n && s && strcmp(n, "<init>") == 0 && s_mid_count < 128) {
        int idx = s_mid_count++;
        snprintf(s_mid_pool[idx], sizeof(s_mid_pool[idx]), "<init>\t%s", s);
        return (jmethodID)s_mid_pool[idx];
    }
    return (jmethodID)n;
}
static jmethodID fn_GetStaticMethodID(JNIEnv *e, jclass c, const char *n, const char *s) {
    (void)e;(void)c;(void)s;
    { char _b[256]; int _n = snprintf(_b,sizeof(_b),"[JNI] GetStaticMethodID: %s %s\n",n?n:"?",s?s:"?"); write(STDERR_FILENO,_b,_n>0?(size_t)_n:0); }
    return (jmethodID)n;
}
// Field IDs: encode the field name pointer as the ID so we can dispatch in Get*Field
static jfieldID fn_GetFieldID(JNIEnv *e, jclass c, const char *n, const char *s) {
    (void)e;(void)c;(void)s;
    { char _b[256]; int _n = snprintf(_b,sizeof(_b),"[JNI] GetFieldID: %s %s\n",n?n:"?",s?s:"?"); write(STDERR_FILENO,_b,_n>0?(size_t)_n:0); }
    return (jfieldID)n;  // return pointer to the name string (in ELF .rodata)
}
static jfieldID fn_GetStaticFieldID(JNIEnv *e, jclass c, const char *n, const char *s) { (void)e;(void)c;(void)n;(void)s; return (jfieldID)(uintptr_t)0xF1E1D; }
static jobject  fn_NewGlobalRef(JNIEnv *e, jobject o)            { (void)e; return o; }
static void     fn_DeleteGlobalRef(JNIEnv *e, jobject o)         { (void)e;(void)o; }
static jobject  fn_NewLocalRef(JNIEnv *e, jobject o)             { (void)e; return o; }
static void     fn_DeleteLocalRef(JNIEnv *e, jobject o)          { (void)e;(void)o; }
static jboolean fn_ExceptionCheck(JNIEnv *e)                     { (void)e; return JNI_FALSE; }
static jthrowable fn_ExceptionOccurred(JNIEnv *e)               { (void)e; return NULL; }
static void     fn_ExceptionClear(JNIEnv *e)                     { (void)e; }
static void     fn_ExceptionDescribe(JNIEnv *e)                  { (void)e; }
static void     fn_FatalError(JNIEnv *e, const char *m)          { (void)e; char _b[256]; int _n=snprintf(_b,sizeof(_b),"[JNI] Fatal: %s\n",m); write(STDERR_FILENO,_b,_n>0?(size_t)_n:0); abort(); }
static jboolean fn_IsSameObject(JNIEnv *e, jobject a, jobject b) { (void)e; return a==b?JNI_TRUE:JNI_FALSE; }
static jboolean fn_IsInstanceOf(JNIEnv *e, jobject o, jclass c)  { (void)e;(void)o;(void)c; return JNI_TRUE; }
static jint     fn_MonitorEnter(JNIEnv *e, jobject o)            { (void)e;(void)o; return 0; }
static jint     fn_MonitorExit(JNIEnv *e, jobject o)             { (void)e;(void)o; return 0; }
// ── RegisterNatives capture ──────────────────────────────────────────────────
typedef struct { const char *name; const char *sig; void *fn; } RegMethod;
#define MAX_REG_METHODS 256
static RegMethod g_reg_methods[MAX_REG_METHODS];
static int g_reg_count = 0;

static jint fn_RegisterNatives(JNIEnv *e, jclass c, const void *m, jint n) {
    (void)e; (void)c;
    // JNINativeMethod layout: { const char *name, const char *signature, void *fnPtr }
    typedef struct { const char *name; const char *sig; void *fn; } JNINativeMethod;
    const JNINativeMethod *methods = (const JNINativeMethod *)m;
    for (jint i = 0; i < n && g_reg_count < MAX_REG_METHODS; i++) {
        g_reg_methods[g_reg_count].name = methods[i].name;
        g_reg_methods[g_reg_count].sig  = methods[i].sig;
        g_reg_methods[g_reg_count].fn   = methods[i].fn;
        { char _b[256]; int _n = snprintf(_b,sizeof(_b),"[JNI] RegisterNatives: %s %s -> %p\n",methods[i].name,methods[i].sig,methods[i].fn); write(STDERR_FILENO,_b,_n>0?(size_t)_n:0); }
        JLOG("REG %s %s → %p", methods[i].name, methods[i].sig, methods[i].fn);
        g_reg_count++;
    }
    return 0;
}

void *jni_find_registered_native(const char *name_substring) {
    for (int i = 0; i < g_reg_count; i++) {
        if (g_reg_methods[i].name && strstr(g_reg_methods[i].name, name_substring))
            return g_reg_methods[i].fn;
    }
    return NULL;
}

void *jni_find_registered_native_exact(const char *name) {
    for (int i = 0; i < g_reg_count; i++) {
        if (g_reg_methods[i].name && strcmp(g_reg_methods[i].name, name) == 0)
            return g_reg_methods[i].fn;
    }
    return NULL;
}

void *jni_find_registered_native_by_sig(const char *name, const char *sig_substring) {
    for (int i = 0; i < g_reg_count; i++) {
        if (g_reg_methods[i].name && strcmp(g_reg_methods[i].name, name) == 0 &&
            g_reg_methods[i].sig && strstr(g_reg_methods[i].sig, sig_substring))
            return g_reg_methods[i].fn;
    }
    return NULL;
}
static jint     fn_UnregisterNatives(JNIEnv *e, jclass c)        { (void)e;(void)c; return 0; }
static jint     fn_PushLocalFrame(JNIEnv *e, jint n)             { (void)e;(void)n; return 0; }
static jobject  fn_PopLocalFrame(JNIEnv *e, jobject o)           { (void)e; return o; }
static jint     fn_EnsureLocalCapacity(JNIEnv *e, jint n)        { (void)e;(void)n; return 0; }

// Strings
// NewString: UTF-16 (jchar*) to UTF-8 string
static jstring fn_NewString(JNIEnv *e, const jchar *chars, jsize len) {
    (void)e;
    // Convert UTF-16 to UTF-8
    size_t utf8_cap = (size_t)len * 3 + 1;
    char *utf8 = malloc(utf8_cap);
    size_t pos = 0;
    for (jsize i = 0; i < len; i++) {
        uint32_t cp = chars[i];
        // Handle surrogate pairs
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
            uint32_t lo = chars[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (cp < 0x80) { utf8[pos++] = (char)cp; }
        else if (cp < 0x800) { utf8[pos++] = (char)(0xC0 | (cp >> 6)); utf8[pos++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { utf8[pos++] = (char)(0xE0 | (cp >> 12)); utf8[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F)); utf8[pos++] = (char)(0x80 | (cp & 0x3F)); }
        else { utf8[pos++] = (char)(0xF0 | (cp >> 18)); utf8[pos++] = (char)(0x80 | ((cp >> 12) & 0x3F)); utf8[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F)); utf8[pos++] = (char)(0x80 | (cp & 0x3F)); }
    }
    utf8[pos] = '\0';
    FakeObj *o = alloc_obj(KIND_STRING, pos + 1);
    memcpy(o->data, utf8, pos + 1);
    o->len = (uint32_t)pos;
    JLOG("NewString: len=%d utf8_len=%zu '%s'", (int)len, pos, utf8);
    free(utf8);
    return (jstring)o;
}
static jstring fn_NewStringUTF(JNIEnv *e, const char *s)         { jstring r=jni_new_string(e,s); JLOG("NewStringUTF: '%s' → %p", s?s:"(null)", r); return r; }
static jsize   fn_GetStringUTFLength(JNIEnv *e, jstring s)       { (void)e; return (jsize)strlen(jni_get_string(s)); }
static const char *fn_GetStringUTFChars(JNIEnv *e, jstring s, jboolean *cp) { (void)e; if(cp)*cp=JNI_FALSE; const char*r=jni_get_string(s); JLOG("GetStringUTFChars: %p → '%s'", s, r?r:"(null)"); return r; }
static void    fn_ReleaseStringUTFChars(JNIEnv *e, jstring s, const char *c) { (void)e;(void)s;(void)c; }
static jsize   fn_GetStringLength(JNIEnv *e, jstring s)          { (void)e; return (jsize)strlen(jni_get_string(s)); }
static const jchar *fn_GetStringChars(JNIEnv *e, jstring s, jboolean *cp) {
    (void)e; if(cp)*cp=JNI_FALSE;
    JLOG("GetStringChars: %p", s);
    // Convert UTF-8 to UTF-16LE for ASCII range
    const char *utf8 = jni_get_string(s);
    size_t len = strlen(utf8);
    jchar *buf = calloc(len + 1, sizeof(jchar));
    for (size_t i = 0; i < len; i++) buf[i] = (jchar)(unsigned char)utf8[i];
    return buf;
}
static void    fn_ReleaseStringChars(JNIEnv *e, jstring s, const jchar *c) { (void)e;(void)s;(void)c; }
static void    fn_GetStringRegion(JNIEnv *e, jstring s, jsize st, jsize l, jchar *b) { (void)e;(void)s;(void)st;(void)l;(void)b; }
static void    fn_GetStringUTFRegion(JNIEnv *e, jstring s, jsize st, jsize l, char *b) { (void)e; if(b) memcpy(b, jni_get_string(s)+st, (size_t)l); }

// Arrays
static jsize   fn_GetArrayLength(JNIEnv *e, jarray a)  { (void)e; jsize r=(jsize)((FakeObj*)a)->len; JLOG("GetArrayLength: %p → %d", a, r); return r; }

static jbyteArray fn_NewByteArray(JNIEnv *e, jsize n)  { (void)e; jbyteArray r=(jbyteArray)alloc_obj(KIND_BARRAY,(size_t)n); JLOG("NewByteArray: size=%d → %p", n, r); return r; }
static jbyte  *fn_GetByteArrayElements(JNIEnv *e, jbyteArray a, jboolean *cp) {
    (void)e; if(cp)*cp=JNI_FALSE;
    FakeObj *o = (FakeObj*)a;
    JLOG("GetByteArrayElements: arr=%p kind=%d len=%u", a, o->kind, o->len);
    return (jbyte*)o->data;
}
static void    fn_ReleaseByteArrayElements(JNIEnv *e, jbyteArray a, jbyte *el, jint m) { (void)e;(void)a;(void)el;(void)m; }
static void    fn_GetByteArrayRegion(JNIEnv *e, jbyteArray a, jsize s, jsize l, jbyte *b) { (void)e; memcpy(b,((FakeObj*)a)->data+s,(size_t)l); }
static void    fn_SetByteArrayRegion(JNIEnv *e, jbyteArray a, jsize s, jsize l, const jbyte *b) { (void)e; memcpy(((FakeObj*)a)->data+s,b,(size_t)l); }

static jlongArray fn_NewLongArray(JNIEnv *e, jsize n)  { (void)e; FakeObj*o=alloc_obj(KIND_LARRAY,(size_t)n*sizeof(jlong)); o->len=(uint32_t)n; return (jlongArray)o; }
static jlong  *fn_GetLongArrayElements(JNIEnv *e, jlongArray a, jboolean *cp) { (void)e;if(cp)*cp=JNI_FALSE; return (jlong*)((FakeObj*)a)->data; }
static void    fn_ReleaseLongArrayElements(JNIEnv *e, jlongArray a, jlong *el, jint m) { (void)e;(void)a;(void)el;(void)m; }
static void    fn_GetLongArrayRegion(JNIEnv *e, jlongArray a, jsize s, jsize l, jlong *b) { (void)e; memcpy(b,(jlong*)((FakeObj*)a)->data+s,(size_t)l*sizeof(jlong)); }
static void    fn_SetLongArrayRegion(JNIEnv *e, jlongArray a, jsize s, jsize l, const jlong *b) { (void)e; memcpy((jlong*)((FakeObj*)a)->data+s,b,(size_t)l*sizeof(jlong)); }

static jintArray fn_NewIntArray(JNIEnv *e, jsize n)    { (void)e; FakeObj*o=alloc_obj(KIND_IARRAY,(size_t)n*sizeof(jint)); o->len=(uint32_t)n; return (jintArray)o; }
static jint   *fn_GetIntArrayElements(JNIEnv *e, jintArray a, jboolean *cp)   { (void)e;if(cp)*cp=JNI_FALSE; return (jint*)((FakeObj*)a)->data; }
static void    fn_ReleaseIntArrayElements(JNIEnv *e, jintArray a, jint *el, jint m) { (void)e;(void)a;(void)el;(void)m; }
static void    fn_GetIntArrayRegion(JNIEnv *e, jintArray a, jsize s, jsize l, jint *b) { (void)e; memcpy(b,(jint*)((FakeObj*)a)->data+s,(size_t)l*sizeof(jint)); }
static void    fn_SetIntArrayRegion(JNIEnv *e, jintArray a, jsize s, jsize l, const jint *b) { (void)e; memcpy((jint*)((FakeObj*)a)->data+s,b,(size_t)l*sizeof(jint)); }

static jfloatArray fn_NewFloatArray(JNIEnv *e, jsize n) { (void)e; FakeObj*o=alloc_obj(KIND_FARRAY,(size_t)n*sizeof(jfloat)); o->len=(uint32_t)n; return (jfloatArray)o; }
static jfloat *fn_GetFloatArrayElements(JNIEnv *e, jfloatArray a, jboolean *cp) { (void)e;if(cp)*cp=JNI_FALSE; return (jfloat*)((FakeObj*)a)->data; }
static void    fn_ReleaseFloatArrayElements(JNIEnv *e, jfloatArray a, jfloat *el, jint m) { (void)e;(void)a;(void)el;(void)m; }

static void    *fn_GetPAC(JNIEnv *e, jarray a, jboolean *cp) { (void)e;if(cp)*cp=JNI_FALSE; JLOG("GetPAC: %p", a); return ((FakeObj*)a)->data; }
static void     fn_RelPAC(JNIEnv *e, jarray a, void *p, jint m) { (void)e;(void)a;(void)p;(void)m; }

// Object arrays — store jobject pointers
static jobject fn_NewObjectArray(JNIEnv *e, jsize n, jclass c, jobject init) {
    (void)e;(void)c;(void)init;
    FakeObj *o = alloc_obj(KIND_OBJARRAY, (size_t)n * sizeof(jobject));
    o->len = (uint32_t)n;
    return (jobject)o;
}
static jobject fn_GetObjectArrayElement(JNIEnv *e, jobject a, jsize i) {
    (void)e;
    FakeObj *o = (FakeObj*)a;
    JLOG("GetObjArrayElem: arr=%p i=%d len=%u", a, i, o->len);
    if (i < 0 || (uint32_t)i >= o->len) return NULL;
    jobject result = ((jobject*)o->data)[i];
    JLOG("GetObjArrayElem → %p", result);
    return result;
}
static void fn_SetObjectArrayElement(JNIEnv *e, jobject a, jsize i, jobject v) {
    (void)e;
    FakeObj *o = (FakeObj*)a;
    if (i >= 0 && (uint32_t)i < o->len)
        ((jobject*)o->data)[i] = v;
}

// Exported wrappers for object arrays
jobject jni_NewObjectArray(JNIEnv *e, jsize n, jclass c, jobject init) { return fn_NewObjectArray(e,n,c,init); }
void    jni_SetObjectArrayElement(JNIEnv *e, jobject arr, jsize i, jobject val) { fn_SetObjectArrayElement(e,arr,i,val); }

// Object / method calls
// Dispatch NewObject based on method signature encoded as "<init>\tsig"
static jobject new_object_dispatch(const char *mid, va_list args) {
    if (!mid || (uintptr_t)mid < 0x10000) return alloc_obj(0, 8);
    // Check for encoded <init> with signature (contains \t)
    const char *sig = strchr(mid, '\t');
    if (sig) {
        sig++; // skip \t
        if (strstr(sig, "([B)V")) {
            // String constructor: new String(byte[])
            jbyteArray ba = va_arg(args, jbyteArray);
            fprintf(stderr, "[JNI-DBG] NewObject String([B)V ba=%p\n", (void*)ba);
            if (ba) {
                // Dump raw bytes at ba to understand format
                uint8_t *raw = (uint8_t*)ba;
                char hex[145]; hex[0]=0;
                for (int h=0; h<48; h++) sprintf(hex+h*3, "%02x ", raw[h]);
                fprintf(stderr, "[JNI-DBG]   ba hex48: %s\n", hex);

                // Check if it's our FakeObj byte array
                FakeObj *arr = (FakeObj*)ba;
                if (arr->kind == 2 || arr->kind == 3) { // KIND_BARRAY or similar
                    size_t n = arr->len;
                    FakeObj *s = alloc_obj(KIND_STRING, n + 1);
                    memcpy(s->data, arr->data, n);
                    s->data[n] = '\0';
                    s->len = (uint32_t)n;
                    return (jobject)s;
                }

                // Engine-internal byte array — try to find string data
                // Could be a raw pointer to UTF-8 data, or a struct with length+data
                // Try treating it as: first 8 bytes = pointer to data, next 8 = length
                // Or: first 4 bytes = length, then data
                uint32_t *w = (uint32_t*)raw;
                uint64_t *q = (uint64_t*)raw;
                fprintf(stderr, "[JNI-DBG]   w[0]=%u w[1]=%u w[2]=%u q[0]=%llu q[1]=%llu\n",
                        w[0], w[1], w[2], (unsigned long long)q[0], (unsigned long long)q[1]);

                // For now, return empty string
                FakeObj *s = alloc_obj(KIND_STRING, 1);
                s->data[0] = '\0';
                return (jobject)s;
            }
            // NULL byte array — return empty string
            FakeObj *s = alloc_obj(KIND_STRING, 1);
            s->data[0] = '\0';
            return (jobject)s;
        }
        if (strstr(sig, "(Ljava/lang/String;)V") || strstr(sig, "()V")) {
            // Other String constructors — allocate empty
            FakeObj *s = alloc_obj(KIND_STRING, 1);
            s->data[0] = '\0';
            return (jobject)s;
        }
        if (strstr(sig, "(II)V")) {
            // Range constructor: new Range(int, int)
            int a1 = va_arg(args, int);
            int a2 = va_arg(args, int);
            FakeRange *r = calloc(1, sizeof(FakeRange));
            r->kind = KIND_RANGE;
            r->start = a1;
            r->end = a2;
            return (jobject)r;
        }
    }
    return alloc_obj(0, 8);
}
static jobject fn_NewObject(JNIEnv *e, jclass c, jmethodID m, ...) {
    (void)e; (void)c;
    va_list ap; va_start(ap, m);
    jobject r = new_object_dispatch((const char *)m, ap);
    va_end(ap);
    return r;
}
// Android ARM64 va_list struct layout:
// { void *__stack, void *__gr_top, void *__vr_top, int __gr_offs, int __vr_offs }
typedef struct {
    void *__stack;
    void *__gr_top;
    void *__vr_top;
    int __gr_offs;
    int __vr_offs;
} android_va_list;

// Extract first pointer arg from Android va_list
static void *android_va_arg_ptr(void *vl_raw) {
    android_va_list *vl = (android_va_list *)vl_raw;
    void *result;
    if (vl->__gr_offs < 0) {
        result = *(void **)((char *)vl->__gr_top + vl->__gr_offs);
        vl->__gr_offs += 8;
    } else {
        result = *(void **)vl->__stack;
        vl->__stack = (char *)vl->__stack + 8;
    }
    return result;
}

static jobject fn_NewObjectV(JNIEnv *e, jclass c, jmethodID m, va_list args) {
    (void)e; (void)c;
    const char *mid = (const char *)m;
    if (!mid || (uintptr_t)mid < 0x10000) return alloc_obj(0, 8);

    const char *sig = strchr(mid, '\t');
    if (sig) {
        sig++;
        if (strstr(sig, "([B)V")) {
            // String(byte[]) constructor — extract byte array from Android va_list
            void *vl_ptr = (void *)args;
            jbyteArray ba = (jbyteArray)android_va_arg_ptr(vl_ptr);
            if (ba) {
                FakeObj *arr = (FakeObj*)ba;
                if (arr->kind == 2) { // KIND_BARRAY
                    size_t n = arr->len;
                    FakeObj *s = alloc_obj(KIND_STRING, n + 1);
                    memcpy(s->data, arr->data, n);
                    s->data[n] = '\0';
                    s->len = (uint32_t)n;
                    return (jobject)s;
                }
            }
            FakeObj *s = alloc_obj(KIND_STRING, 1);
            s->data[0] = '\0';
            return (jobject)s;
        }
        if (strstr(sig, "(II)V")) {
            void *vl_ptr = (void *)args;
            // Extract two ints from Android va_list
            int a1 = (int)(intptr_t)android_va_arg_ptr(vl_ptr);
            int a2 = (int)(intptr_t)android_va_arg_ptr(vl_ptr);
            FakeRange *r = calloc(1, sizeof(FakeRange));
            r->kind = KIND_RANGE;
            r->start = a1;
            r->end = a2;
            return (jobject)r;
        }
    }
    return alloc_obj(0, 8);
}
static jobject fn_CallObjectMethodV(JNIEnv *e, jobject o, jmethodID m, va_list args) {
    (void)args;
    const char *mname = (const char *)m;
    if (mname && (uintptr_t)mname > 0x10000) {
        // Handle getAssets() → return a fake AssetManager object
        if (strcmp(mname, "getAssets") == 0) {
            JLOG("CallObjectMethodV: getAssets → returning fake");
            return alloc_obj(0, 8);  // dummy non-NULL object
        }
        // Handle String.getBytes() → return jbyteArray with UTF-8 bytes
        if (strcmp(mname, "getBytes") == 0 && o) {
            FakeObj *fo = (FakeObj *)o;
            if (fo->kind == KIND_STRING) {
                const char *s = fo->data;
                size_t len = strlen(s);
                jbyteArray ba = jni_NewByteArray(e, (jsize)len);
                jni_SetByteArrayRegion(e, ba, 0, (jsize)len, (const jbyte*)s);
                return (jobject)ba;
            }
        }
    }
    return NULL;
}
static jobject fn_CallObjectMethod(JNIEnv *e, jobject o, jmethodID m, ...) {
    const char *mn = (const char*)m;
    if (mn && (uintptr_t)mn>0x10000) JLOG("CallObjectMethod: obj=%p method=%s", o, mn);
    va_list args;
    va_start(args, m);
    jobject result = fn_CallObjectMethodV(e, o, m, args);
    va_end(args);
    JLOG("CallObjectMethod → %p", result);
    return result;
}
static jobject fn_CallStaticObjectMethod(JNIEnv *e, jclass c, jmethodID m, ...) {
    const char *mn = (const char*)m;
    if (mn && (uintptr_t)mn>0x10000) JLOG("CallStaticObjectMethod: %s", mn);
    (void)e;(void)c; return NULL;
}
static jboolean fn_CallBooleanMethod(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e;(void)o;(void)m; return JNI_FALSE; }
static jint    fn_CallIntMethod(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e;(void)o;(void)m; return 0; }
static jlong   fn_CallLongMethod(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e;(void)o;(void)m; return 0; }
static void    fn_CallVoidMethod(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e;(void)o;(void)m; }
static void    fn_CallStaticVoidMethod(JNIEnv *e, jclass c, jmethodID m, ...) { (void)e;(void)c;(void)m; }
static jboolean fn_CallStaticBooleanMethod(JNIEnv *e, jclass c, jmethodID m, ...) { (void)e;(void)c;(void)m; return JNI_FALSE; }

// Fields — dispatch based on object kind + field name
static jlong fn_GetLongField(JNIEnv *e, jobject o, jfieldID f)   { (void)e;(void)o;(void)f; return 0; }
static jint fn_GetIntField(JNIEnv *e, jobject o, jfieldID f) {
    (void)e;
    if (!o) return 0;
    uint8_t kind = ((FakeObj*)o)->kind;
    const char *fname = (const char *)f;
    if (kind == KIND_FILE_DESCRIPTOR) {
        if (fname && strstr(fname, "descriptor"))
            return ((FakeFileDescriptor*)o)->fd;
    } else if (kind == KIND_SCORED_INPUT) {
        if (fname && strstr(fname, "startVertex"))
            return ((FakeScoredInput*)o)->startVertexIndex;
        if (fname && strstr(fname, "endVertex"))
            return ((FakeScoredInput*)o)->endVertexIndex;
    } else if (kind == KIND_RANGE) {
        if (fname && strstr(fname, "start"))
            return ((FakeRange*)o)->start;
        if (fname && strstr(fname, "end"))
            return ((FakeRange*)o)->end;
    }
    return 0;
}
static jobject fn_GetObjectField(JNIEnv *e, jobject o, jfieldID f) {
    (void)e;
    JLOG("GetObjectField: obj=%p kind=%d field=%p", o, o?((FakeObj*)o)->kind:-1, (void*)f);
    if (o && ((FakeObj*)o)->kind == KIND_SCORED_INPUT) {
        const char *fname = (const char *)f;
        JLOG("GetObjectField ScoredInput: fname=%s", fname?fname:"(null)");
        if (fname && strstr(fname, "nput"))  // "input" or "mInput" etc.
            return (jobject)((FakeScoredInput*)o)->input;
    }
    return NULL;
}
static jfloat fn_GetFloatField(JNIEnv *e, jobject o, jfieldID f) {
    (void)e;
    JLOG("GetFloatField: obj=%p field=%p", o, (void*)f);
    if (o && ((FakeObj*)o)->kind == KIND_SCORED_INPUT) {
        const char *fname = (const char *)f;
        if (fname && strstr(fname, "core"))  // "score" or "mScore" etc.
            return ((FakeScoredInput*)o)->score;
    }
    return 0.0f;
}
static void    fn_SetLongField(JNIEnv *e, jobject o, jfieldID f, jlong v) { (void)e;(void)o;(void)f;(void)v; }
static void    fn_SetIntField(JNIEnv *e, jobject o, jfieldID f, jint v)   { (void)e;(void)o;(void)f;(void)v; }
static jlong   fn_GetStaticLongField(JNIEnv *e, jclass c, jfieldID f) { (void)e;(void)c;(void)f; return 0; }

// Direct buffers
static jobject fn_NewDirectByteBuffer(JNIEnv *e, void *a, jlong c) { (void)e;(void)a;(void)c; return NULL; }
static void   *fn_GetDirectBufferAddress(JNIEnv *e, jobject b)     { (void)e;(void)b; return NULL; }
// JNI slot 231: GetDirectBufferCapacity — not used by HMM engine but kept
// in case future native code calls it.
static jlong   fn_GetDirectBufferCapacity(JNIEnv *e, jobject b) __attribute__((unused));
static jlong   fn_GetDirectBufferCapacity(JNIEnv *e, jobject b)    { (void)e;(void)b; return 0; }

// Extra stubs needed by build_iface
static void *fn_GetSuperclass(JNIEnv *e, jclass c)              { (void)e;(void)c; return NULL; }
static int   fn_Throw(JNIEnv *e, void *t)                      { (void)e;(void)t; return 0; }
static int   fn_ThrowNew(JNIEnv *e, jclass c, const char *m)   { (void)e;(void)c;(void)m; return 0; }
static void *fn_GetObjectClass(JNIEnv *e, jobject o)            { (void)e;(void)o; return (void*)(uintptr_t)0xC1A55; }

// ── JavaVM / GetEnv ───────────────────────────────────────────────────────────
struct JNIInvokeInterface_ {
    void *r0,*r1,*r2;
    jint (*DestroyJavaVM)(JavaVM*);
    jint (*AttachCurrentThread)(JavaVM*,void**,void*);
    jint (*DetachCurrentThread)(JavaVM*);
    jint (*GetEnv)(JavaVM*,void**,jint);
    jint (*AttachCurrentThreadAsDaemon)(JavaVM*,void**,void*);
};
struct JavaVM_ { const struct JNIInvokeInterface_ *functions; };

static JNIEnv *g_env;
static struct JavaVM_ g_vm;

static jint vm_GetEnv(JavaVM *vm, void **env, jint v) { (void)vm;(void)v; *env=g_env; return 0; }
static jint vm_Attach(JavaVM *vm, void **env, void *a) { (void)vm;(void)a; *env=g_env; return 0; }
static jint vm_Detach(JavaVM *vm) { (void)vm; return 0; }
static jint vm_Destroy(JavaVM *vm) { (void)vm; return 0; }
static const struct JNIInvokeInterface_ g_vm_iface = {
    NULL,NULL,NULL, vm_Destroy, vm_Attach, vm_Detach, vm_GetEnv, vm_Attach
};

static jint fn_GetJavaVM(JNIEnv *e, JavaVM **vm) { (void)e; *vm=(JavaVM*)&g_vm; return 0; }

// ── JNINativeInterface_ using void* table for all slots ──────────────────────
// We use a flat void* array matching the JNI spec slot order exactly.
// This avoids function pointer type mismatch errors entirely.
#define NSLOTS 232
static void* g_iface_slots[NSLOTS];

// Catch-all for unimplemented JNI functions
// Use slot-specific handlers so we can identify which slot is called
static int g_unimp_slot = -1;
#define UNIMP(n) static jint fn_Unimp_##n(void) { \
    char _b[128]; int _len = snprintf(_b,sizeof(_b),"[JNI] WARNING: unimplemented slot %d called\n",n); \
    write(STDERR_FILENO,_b,_len>0?(size_t)_len:0); \
    JLOG("UNIMP slot %d called", n); \
    g_unimp_slot = n; return 0; }
UNIMP(0) UNIMP(1) UNIMP(2) UNIMP(3) UNIMP(4) UNIMP(5) UNIMP(6) UNIMP(7)
UNIMP(8) UNIMP(9) UNIMP(10) UNIMP(11) UNIMP(12) UNIMP(13) UNIMP(14) UNIMP(15)
UNIMP(16) UNIMP(17) UNIMP(18) UNIMP(19) UNIMP(20) UNIMP(21) UNIMP(22) UNIMP(23)
UNIMP(24) UNIMP(25) UNIMP(26) UNIMP(27) UNIMP(28) UNIMP(29) UNIMP(30) UNIMP(31)
UNIMP(32) UNIMP(33) UNIMP(34) UNIMP(35) UNIMP(36) UNIMP(37) UNIMP(38) UNIMP(39)
UNIMP(40) UNIMP(41) UNIMP(42) UNIMP(43) UNIMP(44) UNIMP(45) UNIMP(46) UNIMP(47)
UNIMP(48) UNIMP(49) UNIMP(50) UNIMP(51) UNIMP(52) UNIMP(53) UNIMP(54) UNIMP(55)
UNIMP(56) UNIMP(57) UNIMP(58) UNIMP(59) UNIMP(60) UNIMP(61) UNIMP(62) UNIMP(63)
UNIMP(64) UNIMP(65) UNIMP(66) UNIMP(67) UNIMP(68) UNIMP(69) UNIMP(70) UNIMP(71)
UNIMP(72) UNIMP(73) UNIMP(74) UNIMP(75) UNIMP(76) UNIMP(77) UNIMP(78) UNIMP(79)
UNIMP(80) UNIMP(81) UNIMP(82) UNIMP(83) UNIMP(84) UNIMP(85) UNIMP(86) UNIMP(87)
UNIMP(88) UNIMP(89) UNIMP(90) UNIMP(91) UNIMP(92) UNIMP(93) UNIMP(94) UNIMP(95)
UNIMP(96) UNIMP(97) UNIMP(98) UNIMP(99) UNIMP(100) UNIMP(101) UNIMP(102) UNIMP(103)
UNIMP(104) UNIMP(105) UNIMP(106) UNIMP(107) UNIMP(108) UNIMP(109) UNIMP(110) UNIMP(111)
UNIMP(112) UNIMP(113) UNIMP(114) UNIMP(115) UNIMP(116) UNIMP(117) UNIMP(118) UNIMP(119)
UNIMP(120) UNIMP(121) UNIMP(122) UNIMP(123) UNIMP(124) UNIMP(125) UNIMP(126) UNIMP(127)
UNIMP(128) UNIMP(129) UNIMP(130) UNIMP(131) UNIMP(132) UNIMP(133) UNIMP(134) UNIMP(135)
UNIMP(136) UNIMP(137) UNIMP(138) UNIMP(139) UNIMP(140) UNIMP(141) UNIMP(142) UNIMP(143)
UNIMP(144) UNIMP(145) UNIMP(146) UNIMP(147) UNIMP(148) UNIMP(149) UNIMP(150) UNIMP(151)
UNIMP(152) UNIMP(153) UNIMP(154) UNIMP(155) UNIMP(156) UNIMP(157) UNIMP(158) UNIMP(159)
UNIMP(160) UNIMP(161) UNIMP(162) UNIMP(163) UNIMP(164) UNIMP(165) UNIMP(166) UNIMP(167)
UNIMP(168) UNIMP(169) UNIMP(170) UNIMP(171) UNIMP(172) UNIMP(173) UNIMP(174) UNIMP(175)
UNIMP(176) UNIMP(177) UNIMP(178) UNIMP(179) UNIMP(180) UNIMP(181) UNIMP(182) UNIMP(183)
UNIMP(184) UNIMP(185) UNIMP(186) UNIMP(187) UNIMP(188) UNIMP(189) UNIMP(190) UNIMP(191)
UNIMP(192) UNIMP(193) UNIMP(194) UNIMP(195) UNIMP(196) UNIMP(197) UNIMP(198) UNIMP(199)
UNIMP(200) UNIMP(201) UNIMP(202) UNIMP(203) UNIMP(204) UNIMP(205) UNIMP(206) UNIMP(207)
UNIMP(208) UNIMP(209) UNIMP(210) UNIMP(211) UNIMP(212) UNIMP(213) UNIMP(214) UNIMP(215)
UNIMP(216) UNIMP(217) UNIMP(218) UNIMP(219) UNIMP(220) UNIMP(221) UNIMP(222) UNIMP(223)
UNIMP(224) UNIMP(225) UNIMP(226) UNIMP(227) UNIMP(228) UNIMP(229) UNIMP(230) UNIMP(231)

static void *g_unimp_table[NSLOTS] = {
    fn_Unimp_0,fn_Unimp_1,fn_Unimp_2,fn_Unimp_3,fn_Unimp_4,fn_Unimp_5,fn_Unimp_6,fn_Unimp_7,
    fn_Unimp_8,fn_Unimp_9,fn_Unimp_10,fn_Unimp_11,fn_Unimp_12,fn_Unimp_13,fn_Unimp_14,fn_Unimp_15,
    fn_Unimp_16,fn_Unimp_17,fn_Unimp_18,fn_Unimp_19,fn_Unimp_20,fn_Unimp_21,fn_Unimp_22,fn_Unimp_23,
    fn_Unimp_24,fn_Unimp_25,fn_Unimp_26,fn_Unimp_27,fn_Unimp_28,fn_Unimp_29,fn_Unimp_30,fn_Unimp_31,
    fn_Unimp_32,fn_Unimp_33,fn_Unimp_34,fn_Unimp_35,fn_Unimp_36,fn_Unimp_37,fn_Unimp_38,fn_Unimp_39,
    fn_Unimp_40,fn_Unimp_41,fn_Unimp_42,fn_Unimp_43,fn_Unimp_44,fn_Unimp_45,fn_Unimp_46,fn_Unimp_47,
    fn_Unimp_48,fn_Unimp_49,fn_Unimp_50,fn_Unimp_51,fn_Unimp_52,fn_Unimp_53,fn_Unimp_54,fn_Unimp_55,
    fn_Unimp_56,fn_Unimp_57,fn_Unimp_58,fn_Unimp_59,fn_Unimp_60,fn_Unimp_61,fn_Unimp_62,fn_Unimp_63,
    fn_Unimp_64,fn_Unimp_65,fn_Unimp_66,fn_Unimp_67,fn_Unimp_68,fn_Unimp_69,fn_Unimp_70,fn_Unimp_71,
    fn_Unimp_72,fn_Unimp_73,fn_Unimp_74,fn_Unimp_75,fn_Unimp_76,fn_Unimp_77,fn_Unimp_78,fn_Unimp_79,
    fn_Unimp_80,fn_Unimp_81,fn_Unimp_82,fn_Unimp_83,fn_Unimp_84,fn_Unimp_85,fn_Unimp_86,fn_Unimp_87,
    fn_Unimp_88,fn_Unimp_89,fn_Unimp_90,fn_Unimp_91,fn_Unimp_92,fn_Unimp_93,fn_Unimp_94,fn_Unimp_95,
    fn_Unimp_96,fn_Unimp_97,fn_Unimp_98,fn_Unimp_99,fn_Unimp_100,fn_Unimp_101,fn_Unimp_102,fn_Unimp_103,
    fn_Unimp_104,fn_Unimp_105,fn_Unimp_106,fn_Unimp_107,fn_Unimp_108,fn_Unimp_109,fn_Unimp_110,fn_Unimp_111,
    fn_Unimp_112,fn_Unimp_113,fn_Unimp_114,fn_Unimp_115,fn_Unimp_116,fn_Unimp_117,fn_Unimp_118,fn_Unimp_119,
    fn_Unimp_120,fn_Unimp_121,fn_Unimp_122,fn_Unimp_123,fn_Unimp_124,fn_Unimp_125,fn_Unimp_126,fn_Unimp_127,
    fn_Unimp_128,fn_Unimp_129,fn_Unimp_130,fn_Unimp_131,fn_Unimp_132,fn_Unimp_133,fn_Unimp_134,fn_Unimp_135,
    fn_Unimp_136,fn_Unimp_137,fn_Unimp_138,fn_Unimp_139,fn_Unimp_140,fn_Unimp_141,fn_Unimp_142,fn_Unimp_143,
    fn_Unimp_144,fn_Unimp_145,fn_Unimp_146,fn_Unimp_147,fn_Unimp_148,fn_Unimp_149,fn_Unimp_150,fn_Unimp_151,
    fn_Unimp_152,fn_Unimp_153,fn_Unimp_154,fn_Unimp_155,fn_Unimp_156,fn_Unimp_157,fn_Unimp_158,fn_Unimp_159,
    fn_Unimp_160,fn_Unimp_161,fn_Unimp_162,fn_Unimp_163,fn_Unimp_164,fn_Unimp_165,fn_Unimp_166,fn_Unimp_167,
    fn_Unimp_168,fn_Unimp_169,fn_Unimp_170,fn_Unimp_171,fn_Unimp_172,fn_Unimp_173,fn_Unimp_174,fn_Unimp_175,
    fn_Unimp_176,fn_Unimp_177,fn_Unimp_178,fn_Unimp_179,fn_Unimp_180,fn_Unimp_181,fn_Unimp_182,fn_Unimp_183,
    fn_Unimp_184,fn_Unimp_185,fn_Unimp_186,fn_Unimp_187,fn_Unimp_188,fn_Unimp_189,fn_Unimp_190,fn_Unimp_191,
    fn_Unimp_192,fn_Unimp_193,fn_Unimp_194,fn_Unimp_195,fn_Unimp_196,fn_Unimp_197,fn_Unimp_198,fn_Unimp_199,
    fn_Unimp_200,fn_Unimp_201,fn_Unimp_202,fn_Unimp_203,fn_Unimp_204,fn_Unimp_205,fn_Unimp_206,fn_Unimp_207,
    fn_Unimp_208,fn_Unimp_209,fn_Unimp_210,fn_Unimp_211,fn_Unimp_212,fn_Unimp_213,fn_Unimp_214,fn_Unimp_215,
    fn_Unimp_216,fn_Unimp_217,fn_Unimp_218,fn_Unimp_219,fn_Unimp_220,fn_Unimp_221,fn_Unimp_222,fn_Unimp_223,
    fn_Unimp_224,fn_Unimp_225,fn_Unimp_226,fn_Unimp_227,fn_Unimp_228,fn_Unimp_229,fn_Unimp_230,fn_Unimp_231
};

static void build_iface(void) {
    // Fill all slots with per-slot unimplemented tracers
    for (int i = 0; i < NSLOTS; i++)
        g_iface_slots[i] = g_unimp_table[i];

    // Slot indices from Android NDK jni.h — EXACT positions verified against
    // the JNINativeInterface_ struct definition with all entries counted.
    // 0-3: reserved
    g_iface_slots[0] = NULL;
    g_iface_slots[1] = NULL;
    g_iface_slots[2] = NULL;
    g_iface_slots[3] = NULL;
    g_iface_slots[4]   = fn_GetVersion;
    g_iface_slots[6]   = fn_FindClass;
    g_iface_slots[10]  = fn_GetSuperclass;
    g_iface_slots[13]  = fn_Throw;
    g_iface_slots[14]  = fn_ThrowNew;
    g_iface_slots[15]  = fn_ExceptionOccurred;
    g_iface_slots[16]  = fn_ExceptionDescribe;
    g_iface_slots[17]  = fn_ExceptionClear;
    g_iface_slots[18]  = fn_FatalError;
    g_iface_slots[19]  = fn_PushLocalFrame;
    g_iface_slots[20]  = fn_PopLocalFrame;
    g_iface_slots[21]  = fn_NewGlobalRef;
    g_iface_slots[22]  = fn_DeleteGlobalRef;
    g_iface_slots[23]  = fn_DeleteLocalRef;
    g_iface_slots[24]  = fn_IsSameObject;
    g_iface_slots[25]  = fn_NewLocalRef;
    g_iface_slots[26]  = fn_EnsureLocalCapacity;
    g_iface_slots[28]  = fn_NewObject;
    g_iface_slots[29]  = fn_NewObjectV;
    g_iface_slots[31]  = fn_GetObjectClass;
    g_iface_slots[32]  = fn_IsInstanceOf;
    g_iface_slots[33]  = fn_GetMethodID;
    g_iface_slots[34]  = fn_CallObjectMethod;
    g_iface_slots[35]  = fn_CallObjectMethodV;
    g_iface_slots[37]  = fn_CallBooleanMethod;
    g_iface_slots[49]  = fn_CallIntMethod;
    g_iface_slots[52]  = fn_CallLongMethod;
    g_iface_slots[61]  = fn_CallVoidMethod;
    g_iface_slots[94]  = fn_GetFieldID;
    g_iface_slots[95]  = fn_GetObjectField;
    g_iface_slots[100] = fn_GetIntField;
    g_iface_slots[101] = fn_GetLongField;
    g_iface_slots[102] = fn_GetFloatField;
    g_iface_slots[109] = fn_SetIntField;
    g_iface_slots[110] = fn_SetLongField;
    g_iface_slots[113] = fn_GetStaticMethodID;
    g_iface_slots[114] = fn_CallStaticObjectMethod;
    g_iface_slots[117] = fn_CallStaticBooleanMethod;
    g_iface_slots[141] = fn_CallStaticVoidMethod;
    g_iface_slots[144] = fn_GetStaticFieldID;
    g_iface_slots[145] = fn_CallStaticObjectMethod; // GetStaticObjectField (reuse)
    g_iface_slots[150] = fn_GetIntField;            // GetStaticIntField (reuse)
    g_iface_slots[151] = fn_GetStaticLongField;
    // 154-162: SetStatic*Field
    g_iface_slots[163] = fn_NewString;              // NewString (UTF-16)
    g_iface_slots[164] = fn_GetStringLength;
    g_iface_slots[165] = fn_GetStringChars;
    g_iface_slots[166] = fn_ReleaseStringChars;
    g_iface_slots[167] = fn_NewStringUTF;
    g_iface_slots[168] = fn_GetStringUTFLength;
    g_iface_slots[169] = fn_GetStringUTFChars;
    g_iface_slots[170] = fn_ReleaseStringUTFChars;
    g_iface_slots[171] = fn_GetArrayLength;
    g_iface_slots[172] = fn_NewObjectArray;
    g_iface_slots[173] = fn_GetObjectArrayElement;
    g_iface_slots[174] = fn_SetObjectArrayElement;
    g_iface_slots[175] = fn_NewByteArray;           // NewBooleanArray (reuse)
    g_iface_slots[176] = fn_NewByteArray;
    g_iface_slots[179] = fn_NewIntArray;
    g_iface_slots[180] = fn_NewLongArray;
    g_iface_slots[181] = fn_NewFloatArray;
    g_iface_slots[184] = fn_GetByteArrayElements;
    g_iface_slots[187] = fn_GetIntArrayElements;
    g_iface_slots[188] = fn_GetLongArrayElements;
    g_iface_slots[189] = fn_GetFloatArrayElements;
    g_iface_slots[192] = fn_ReleaseByteArrayElements;
    g_iface_slots[195] = fn_ReleaseIntArrayElements;
    g_iface_slots[196] = fn_ReleaseLongArrayElements;
    g_iface_slots[197] = fn_ReleaseFloatArrayElements;
    g_iface_slots[200] = fn_GetByteArrayRegion;
    g_iface_slots[203] = fn_GetIntArrayRegion;
    g_iface_slots[204] = fn_GetLongArrayRegion;
    g_iface_slots[208] = fn_SetByteArrayRegion;
    g_iface_slots[211] = fn_SetIntArrayRegion;
    g_iface_slots[212] = fn_SetLongArrayRegion;
    g_iface_slots[215] = fn_RegisterNatives;
    g_iface_slots[216] = fn_UnregisterNatives;
    g_iface_slots[217] = fn_MonitorEnter;
    g_iface_slots[218] = fn_MonitorExit;
    g_iface_slots[219] = fn_GetJavaVM;
    g_iface_slots[220] = fn_GetStringRegion;
    g_iface_slots[221] = fn_GetStringUTFRegion;
    g_iface_slots[222] = fn_GetPAC;
    g_iface_slots[223] = fn_RelPAC;
    g_iface_slots[228] = fn_ExceptionCheck;
    g_iface_slots[229] = fn_NewDirectByteBuffer;
    g_iface_slots[230] = fn_GetDirectBufferAddress;
}


JNIEnv *jni_env_create(void) {
    if (g_env) return g_env;
    build_iface();
    g_vm.functions = &g_vm_iface;
    JNIEnv *env = malloc(sizeof(void*));
    *(void**)env = (void*)g_iface_slots;
    g_env = env;
    return env;
}
JavaVM *jni_vm_get(void) { if(!g_env) jni_env_create(); return (JavaVM*)&g_vm; }
void    jni_env_destroy(JNIEnv *env) { free(env); g_env = NULL; }

// ── JNI call helpers ──────────────────────────────────────────────────────────
// These call into our slot table without needing the struct to be fully defined.
typedef void* (*slot_fn)(void);
#define CALL_SLOT(e, idx, ...) \
    ((slot_fn*)*(void**)(e))[idx]

jstring    jni_NewStringUTF(JNIEnv *e, const char *s)       { return jni_new_string(e, s); }
jbyteArray jni_NewByteArray(JNIEnv *e, jsize n)             { return fn_NewByteArray(e, n); }
jlongArray jni_NewLongArray(JNIEnv *e, jsize n)             { return fn_NewLongArray(e, n); }
void jni_SetByteArrayRegion(JNIEnv *e, jbyteArray a, jsize start, jsize len, const jbyte *buf) {
    fn_SetByteArrayRegion(e, a, start, len, buf);
}
jlong *jni_GetLongArrayElements(JNIEnv *e, jlongArray a) {
    return fn_GetLongArrayElements(e, a, NULL);
}
void jni_ReleaseLongArrayElements(JNIEnv *e, jlongArray a, jlong *elems) {
    fn_ReleaseLongArrayElements(e, a, elems, 0);
}
