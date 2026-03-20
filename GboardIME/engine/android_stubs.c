// Stubs for Android-specific and Linux-specific APIs
// that don't exist on macOS.

#include "android_stubs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/mman.h>

// Use raw write() for logging to avoid stdio lock deadlocks when siglongjmp
// is used for crash recovery in constructors.
extern int g_log_fd;
static void alog_write(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0 && g_log_fd >= 0) write(g_log_fd, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}
#define ALOG(fmt, ...) alog_write("[AAsset] " fmt "\n", ##__VA_ARGS__)

static char s_asset_base[4096] = ".";

// ── Bionic TLS compatibility ──────────────────────────────────────────────────
// On ARM64 Android, TPIDR_EL0 points to the Bionic TLS block.
// On macOS, TPIDR_EL0 is a small kernel value (e.g. 0x1004), not a pointer.
// The .so reads [TPIDR_EL0 + 0x28] (stack canary) and other TLS slots.
// We allocate a fake TLS block and swap TPIDR_EL0 when calling .so code.

// Bionic TLS layout (ARM64):
//   offset 0x00-0x07: slot[0] — bionic_tls pointer
//   offset 0x08-0x0f: slot[1] — thread ID
//   offset 0x10-0x17: slot[2] — errno pointer
//   offset 0x18-0x1f: slot[3]
//   offset 0x20-0x27: slot[4]
//   offset 0x28-0x2f: slot[5] — stack guard canary
//   ...up to many slots
//
// STRATEGY: macOS kernel manages TPIDR_EL0 and resets it on every syscall,
// so we cannot change it. Instead, we patch all `mrs xN, TPIDR_EL0`
// instructions in the loaded .so to `ldr xN, [PC + offset]` pointing to
// a literal containing our fake TLS address. This is done by
// elf_patch_tpidr() in elf_loader.c after the .so is loaded/relocated.

#include <sys/mman.h>

#define FAKE_TLS_SIZE 4096
uint8_t s_fake_tls[FAKE_TLS_SIZE] __attribute__((aligned(16)));
static int s_fake_errno = 0;

// Secondary TLS-like struct that TLS+0x28 points to.
// On Bionic ARM64, TLS+0x28 (slot 5) is the stack guard value.
// But this .so treats it as a pointer and dereferences it.
// Provide a valid zero-filled target. Any field that gets dereferenced
// further points back to this region (self-referential safety net).
static uint8_t s_tls_slot5_target[4096] __attribute__((aligned(16)));

void bionic_tls_setup(void) {
    memset(s_fake_tls, 0, FAKE_TLS_SIZE);
    memset(s_tls_slot5_target, 0, sizeof(s_tls_slot5_target));

    // TLS layout (8-byte slots):
    //   +0x00 = self pointer (points to TLS block itself)
    //   +0x08 = thread ID / pthread_internal_t*
    //   +0x10 = errno pointer
    //   +0x18 = slot 3
    //   +0x20 = slot 4
    //   +0x28 = slot 5 (stack guard OR pointer — this .so dereferences it)
    //   ...

    // Slot 0: self pointer
    uint64_t self_ptr = (uint64_t)s_fake_tls;
    memcpy(&s_fake_tls[0x00], &self_ptr, 8);

    // Slot 2: errno pointer
    uint64_t errno_ptr = (uint64_t)&s_fake_errno;
    memcpy(&s_fake_tls[0x10], &errno_ptr, 8);

    // Slot 5 (+0x28): pointer to a valid region (the .so dereferences this)
    // Make the target self-referential: target+0x28 also points to target
    uint64_t slot5_ptr = (uint64_t)s_tls_slot5_target;
    memcpy(&s_fake_tls[0x28], &slot5_ptr, 8);
    memcpy(&s_tls_slot5_target[0x28], &slot5_ptr, 8);  // self-ref for deeper deref
    // Also set target+0x00 to point to itself
    memcpy(&s_tls_slot5_target[0x00], &slot5_ptr, 8);
}

void bionic_tls_enter(void) {
    // No-op: TPIDR_EL0 patching is done at load time via binary rewriting
}

void bionic_tls_leave(void) {
    // No-op
}

void android_stubs_init(const char *asset_base_dir) {
    if (asset_base_dir)
        snprintf(s_asset_base, sizeof(s_asset_base), "%s", asset_base_dir);
}

// ── Android logging ───────────────────────────────────────────────────────────
#define ANDROID_LOG_VERBOSE 2
#define ANDROID_LOG_DEBUG   3
#define ANDROID_LOG_INFO    4
#define ANDROID_LOG_WARN    5
#define ANDROID_LOG_ERROR   6

static int stub_android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio;
    char buf[1024];
    int off = snprintf(buf, sizeof(buf), "[%s] ", tag ? tag : "?");
    va_list ap; va_start(ap, fmt);
    off += vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
    va_end(ap);
    if (off < (int)sizeof(buf) - 1) buf[off++] = '\n';
    write(STDERR_FILENO, buf, off);
    return 0;
}
static int stub_android_log_write(int prio, const char *tag, const char *text) {
    (void)prio;
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "[%s] %s\n", tag ? tag : "?", text ? text : "");
    write(STDERR_FILENO, buf, n > 0 ? (size_t)n : 0);
    return 0;
}
static int stub_android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    (void)prio;
    char buf[1024];
    int off = snprintf(buf, sizeof(buf), "[%s] ", tag ? tag : "?");
    off += vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
    if (off < (int)sizeof(buf) - 1) buf[off++] = '\n';
    write(STDERR_FILENO, buf, off);
    return 0;
}

// ── Android system properties ─────────────────────────────────────────────────
static int stub_system_property_get(const char *name, char *value) {
    (void)name;
    if (value) value[0] = '\0';
    return 0;
}
static void stub_android_set_abort_message(const char *msg) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "[abort] %s\n", msg ? msg : "");
    write(STDERR_FILENO, buf, n > 0 ? (size_t)n : 0);
}

// ── Android Asset Manager ─────────────────────────────────────────────────────
// We implement a minimal file-based asset manager.
// The HMM engine uses AAssetManager to load data files from APK assets.
// We redirect reads to our extracted asset directory.

typedef struct { FILE *fp; long size; } FakeAsset;
typedef struct { const char *base; } FakeAssetManager;

static FakeAssetManager s_am = { NULL };

// AAssetManager_fromJava — converts a Java AAssetManager to native AAssetManager*
// In our case, we always return our fake asset manager.
static void *stub_AAssetManager_fromJava(void *env, void *assetManager) {
    (void)env; (void)assetManager;
    if (!s_am.base) s_am.base = s_asset_base;
    ALOG("fromJava → %p (base=%s)", &s_am, s_asset_base);
    return &s_am;
}

static void *stub_AAssetManager_open(void *mgr, const char *filename, int mode) {
    (void)mgr; (void)mode;
    char path[8192];
    snprintf(path, sizeof(path), "%s/%s", s_asset_base, filename);
    ALOG("open: %s", path);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        { char eb[512]; int en = snprintf(eb, sizeof(eb), "[AAsset] not found: %s\n", path); write(STDERR_FILENO, eb, en > 0 ? (size_t)en : 0); }
        ALOG("FAILED: %s", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    FakeAsset *a = malloc(sizeof(FakeAsset));
    a->fp = fp; a->size = size;
    return a;
}
static void stub_AAsset_close(FakeAsset *a) {
    if (!a) return;
    fclose(a->fp); free(a);
}
static int stub_AAsset_read(FakeAsset *a, void *buf, size_t count) {
    if (!a) return -1;
    return (int)fread(buf, 1, count, a->fp);
}
static off_t stub_AAsset_seek(FakeAsset *a, off_t offset, int whence) {
    if (!a) return -1;
    fseek(a->fp, offset, whence);
    return ftell(a->fp);
}
static off_t stub_AAsset_getLength(FakeAsset *a) {
    return a ? (off_t)a->size : 0;
}
static off_t stub_AAsset_getRemainingLength(FakeAsset *a) {
    if (!a) return 0;
    long pos = ftell(a->fp);
    return (off_t)(a->size - pos);
}
static const void *stub_AAsset_getBuffer(FakeAsset *a) {
    if (!a) return NULL;
    // Read entire file into buffer
    long pos = ftell(a->fp);
    void *buf = malloc((size_t)a->size);
    fseek(a->fp, 0, SEEK_SET);
    fread(buf, 1, (size_t)a->size, a->fp);
    fseek(a->fp, pos, SEEK_SET);
    ALOG("getBuffer: %ld bytes", a->size);
    return buf;
}

// ── AAudio (voice input — stub everything) ────────────────────────────────────
typedef struct { int dummy; } FakeAudioStream;
typedef struct { int dummy; } FakeAudioStreamBuilder;

static int stub_AAudio_createStreamBuilder(void **out) {
    FakeAudioStreamBuilder *b = calloc(1, sizeof(*b));
    *out = b; return 0; // AAUDIO_OK
}
static void stub_AAudioStreamBuilder_setChannelCount(void *b, int32_t c)    { (void)b;(void)c; }
static void stub_AAudioStreamBuilder_setSampleRate(void *b, int32_t r)      { (void)b;(void)r; }
static void stub_AAudioStreamBuilder_setFormat(void *b, int32_t f)          { (void)b;(void)f; }
static void stub_AAudioStreamBuilder_setDirection(void *b, int32_t d)       { (void)b;(void)d; }
static void stub_AAudioStreamBuilder_setSharingMode(void *b, int32_t m)     { (void)b;(void)m; }
static void stub_AAudioStreamBuilder_setPerformanceMode(void *b, int32_t m) { (void)b;(void)m; }
static void stub_AAudioStreamBuilder_setBufferCapacityInFrames(void *b, int32_t n){ (void)b;(void)n; }
static int  stub_AAudioStreamBuilder_openStream(void *b, void **out) {
    (void)b;
    FakeAudioStream *s = calloc(1, sizeof(*s));
    *out = s; return 0;
}
static void stub_AAudioStreamBuilder_delete(void *b) { free(b); }
static int  stub_AAudioStream_requestStart(void *s)  { (void)s; return 0; }
static int  stub_AAudioStream_requestStop(void *s)   { (void)s; return 0; }
static int  stub_AAudioStream_close(void *s)         { free(s); return 0; }
static int32_t stub_AAudioStream_getFramesPerBurst(void *s)    { (void)s; return 256; }
static int32_t stub_AAudioStream_setBufferSizeInFrames(void *s, int32_t n) { (void)s; return n; }
static int  stub_AAudioStream_waitForStateChange(void *s, int32_t cur, int32_t *next, int64_t to) {
    (void)s;(void)cur;(void)next;(void)to; return 0;
}
static int32_t stub_AAudioStream_read(void *s, void *buf, int32_t n, int64_t to) {
    (void)s;(void)to;
    memset(buf, 0, (size_t)n * 4); // silence
    return n;
}

// ── AndroidBitmap ─────────────────────────────────────────────────────────────
static int stub_AndroidBitmap_getInfo(void *env, void *bitmap, void *info) {
    (void)env;(void)bitmap;(void)info; return 0;
}
static int stub_AndroidBitmap_lockPixels(void *env, void *bitmap, void **pixels) {
    (void)env;(void)bitmap; *pixels = NULL; return 0;
}
static int stub_AndroidBitmap_unlockPixels(void *env, void *bitmap) {
    (void)env;(void)bitmap; return 0;
}

// ── AHardwareBuffer ───────────────────────────────────────────────────────────
static int stub_AHardwareBuffer_describe(void *buf, void *desc) {
    (void)buf; memset(desc, 0, 64); return 0;
}

// ── Linux-specific: epoll / eventfd / timerfd ─────────────────────────────────
// These are only used in background I/O paths (downloading, metrics).
// For the Pinyin HMM core we stub them out.

static int stub_epoll_create1(int flags) { (void)flags; errno = ENOSYS; return -1; }
static int stub_epoll_ctl(int epfd, int op, int fd, void *event) {
    (void)epfd;(void)op;(void)fd;(void)event; return 0;
}
static int stub_epoll_wait(int epfd, void *events, int maxevents, int timeout) {
    (void)epfd;(void)events;(void)maxevents;(void)timeout; return 0;
}
static int stub_eventfd(unsigned int initval, int flags) {
    (void)initval;(void)flags; errno = ENOSYS; return -1;
}
static int stub_timerfd_create(int clockid, int flags) {
    (void)clockid;(void)flags; errno = ENOSYS; return -1;
}
static int stub_timerfd_settime(int fd, int flags, const void *new_value, void *old_value) {
    (void)fd;(void)flags;(void)new_value;(void)old_value; return -1;
}

// ── dl_iterate_phdr (Linux-only) ──────────────────────────────────────────────
static int stub_dl_iterate_phdr(int (*cb)(void*, size_t, void*), void *data) {
    (void)cb;(void)data; return 0;
}

// ── __register_atfork ─────────────────────────────────────────────────────────
static int stub_register_atfork(void (*prep)(void), void (*parent)(void),
                                  void (*child)(void), void *handle) {
    (void)handle;
    pthread_atfork(prep, parent, child);
    return 0;
}

// ── CPU set stubs ─────────────────────────────────────────────────────────────
static void *stub_sched_cpualloc(size_t count) { (void)count; return calloc(1, 128); }
static int   stub_sched_cpucount(size_t setsize, void *set) { (void)setsize;(void)set; return 1; }
static void  stub_sched_cpufree(void *set) { free(set); }

// ── __ctype_get_mb_cur_max ────────────────────────────────────────────────────
static size_t stub_ctype_get_mb_cur_max(void) { return MB_CUR_MAX; }

// ── __gnu_strerror_r ──────────────────────────────────────────────────────────
static char *stub_gnu_strerror_r(int errnum, char *buf, size_t buflen) {
    strerror_r(errnum, buf, buflen);
    return buf;
}

// ── __sF (Bionic's stdin/stdout/stderr array) ─────────────────────────────────
// On Bionic, __sF is FILE[3] (actual structs, ~152 bytes each).
// The .so computes &__sF[i] using Bionic's sizeof(FILE), so we must allocate
// enough space. We use a 512-byte-per-slot buffer and store the macOS FILE*
// mapping separately for fixup.
#define BIONIC_FILE_SIZE 152
uint8_t __sF[3 * BIONIC_FILE_SIZE];
static FILE *sF_map[3];  // macOS FILE* for stdin/stdout/stderr

static void __attribute__((constructor)) init_sF(void) {
    memset(__sF, 0, sizeof(__sF));
    sF_map[0] = stdin; sF_map[1] = stdout; sF_map[2] = stderr;
}

// Detect if a FILE* is a fake Bionic __sF entry and return the real macOS FILE*.
static FILE *fixup_file(FILE *f) {
    if (!f) return stderr;
    uintptr_t fp = (uintptr_t)f;
    uintptr_t base = (uintptr_t)__sF;
    if (fp >= base && fp < base + sizeof(__sF)) {
        int idx = (int)((fp - base) / BIONIC_FILE_SIZE);
        if (idx >= 0 && idx < 3) return sF_map[idx];
        return stderr;
    }
    return f;
}

// ── stdio wrappers (intercept .so calls with fake Bionic FILE*) ───────────────
static size_t stub_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, fixup_file(stream));
}
static int stub_fflush(FILE *stream) {
    return fflush(fixup_file(stream));
}
static int stub_fputs(const char *s, FILE *stream) {
    return fputs(s, fixup_file(stream));
}
static int stub_fputc(int c, FILE *stream) {
    return fputc(c, fixup_file(stream));
}
static size_t stub_fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fread(ptr, size, nmemb, fixup_file(stream));
}
static int stub_fprintf(FILE *stream, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vfprintf(fixup_file(stream), fmt, ap);
    va_end(ap);
    return ret;
}
static int stub_vfprintf(FILE *stream, const char *fmt, va_list ap) {
    return vfprintf(fixup_file(stream), fmt, ap);
}

// ── pthread wrappers via side-table ───────────────────────────────────────────
// CRITICAL: Bionic pthread_mutex_t = 40 bytes, macOS = 64 bytes.
//           Bionic pthread_rwlock_t = 56 bytes, macOS = 200 bytes.
//           Passing .so's bionic-sized structs to macOS pthread functions causes
//           buffer overflow and memory corruption. We use a side-table to store
//           macOS-sized objects separately from the .so's memory.
#include <os/lock.h>

#define SIDE_TABLE_SIZE 16384
typedef struct {
    void *addr;         // bionic struct address in .so memory
    int type;           // 1=mutex, 2=cond, 3=rwlock
    union {
        pthread_mutex_t mutex;
        pthread_cond_t cond;
        pthread_rwlock_t rwlock;
    };
} SideEntry;

static SideEntry s_side_table[SIDE_TABLE_SIZE];
static os_unfair_lock s_side_lock = OS_UNFAIR_LOCK_INIT;

static SideEntry *side_get(void *addr, int type) {
    uint32_t hash = (uint32_t)(((uintptr_t)addr >> 3) % SIDE_TABLE_SIZE);
    os_unfair_lock_lock(&s_side_lock);
    for (uint32_t i = 0; i < SIDE_TABLE_SIZE; i++) {
        uint32_t idx = (hash + i) % SIDE_TABLE_SIZE;
        if (s_side_table[idx].addr == addr) {
            os_unfair_lock_unlock(&s_side_lock);
            return &s_side_table[idx];
        }
        if (s_side_table[idx].addr == NULL) {
            s_side_table[idx].addr = addr;
            s_side_table[idx].type = type;
            if (type == 1) pthread_mutex_init(&s_side_table[idx].mutex, NULL);
            else if (type == 2) pthread_cond_init(&s_side_table[idx].cond, NULL);
            else if (type == 3) pthread_rwlock_init(&s_side_table[idx].rwlock, NULL);
            os_unfair_lock_unlock(&s_side_lock);
            return &s_side_table[idx];
        }
    }
    os_unfair_lock_unlock(&s_side_lock);
    return NULL; // table full — should never happen
}

static void side_remove(void *addr) {
    uint32_t hash = (uint32_t)(((uintptr_t)addr >> 3) % SIDE_TABLE_SIZE);
    os_unfair_lock_lock(&s_side_lock);
    for (uint32_t i = 0; i < SIDE_TABLE_SIZE; i++) {
        uint32_t idx = (hash + i) % SIDE_TABLE_SIZE;
        if (s_side_table[idx].addr == addr) {
            if (s_side_table[idx].type == 1) pthread_mutex_destroy(&s_side_table[idx].mutex);
            else if (s_side_table[idx].type == 2) pthread_cond_destroy(&s_side_table[idx].cond);
            else if (s_side_table[idx].type == 3) pthread_rwlock_destroy(&s_side_table[idx].rwlock);
            s_side_table[idx].addr = NULL;
            s_side_table[idx].type = 0;
            os_unfair_lock_unlock(&s_side_lock);
            return;
        }
        if (s_side_table[idx].addr == NULL) break;
    }
    os_unfair_lock_unlock(&s_side_lock);
}

// Mutex wrappers — .so passes bionic-sized (40-byte) mutex pointers.
// We look up/create a macOS mutex in the side table.
static int stub_pthread_mutex_lock(void *m) {
    SideEntry *e = side_get(m, 1);
    return e ? pthread_mutex_lock(&e->mutex) : EINVAL;
}
static int stub_pthread_mutex_unlock(void *m) {
    SideEntry *e = side_get(m, 1);
    return e ? pthread_mutex_unlock(&e->mutex) : EINVAL;
}
static int stub_pthread_mutex_trylock(void *m) {
    SideEntry *e = side_get(m, 1);
    return e ? pthread_mutex_trylock(&e->mutex) : EINVAL;
}
static int stub_pthread_mutex_init(void *m, const void *attr) {
    (void)attr;
    SideEntry *e = side_get(m, 1);
    return e ? 0 : EINVAL; // side_get already initializes
}
static int stub_pthread_mutex_destroy(void *m) {
    side_remove(m);
    return 0;
}

// Condition variable wrappers (bionic cond = 48 bytes, macOS = 48 — same size
// but different internal layout, so still use side table for correctness)
static int stub_pthread_cond_wait(void *c, void *m) {
    SideEntry *ce = side_get(c, 2);
    SideEntry *me = side_get(m, 1);
    if (!ce || !me) return EINVAL;
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "[stubs] cond_wait: cond=%p mutex=%p\n", c, m);
    write(STDERR_FILENO, buf, n > 0 ? (size_t)n : 0);
    int r = pthread_cond_wait(&ce->cond, &me->mutex);
    n = snprintf(buf, sizeof(buf), "[stubs] cond_wait done: cond=%p ret=%d\n", c, r);
    write(STDERR_FILENO, buf, n > 0 ? (size_t)n : 0);
    return r;
}
static int stub_pthread_cond_signal(void *c) {
    SideEntry *e = side_get(c, 2);
    return e ? pthread_cond_signal(&e->cond) : EINVAL;
}
static int stub_pthread_cond_broadcast(void *c) {
    SideEntry *e = side_get(c, 2);
    return e ? pthread_cond_broadcast(&e->cond) : EINVAL;
}
static int stub_pthread_cond_timedwait(void *c, void *m, const struct timespec *t) {
    SideEntry *ce = side_get(c, 2);
    SideEntry *me = side_get(m, 1);
    if (!ce || !me) return EINVAL;
    return pthread_cond_timedwait(&ce->cond, &me->mutex, t);
}

// pthread_once — Bionic: 4 bytes (int), macOS: 16 bytes. Use atomic on first 4 bytes.
static volatile int s_once_spin_count = 0;
static int stub_pthread_once(void *once, void (*init_routine)(void)) {
    int *flag = (int *)once;
    if (__sync_val_compare_and_swap(flag, 0, 2) == 0) {
        init_routine();
        __sync_synchronize();
        *flag = 1;
    } else {
        int spins = 0;
        while (__sync_add_and_fetch(flag, 0) != 1) {
            if (++spins > 100) {
                char buf[128];
                int n = snprintf(buf, sizeof(buf),
                    "[stubs] pthread_once spinning: flag=%p val=%d spins=%d\n",
                    (void*)flag, *flag, spins);
                write(STDERR_FILENO, buf, n > 0 ? (size_t)n : 0);
                if (spins > 200) {
                    // Give up — force completion to avoid deadlock
                    *flag = 1;
                    break;
                }
            }
            usleep(1000);
        }
    }
    return 0;
}

// Rwlock wrappers — Bionic rwlock = 56 bytes, macOS = 200 bytes!
static int stub_pthread_rwlock_rdlock(void *rw) {
    SideEntry *e = side_get(rw, 3);
    return e ? pthread_rwlock_rdlock(&e->rwlock) : EINVAL;
}
static int stub_pthread_rwlock_wrlock(void *rw) {
    SideEntry *e = side_get(rw, 3);
    return e ? pthread_rwlock_wrlock(&e->rwlock) : EINVAL;
}
static int stub_pthread_rwlock_unlock(void *rw) {
    SideEntry *e = side_get(rw, 3);
    return e ? pthread_rwlock_unlock(&e->rwlock) : EINVAL;
}
static int stub_pthread_rwlock_tryrdlock(void *rw) {
    SideEntry *e = side_get(rw, 3);
    return e ? pthread_rwlock_tryrdlock(&e->rwlock) : EINVAL;
}
static int stub_pthread_rwlock_trywrlock(void *rw) {
    SideEntry *e = side_get(rw, 3);
    return e ? pthread_rwlock_trywrlock(&e->rwlock) : EINVAL;
}

// ── __errno (Bionic's errno accessor) ─────────────────────────────────────────
static int *stub_errno(void) { return &errno; }

// ── sincos / sincosf (Linux-specific, macOS has __sincos) ─────────────────────
#include <math.h>
static void stub_sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
static void stub_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }

// ── memalign (Linux; use posix_memalign on macOS) ─────────────────────────────
static void *stub_memalign(size_t alignment, size_t size) {
    void *p = NULL;
    posix_memalign(&p, alignment, size);
    return p;
}

// ── malloc_usable_size → malloc_size on macOS ─────────────────────────────────
#include <malloc/malloc.h>
static size_t stub_malloc_usable_size(void *ptr) { return malloc_size(ptr); }

// ── __cxa_thread_atexit_impl ──────────────────────────────────────────────────
static int stub_cxa_thread_atexit_impl(void (*dtor)(void*), void *obj, void *dso) {
    (void)dtor; (void)obj; (void)dso; return 0; // leak thread-locals, fine for our use
}

// ── Linux-specific stubs (sched, prctl, sysinfo, etc.) ───────────────────────
static int stub_sched_setaffinity(int pid, size_t sz, void *m) { (void)pid;(void)sz;(void)m; return 0; }
static int stub_sched_getaffinity(int pid, size_t sz, void *m) { (void)pid;(void)sz; if(m) memset(m,0xff,sz); return 0; }
static int stub_prctl(int op, ...) { (void)op; return 0; }
static int stub_sysinfo(void *info) { (void)info; return -1; }
static unsigned long stub_getauxval(unsigned long type) { (void)type; return 0; }
static int stub_tgkill(int tgid, int tid, int sig) { (void)tgid;(void)tid;(void)sig; return -1; }
static int stub_posix_fadvise(int fd, off_t o, off_t l, int a) { (void)fd;(void)o;(void)l;(void)a; return 0; }
static int stub_sem_timedwait(void *sem, const void *ts) { (void)sem;(void)ts; errno=ETIMEDOUT; return -1; }
static void *stub_mremap(void *old, size_t oldsz, size_t newsz, int flags, ...) {
    (void)old;(void)oldsz;(void)newsz;(void)flags; errno=ENOMEM; return (void*)-1;
}

// ── timer stubs ──────────────────────────────────────────────────────────────
static int stub_timer_create(int clockid, void *evp, void *tid) { (void)clockid;(void)evp;(void)tid; return -1; }
static int stub_timer_settime(void *tid, int f, const void *n, void *o) { (void)tid;(void)f;(void)n;(void)o; return -1; }
static int stub_timer_delete(void *tid) { (void)tid; return -1; }

// ── AStatus stubs ────────────────────────────────────────────────────────────
static int stub_AStatus_isOk(void *s) { (void)s; return 1; }
static int stub_AStatus_getExceptionCode(void *s) { (void)s; return 0; }
static int stub_AStatus_getServiceSpecificError(void *s) { (void)s; return 0; }
static const char *stub_AStatus_getMessage(void *s) { (void)s; return ""; }
static const char *stub_AStatus_getDescription(void *s) { (void)s; return "ok"; }
static void stub_AStatus_deleteDescription(const char *d) { (void)d; }

// ── ICU data stubs (return NULL — engine has its own data) ───────────────────
static const void *stub_uprv_getICUData(void) { return NULL; }

// ── No-op stubs for various optional symbols ────────────────────────────────
static void stub_noop(void) {}
static int stub_noop_ret0(void) { return 0; }

// ── Fake dlopen/dlsym for data bundle .so files ─────────────────────────────
// The engine tries dlopen("libpinyin_data_bundle.so") to load embedded data.
// We intercept this to serve data from loose files in the pack directory.

#define FAKE_DL_MAGIC ((void*)(uintptr_t)0xDEADB00C)
#define MAX_MAPPED_FILES 128

typedef struct {
    char name[256];      // symbol name (e.g. "pinyin_system_dictionary")
    void *data;          // mmap'd data
    size_t size;         // file size
} MappedFile;

static MappedFile s_mapped[MAX_MAPPED_FILES];
static int s_mapped_count = 0;

static MappedFile *find_or_load(const char *name) {
    // Check if already loaded
    for (int i = 0; i < s_mapped_count; i++) {
        if (strcmp(s_mapped[i].name, name) == 0)
            return &s_mapped[i];
    }
    // Try to load from pack dir
    if (s_mapped_count >= MAX_MAPPED_FILES) return NULL;
    char path[8192];
    snprintf(path, sizeof(path), "%s/%s", s_asset_base, name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) { close(fd); return NULL; }
    void *data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) return NULL;

    MappedFile *mf = &s_mapped[s_mapped_count++];
    snprintf(mf->name, sizeof(mf->name), "%s", name);
    mf->data = data;
    mf->size = (size_t)st.st_size;
    { char b[512]; int n = snprintf(b, sizeof(b), "[dlsym] loaded %s (%zu bytes) from %s\n", name, mf->size, path);
      write(STDERR_FILENO, b, n > 0 ? (size_t)n : 0); }
    return mf;
}

static void *stub_dlopen(const char *filename, int flags) {
    (void)flags;
    if (!filename) return dlopen(NULL, flags);  // self-handle

    // Intercept data bundle .so requests
    if (strstr(filename, "data_bundle") || strstr(filename, "pinyin_data")) {
        { char b[512]; int n = snprintf(b, sizeof(b), "[dlopen] intercepting %s → fake handle\n", filename);
          write(STDERR_FILENO, b, n > 0 ? (size_t)n : 0); }
        return FAKE_DL_MAGIC;
    }
    // Pass through to real dlopen for other libs
    return dlopen(filename, flags);
}

static void *stub_dlsym(void *handle, const char *symbol) {
    if (handle == FAKE_DL_MAGIC) {
        // The .so looks for symbols like:
        //   _binary_<filename>_start → pointer to beginning of data
        //   _binary_<filename>_end   → pointer to end of data
        const char *prefix = "_binary_";
        size_t pfxlen = 8;
        if (strncmp(symbol, prefix, pfxlen) == 0) {
            const char *rest = symbol + pfxlen;
            size_t rlen = strlen(rest);
            int is_start = (rlen > 6 && strcmp(rest + rlen - 6, "_start") == 0);
            int is_end   = (rlen > 4 && strcmp(rest + rlen - 4, "_end") == 0);
            if (is_start || is_end) {
                // Extract base name (between _binary_ and _start/_end)
                char base[256];
                size_t baselen = is_start ? (rlen - 6) : (rlen - 4);
                if (baselen >= sizeof(base)) baselen = sizeof(base) - 1;
                memcpy(base, rest, baselen);
                base[baselen] = '\0';
                MappedFile *mf = find_or_load(base);
                if (mf) {
                    if (is_start) return mf->data;
                    else return (char*)mf->data + mf->size;
                }
            }
        }
        // Direct name lookup
        MappedFile *mf = find_or_load(symbol);
        if (mf) return mf->data;
        // Try with _size suffix
        size_t slen = strlen(symbol);
        if (slen > 5 && strcmp(symbol + slen - 5, "_size") == 0) {
            char base[256];
            snprintf(base, sizeof(base), "%.*s", (int)(slen - 5), symbol);
            MappedFile *bmf = find_or_load(base);
            if (bmf) return &bmf->size;
        }
        { char b[512]; int n = snprintf(b, sizeof(b), "[dlsym] symbol not found in bundle: %s\n", symbol);
          write(STDERR_FILENO, b, n > 0 ? (size_t)n : 0); }
        return NULL;
    }
    return dlsym(handle, symbol);
}

static int stub_dlclose(void *handle) {
    if (handle == FAKE_DL_MAGIC) return 0;
    return dlclose(handle);
}

static char *stub_dlerror(void) {
    return dlerror();
}

// ── Symbol table ──────────────────────────────────────────────────────────────
#define E(n, f) { n, (void*)(f) }

static const SymEntry s_table[] = {
    E("__android_log_print",            stub_android_log_print),
    E("__android_log_write",            stub_android_log_write),
    E("__android_log_vprint",           stub_android_log_vprint),
    E("__system_property_get",          stub_system_property_get),
    E("android_set_abort_message",      stub_android_set_abort_message),

    E("AAssetManager_fromJava",          stub_AAssetManager_fromJava),
    E("AAssetManager_open",             stub_AAssetManager_open),
    E("AAsset_close",                   stub_AAsset_close),
    E("AAsset_read",                    stub_AAsset_read),
    E("AAsset_seek",                    stub_AAsset_seek),
    E("AAsset_getLength",               stub_AAsset_getLength),
    E("AAsset_getRemainingLength",      stub_AAsset_getRemainingLength),
    E("AAsset_getBuffer",               stub_AAsset_getBuffer),

    E("AAudio_createStreamBuilder",                 stub_AAudio_createStreamBuilder),
    E("AAudioStreamBuilder_setChannelCount",        stub_AAudioStreamBuilder_setChannelCount),
    E("AAudioStreamBuilder_setSampleRate",          stub_AAudioStreamBuilder_setSampleRate),
    E("AAudioStreamBuilder_setFormat",              stub_AAudioStreamBuilder_setFormat),
    E("AAudioStreamBuilder_setDirection",           stub_AAudioStreamBuilder_setDirection),
    E("AAudioStreamBuilder_setSharingMode",         stub_AAudioStreamBuilder_setSharingMode),
    E("AAudioStreamBuilder_setPerformanceMode",     stub_AAudioStreamBuilder_setPerformanceMode),
    E("AAudioStreamBuilder_setBufferCapacityInFrames", stub_AAudioStreamBuilder_setBufferCapacityInFrames),
    E("AAudioStreamBuilder_openStream",             stub_AAudioStreamBuilder_openStream),
    E("AAudioStreamBuilder_delete",                 stub_AAudioStreamBuilder_delete),
    E("AAudioStream_requestStart",                  stub_AAudioStream_requestStart),
    E("AAudioStream_requestStop",                   stub_AAudioStream_requestStop),
    E("AAudioStream_close",                         stub_AAudioStream_close),
    E("AAudioStream_getFramesPerBurst",             stub_AAudioStream_getFramesPerBurst),
    E("AAudioStream_setBufferSizeInFrames",         stub_AAudioStream_setBufferSizeInFrames),
    E("AAudioStream_waitForStateChange",            stub_AAudioStream_waitForStateChange),
    E("AAudioStream_read",                          stub_AAudioStream_read),

    E("AndroidBitmap_getInfo",          stub_AndroidBitmap_getInfo),
    E("AndroidBitmap_lockPixels",       stub_AndroidBitmap_lockPixels),
    E("AndroidBitmap_unlockPixels",     stub_AndroidBitmap_unlockPixels),
    E("AHardwareBuffer_describe",       stub_AHardwareBuffer_describe),

    E("epoll_create1",                  stub_epoll_create1),
    E("epoll_ctl",                      stub_epoll_ctl),
    E("epoll_wait",                     stub_epoll_wait),
    E("eventfd",                        stub_eventfd),
    E("timerfd_create",                 stub_timerfd_create),
    E("timerfd_settime",                stub_timerfd_settime),
    E("dl_iterate_phdr",                stub_dl_iterate_phdr),
    E("__register_atfork",              stub_register_atfork),
    E("__sched_cpualloc",               stub_sched_cpualloc),
    E("__sched_cpucount",               stub_sched_cpucount),
    E("__sched_cpufree",                stub_sched_cpufree),
    E("__ctype_get_mb_cur_max",         stub_ctype_get_mb_cur_max),
    E("__gnu_strerror_r",               stub_gnu_strerror_r),
    E("__sF",                           __sF),

    // errno, math, memory
    E("__errno",                         stub_errno),
    E("sincos",                          stub_sincos),
    E("sincosf",                         stub_sincosf),
    E("memalign",                        stub_memalign),
    E("malloc_usable_size",              stub_malloc_usable_size),
    E("__cxa_thread_atexit_impl",        stub_cxa_thread_atexit_impl),

    // Linux scheduling / process
    E("sched_setaffinity",               stub_sched_setaffinity),
    E("sched_getaffinity",               stub_sched_getaffinity),
    E("prctl",                           stub_prctl),
    E("sysinfo",                         stub_sysinfo),
    E("getauxval",                       stub_getauxval),
    E("tgkill",                          stub_tgkill),
    E("posix_fadvise",                   stub_posix_fadvise),
    E("sem_timedwait",                   stub_sem_timedwait),
    E("mremap",                          stub_mremap),

    // Timers
    E("timer_create",                    stub_timer_create),
    E("timer_settime",                   stub_timer_settime),
    E("timer_delete",                    stub_timer_delete),

    // AStatus (AIDL)
    E("AStatus_isOk",                    stub_AStatus_isOk),
    E("AStatus_getExceptionCode",        stub_AStatus_getExceptionCode),
    E("AStatus_getServiceSpecificError", stub_AStatus_getServiceSpecificError),
    E("AStatus_getMessage",              stub_AStatus_getMessage),
    E("AStatus_getDescription",          stub_AStatus_getDescription),
    E("AStatus_deleteDescription",       stub_AStatus_deleteDescription),

    // stdio symbols — point into our fake Bionic __sF buffer
    E("stdin",                           &__sF[0 * BIONIC_FILE_SIZE]),
    E("stdout",                          &__sF[1 * BIONIC_FILE_SIZE]),
    E("stderr",                          &__sF[2 * BIONIC_FILE_SIZE]),

    // stdio function wrappers — intercept before dlsym finds macOS versions
    E("fwrite",                          stub_fwrite),
    E("fread",                           stub_fread),
    E("fflush",                          stub_fflush),
    E("fputs",                           stub_fputs),
    E("fputc",                           stub_fputc),
    E("fprintf",                         stub_fprintf),
    E("vfprintf",                        stub_vfprintf),

    // pthread wrappers — auto-init zero-filled mutexes/condvars for Linux compat
    E("pthread_mutex_lock",              stub_pthread_mutex_lock),
    E("pthread_mutex_unlock",            stub_pthread_mutex_unlock),
    E("pthread_mutex_trylock",           stub_pthread_mutex_trylock),
    E("pthread_mutex_init",              stub_pthread_mutex_init),
    E("pthread_mutex_destroy",           stub_pthread_mutex_destroy),
    E("pthread_cond_wait",               stub_pthread_cond_wait),
    E("pthread_cond_signal",             stub_pthread_cond_signal),
    E("pthread_cond_broadcast",          stub_pthread_cond_broadcast),
    E("pthread_cond_timedwait",          stub_pthread_cond_timedwait),
    E("pthread_once",                    stub_pthread_once),
    E("pthread_rwlock_rdlock",           stub_pthread_rwlock_rdlock),
    E("pthread_rwlock_wrlock",           stub_pthread_rwlock_wrlock),
    E("pthread_rwlock_unlock",           stub_pthread_rwlock_unlock),
    E("pthread_rwlock_tryrdlock",        stub_pthread_rwlock_tryrdlock),
    E("pthread_rwlock_trywrlock",        stub_pthread_rwlock_trywrlock),

    // ICU data
    E("uprv_getICUData_brkitr_char",     stub_uprv_getICUData),
    E("uprv_getICUData_brkitr",          stub_uprv_getICUData),
    E("uprv_getICUData_collation",       stub_uprv_getICUData),
    E("uprv_getICUData_conversion",      stub_uprv_getICUData),
    E("uprv_getICUData_core",            stub_uprv_getICUData),
    E("uprv_getICUData_likely",          stub_uprv_getICUData),
    E("uprv_getICUData_locale",          stub_uprv_getICUData),
    E("uprv_getICUData_nfkccf",          stub_uprv_getICUData),
    E("uprv_getICUData_translit",        stub_uprv_getICUData),
    E("uprv_getICUData_tz",              stub_uprv_getICUData),
    E("uprv_getICUData_other",           stub_uprv_getICUData),
    E("uprv_getICUData_custom",          stub_uprv_getICUData),

    // Thread-local storage symbols (provide zero-initialized static vars)
    E("_ZTHN4util5cache21global_sampling_stateE", stub_noop),
    E("_ZTHN6thread5local8internal3Var21per_thread_instances_E", stub_noop),
    E("_ZTHN3re25hooks7contextE", stub_noop),

    // dlopen/dlsym intercept for data bundle .so files
    E("dlopen",                          stub_dlopen),
    E("dlsym",                           stub_dlsym),
    E("dlclose",                         stub_dlclose),
    E("dlerror",                         stub_dlerror),

    // Weak/optional — no-ops
    E("ProcessInactiveCoThreadTracesImpl",  stub_noop),
    E("stats_census_experimental_export_root_scoped_data_signal_safe", stub_noop),
    E("_ZN4base33HasDuplicateGlobalSymbolsInternalEv", stub_noop_ret0),
    E("_ZN4absl19leak_check_internal12DoIgnoreLeakEPKv", stub_noop),
    E("_ZN4absl19leak_check_internal17DisableLeakChecksEPPi", stub_noop),
    E("_ZN4absl19leak_check_internal16EnableLeakChecksEPi", stub_noop),
    E("__gcov_dump",                     stub_noop),
    E("__gcov_flush",                    stub_noop),
    E("MallocExtension_Internal_MarkThreadIdle", stub_noop),
    E("MallocExtension_Internal_MarkThreadBusy", stub_noop),
    E("MallocExtension_Internal_GetNumericProperty", stub_noop_ret0),
    E("MallocExtension_Internal_ProcessBackgroundActions", stub_noop),
    E("x_cgo_init",                      stub_noop),

    { NULL, NULL }
};

const SymEntry *android_stubs_table(void) { return s_table; }
