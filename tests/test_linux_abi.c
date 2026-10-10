// Unit tests for the Linux/Bionic ABI shims in linux_abi.c. Stubs are
// looked up by name the same way elf_loader resolves .so imports
// (linux_abi table first).

#include "engine/android_stubs.h"
#include "engine/linux_abi.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <xlocale.h>

static int s_pass, s_fail;

#define CHECK(cond) do { \
    if (cond) { s_pass++; } \
    else { s_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void *sym(const char *name) {
    const SymEntry *tables[] = { linux_abi_table(), android_stubs_table() };
    for (int t = 0; t < 2; t++) {
        for (const SymEntry *e = tables[t]; e->name; e++) {
            if (strcmp(e->name, name) == 0) return e->addr;
        }
    }
    fprintf(stderr, "missing stub %s\n", name);
    abort();
}

#define STUB(type, name) ((type)sym(name))

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

// ── errno ────────────────────────────────────────────────────────────────────

static void test_errno(void) {
    CHECK(linux_errno_from_darwin(EINVAL) == 22);
    CHECK(linux_errno_from_darwin(EAGAIN) == 11);
    CHECK(linux_errno_from_darwin(EDEADLK) == 35);
    CHECK(linux_errno_from_darwin(ETIMEDOUT) == 110);
    CHECK(linux_errno_from_darwin(ENOSYS) == 38);
    CHECK(linux_errno_from_darwin(ENOTSUP) == 95);
    CHECK(linux_errno_from_darwin(ECONNREFUSED) == 111);
}

// ── futex via syscall(98) ────────────────────────────────────────────────────

typedef long (*SyscallFn)(long, long, long, long, long, long, long);
typedef struct { int64_t sec, nsec; } LTs;

static _Atomic uint32_t s_futex_word;
static SyscallFn s_syscall;

static void *futex_waker(void *arg) {
    (void)arg;
    usleep(50000);
    atomic_store(&s_futex_word, 1);
    s_syscall(98, (long)&s_futex_word, 1 | 128, 1, 0, 0, 0);
    return NULL;
}

static void test_futex(void) {
    s_syscall = STUB(SyscallFn, "syscall");
    atomic_store(&s_futex_word, 0);

    // Value mismatch → EAGAIN
    errno = 0;
    CHECK(s_syscall(98, (long)&s_futex_word, 0, 5, 0, 0, 0) == -1);
    CHECK(errno == 11);

    // Relative timeout → ETIMEDOUT
    LTs ts = { 0, 30 * 1000000 };
    int64_t t0 = now_ms();
    errno = 0;
    CHECK(s_syscall(98, (long)&s_futex_word, 0 | 128, 0, (long)&ts, 0, 0) == -1);
    CHECK(errno == 110);
    CHECK(now_ms() - t0 >= 25);

    // FUTEX_WAIT_BITSET with an absolute monotonic deadline in the past
    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    LTs past = { mono.tv_sec - 1, 0 };
    errno = 0;
    CHECK(s_syscall(98, (long)&s_futex_word, 9, 0, (long)&past, 0, ~0L) == -1);
    CHECK(errno == 110);

    // Wake from another thread
    pthread_t th;
    pthread_create(&th, NULL, futex_waker, NULL);
    long r = s_syscall(98, (long)&s_futex_word, 0 | 128, 0, 0, 0, 0);
    CHECK(r == 0);
    CHECK(atomic_load(&s_futex_word) == 1);
    pthread_join(th, NULL);

    // Wake counts: nobody waiting → 0; WAKE_BITSET with mask 0 → EINVAL
    CHECK(s_syscall(98, (long)&s_futex_word, 1 | 128, 10, 0, 0, 0) == 0);
    errno = 0;
    CHECK(s_syscall(98, (long)&s_futex_word, 10, 1, 0, 0, 0) == -1 && errno == 22);
    errno = 0;
    CHECK(s_syscall(98, (long)&s_futex_word, 4, 1, 1, 0, 99) == -1 && errno == 11);
    CHECK(s_syscall(98, (long)&s_futex_word, 4, 1, 1, 0, 1) == 0);

    // Unknown syscalls report Linux ENOSYS
    errno = 0;
    CHECK(s_syscall(279, 0, 0, 0, 0, 0, 0) == -1);
    CHECK(errno == 38);
    CHECK(s_syscall(178, 0, 0, 0, 0, 0, 0) > 0);  // gettid
    uint8_t rnd[16] = {0};
    CHECK(s_syscall(278, (long)rnd, sizeof(rnd), 0, 0, 0, 0) == 16);
}

// ── semaphores ───────────────────────────────────────────────────────────────

typedef int (*SemFn)(void *);
typedef int (*SemInitFn)(void *, int, unsigned);
typedef int (*SemTimedFn)(void *, const LTs *);

static uint32_t s_sem[4];

static void *sem_poster(void *arg) {
    (void)arg;
    usleep(30000);
    STUB(SemFn, "sem_post")(s_sem);
    return NULL;
}

static void test_semaphores(void) {
    SemInitFn init = STUB(SemInitFn, "sem_init");
    SemFn wait = STUB(SemFn, "sem_wait");
    SemFn trywait = STUB(SemFn, "sem_trywait");
    SemFn post = STUB(SemFn, "sem_post");
    SemTimedFn timedwait = STUB(SemTimedFn, "sem_timedwait");

    CHECK(init(s_sem, 0, 1) == 0);
    CHECK(trywait(s_sem) == 0);
    errno = 0;
    CHECK(trywait(s_sem) == -1 && errno == 11);
    CHECK(post(s_sem) == 0);
    CHECK(wait(s_sem) == 0);

    // Bionic SEM_VALUE_MAX is 0x3fffffff
    uint32_t big[4];
    errno = 0;
    CHECK(init(big, 0, 0x40000000) == -1 && errno == 22);
    CHECK(init(big, 0, 0x3fffffff) == 0);
    errno = 0;
    CHECK(post(big) == -1 && errno == 75);

    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    LTs deadline = { rt.tv_sec, rt.tv_nsec + 20000000 };
    if (deadline.nsec >= 1000000000) { deadline.sec++; deadline.nsec -= 1000000000; }
    errno = 0;
    CHECK(timedwait(s_sem, &deadline) == -1 && errno == 110);

    pthread_t th;
    pthread_create(&th, NULL, sem_poster, NULL);
    CHECK(wait(s_sem) == 0);
    pthread_join(th, NULL);
}

// ── open / fcntl / stat / readdir ────────────────────────────────────────────

typedef int (*OpenFn)(const char *, int, unsigned);
typedef int (*FcntlFn)(int, int, long);
typedef int (*StatFn)(const char *, void *);
typedef int (*FstatFn)(int, void *);

typedef struct {
    int16_t type, whence;
    int64_t start, len;
    int32_t pid;
} LFlock;

static void test_files(const char *dir) {
    OpenFn open_ = STUB(OpenFn, "open");
    FcntlFn fcntl_ = STUB(FcntlFn, "fcntl");
    char path[1024];
    snprintf(path, sizeof(path), "%s/compat_file", dir);

    // O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC in Linux encoding
    int fd = open_(path, 1 | 0x40 | 0x200 | 0x80000, 0640);
    CHECK(fd >= 0);
    CHECK(write(fd, "hello", 5) == 5);
    CHECK(fcntl_(fd, 1, 0) == 1);                     // F_GETFD → FD_CLOEXEC
    CHECK((fcntl_(fd, 3, 0) & 3) == 1);               // F_GETFL → O_WRONLY
    CHECK(fcntl_(fd, 4, 0x800) == 0);                 // F_SETFL O_NONBLOCK
    CHECK(fcntl_(fd, 3, 0) & 0x800);
    CHECK(fcntl_(fd, 1030, 100) >= 100);              // F_DUPFD_CLOEXEC
    errno = 0;
    CHECK(fcntl_(fd, 9999, 0) == -1 && errno == 22);

    LFlock lk = { 1, 0, 0, 0, 0 };                    // F_WRLCK whole file
    CHECK(fcntl_(fd, 6, (long)&lk) == 0);             // F_SETLK
    lk.type = 2;                                      // F_UNLCK
    CHECK(fcntl_(fd, 6, (long)&lk) == 0);
    close(fd);

    // O_CREAT|O_EXCL on an existing file → EEXIST
    errno = 0;
    CHECK(open_(path, 1 | 0x40 | 0x80, 0640) == -1 && errno == 17);

    uint8_t st[128];
    memset(st, 0xAB, sizeof(st));
    CHECK(STUB(StatFn, "stat")(path, st) == 0);
    CHECK(*(int64_t *)(st + 48) == 5);                // st_size
    CHECK((*(uint32_t *)(st + 16) & 0170777) == (0100000 | 0640));  // st_mode
    fd = open(path, O_RDONLY);
    memset(st, 0, sizeof(st));
    CHECK(STUB(FstatFn, "fstat")(fd, st) == 0);
    CHECK(*(int64_t *)(st + 48) == 5);
    close(fd);
    errno = 0;
    CHECK(STUB(StatFn, "stat")("/nonexistent/x", st) == -1 && errno == 2);

    void *(*opendir_)(const char *) = sym("opendir");
    uint8_t *(*readdir_)(void *) = sym("readdir");
    int (*closedir_)(void *) = sym("closedir");
    void *d = opendir_(dir);
    CHECK(d != NULL);
    int found = 0;
    for (uint8_t *e; d && (e = readdir_(d)); ) {
        if (strcmp((char *)e + 19, "compat_file") == 0) {
            found = 1;
            CHECK(e[18] == 8);                        // DT_REG
            CHECK(*(uint16_t *)(e + 16) == 280);
        }
    }
    CHECK(found);
    if (d) CHECK(closedir_(d) == 0);
    unlink(path);
}

static void test_uname_sysconf(void) {
    char uts[6 * 65];
    CHECK(STUB(int (*)(void *), "uname")(uts) == 0);
    CHECK(strcmp(uts, "Linux") == 0);
    CHECK(strcmp(uts + 4 * 65, "aarch64") == 0);

    long (*sysconf_)(int) = sym("sysconf");
    CHECK(sysconf_(0x27) == getpagesize());
    CHECK(sysconf_(0x61) >= 1);
    CHECK(sysconf_(0x92) >= 0);
    errno = 0;
    CHECK(sysconf_(0x7777) == -1 && errno == 22);
}

// ── signals ──────────────────────────────────────────────────────────────────

typedef struct { int flags; void *handler; uint64_t mask; void *restorer; } LSigaction;

static void dummy_handler(int sig) { (void)sig; }

static void test_signals(void) {
    uint64_t set;
    CHECK(STUB(int (*)(uint64_t *), "sigemptyset")(&set) == 0 && set == 0);
    CHECK(STUB(int (*)(uint64_t *, int), "sigaddset")(&set, 10) == 0);
    CHECK(set == (1ULL << 9));
    CHECK(STUB(int (*)(const uint64_t *, int), "sigismember")(&set, 10) == 1);
    CHECK(STUB(int (*)(uint64_t *, int), "sigdelset")(&set, 10) == 0 && set == 0);
    errno = 0;
    CHECK(STUB(int (*)(uint64_t *, int), "sigaddset")(&set, 65) == -1 && errno == 22);

    // sigaction is recorded but never installed for real
    int (*sigaction_)(int, const LSigaction *, LSigaction *) = sym("sigaction");
    LSigaction act = { 0, (void *)dummy_handler, 0, NULL }, old;
    struct sigaction real_before, real_after;
    sigaction(SIGUSR1, NULL, &real_before);
    CHECK(sigaction_(10, &act, NULL) == 0);
    CHECK(sigaction_(10, NULL, &old) == 0 && old.handler == (void *)dummy_handler);
    sigaction(SIGUSR1, NULL, &real_after);
    CHECK(real_after.sa_handler == real_before.sa_handler);
    errno = 0;
    CHECK(sigaction_(9, &act, NULL) == -1 && errno == 22);

    // A virtual SIGPIPE handler leaves the real disposition alone; only
    // "ignore" is applied for real.
    signal(SIGPIPE, SIG_IGN);
    act.handler = (void *)dummy_handler;
    CHECK(sigaction_(13, &act, NULL) == 0);
    sigaction(SIGPIPE, NULL, &real_after);
    CHECK(real_after.sa_handler == SIG_IGN);
    signal(SIGPIPE, SIG_DFL);
    act.handler = (void *)1;
    CHECK(sigaction_(13, &act, NULL) == 0);
    sigaction(SIGPIPE, NULL, &real_after);
    CHECK(real_after.sa_handler == SIG_IGN);
    signal(SIGPIPE, SIG_DFL);

    // Linux SIG_BLOCK(0) of SIGUSR1(10) blocks macOS SIGUSR1(30); SIGSEGV
    // is never blocked.
    int (*sigmask_)(int, const uint64_t *, uint64_t *) = sym("pthread_sigmask");
    uint64_t block = (1ULL << 9) | (1ULL << 10), prev = 0, cur = 0;
    CHECK(sigmask_(0, &block, &prev) == 0);
    sigset_t real;
    pthread_sigmask(SIG_BLOCK, NULL, &real);
    CHECK(sigismember(&real, SIGUSR1));
    CHECK(!sigismember(&real, SIGSEGV));
    CHECK(sigmask_(0, NULL, &cur) == 0);
    CHECK((cur & (1ULL << 9)) && !(cur & (1ULL << 10)));
    CHECK(sigmask_(2, &prev, NULL) == 0);
    CHECK(sigmask_(7, &block, NULL) == 22);

    CHECK(STUB(int (*)(pid_t, int), "kill")(getpid(), 10) == 0);  // swallowed
    CHECK(STUB(int (*)(pid_t, int), "kill")(getpid(), 0) == 0);
}

// ── pthread shims ────────────────────────────────────────────────────────────

static void test_keys_attrs(void) {
    int key = -1;
    CHECK(STUB(int (*)(int *, void (*)(void *)), "pthread_key_create")(&key, NULL) == 0);
    CHECK(key >= 0);
    CHECK(STUB(int (*)(int, const void *), "pthread_setspecific")(key, &key) == 0);
    CHECK(STUB(void *(*)(int), "pthread_getspecific")(key) == &key);
    CHECK(STUB(int (*)(int), "pthread_key_delete")(key) == 0);

    uint8_t attr[56];
    CHECK(STUB(int (*)(void *), "pthread_attr_init")(attr) == 0);
    CHECK(*(size_t *)(attr + 16) == 1024 * 1024);
    CHECK(STUB(int (*)(void *, size_t), "pthread_attr_setstacksize")(attr, 100) == 22);
    CHECK(STUB(int (*)(void *, size_t), "pthread_attr_setstacksize")(attr, 65536) == 0);
    CHECK(STUB(int (*)(void *, int), "pthread_attr_setdetachstate")(attr, 1) == 0);
    CHECK(STUB(int (*)(void *, int), "pthread_attr_setscope")(attr, 1) == 95);

    pthread_t th;
    CHECK(STUB(int (*)(pthread_t *, void *, void *(*)(void *), void *),
               "pthread_create")(&th, attr, (void *(*)(void *))usleep, 0) == 0);
    CHECK(pthread_join(th, NULL) != 0);  // detached
    CHECK(STUB(int (*)(pthread_t, const char *), "pthread_setname_np")
              (pthread_self(), "this-name-is-too-long") == 34);
}

static void test_mutexes(void) {
    int (*attr_init)(long *) = sym("pthread_mutexattr_init");
    int (*attr_settype)(long *, int) = sym("pthread_mutexattr_settype");
    int (*mutex_init)(void *, const long *) = sym("pthread_mutex_init");
    int (*lock)(void *) = sym("pthread_mutex_lock");
    int (*trylock)(void *) = sym("pthread_mutex_trylock");
    int (*unlock)(void *) = sym("pthread_mutex_unlock");
    int (*destroy)(void *) = sym("pthread_mutex_destroy");

    // Recursive mutex via attr
    uint8_t m[40];
    long attr;
    CHECK(attr_init(&attr) == 0);
    CHECK(attr_settype(&attr, 1) == 0);
    CHECK(attr_settype(&attr, 7) == 22);
    CHECK(mutex_init(m, &attr) == 0);
    CHECK(lock(m) == 0);
    CHECK(lock(m) == 0);
    CHECK(unlock(m) == 0);
    CHECK(unlock(m) == 0);
    CHECK(destroy(m) == 0);

    // Statically initialized recursive mutex (bit 14 of the state word)
    uint8_t sm[40] = {0};
    *(uint16_t *)sm = 1 << 14;
    CHECK(lock(sm) == 0);
    CHECK(trylock(sm) == 0);
    CHECK(unlock(sm) == 0);
    CHECK(unlock(sm) == 0);
    destroy(sm);

    // Normal mutex: trylock while held reports Linux EBUSY (16)
    uint8_t nm[40] = {0};
    CHECK(mutex_init(nm, NULL) == 0);
    CHECK(lock(nm) == 0);
    CHECK(trylock(nm) == 16);
    CHECK(unlock(nm) == 0);

    // Re-init after use replaces the shadow (an errorcheck mutex now)
    attr_settype(&attr, 2);
    CHECK(mutex_init(nm, &attr) == 0);
    CHECK(unlock(nm) == 1);  // EPERM: not owner
    destroy(nm);

    // A destroyed, re-zeroed mutex works again (lazily recreated).
    CHECK(lock(nm) == 0 && unlock(nm) == 0);

    // The host pointer lives in the next 8-aligned slot after the first
    // 4 bytes, also for 4-byte aligned objects.
    uint64_t raw[8] = {0};
    uint8_t *m4 = (uint8_t *)raw + 4;
    CHECK(lock(m4) == 0 && unlock(m4) == 0);
    CHECK(raw[0] == 0 && raw[1] != 0);
    destroy(m4);
    CHECK(raw[1] == 0);

    // Churn: many create/destroy cycles must not leak or fail.
    uint8_t *pool = calloc(64, 40);
    for (int round = 0; round < 600; round++) {
        for (int i = 0; i < 64; i++) {
            void *p = pool + i * 40;
            CHECK(mutex_init(p, NULL) == 0);
        }
        for (int i = 0; i < 64; i++) destroy(pool + i * 40);
    }
    // Distinct addresses each round
    uint8_t *big = calloc(20000, 40);
    int ok = 1;
    for (int i = 0; i < 20000; i++) {
        if (mutex_init(big + i * 40, NULL) != 0) ok = 0;
        destroy(big + i * 40);
    }
    CHECK(ok);
    free(big);
    free(pool);
}

static uint8_t s_race_mutex[40];
static int s_race_count;

static void *race_worker(void *arg) {
    int (*lock)(void *) = sym("pthread_mutex_lock");
    int (*unlock)(void *) = sym("pthread_mutex_unlock");
    (void)arg;
    for (int i = 0; i < 10000; i++) {
        lock(s_race_mutex);
        s_race_count++;
        unlock(s_race_mutex);
    }
    return NULL;
}

// Threads racing to create the host object of a static mutex must all
// end up on the same one.
static void test_mutex_race(void) {
    pthread_t t[8];
    for (int i = 0; i < 8; i++) pthread_create(&t[i], NULL, race_worker, NULL);
    for (int i = 0; i < 8; i++) pthread_join(t[i], NULL);
    CHECK(s_race_count == 80000);
}

static _Atomic int s_once_calls;
static uint32_t s_once_flag;
static void once_init(void) {
    usleep(20000);
    atomic_fetch_add(&s_once_calls, 1);
}
static void *once_thread(void *arg) {
    (void)arg;
    STUB(int (*)(void *, void (*)(void)), "pthread_once")(&s_once_flag, once_init);
    CHECK(atomic_load(&s_once_calls) == 1);
    return NULL;
}

static uint32_t s_rec_flag;
static void recursive_init(void) {
    STUB(int (*)(void *, void (*)(void)), "pthread_once")(&s_rec_flag, recursive_init);
}

static sigjmp_buf s_crash_jmp;
static int s_crash_mode, s_crash_runs;
static uint32_t s_crash_flag;
static void crashing_init(void) {
    s_crash_runs++;
    if (s_crash_mode) siglongjmp(s_crash_jmp, 1);
}

static uint32_t *s_late_flag;
static void late_init(void) {
    // Simulates a crash after completion was published but before the
    // once is popped: publish manually, then recover and longjmp out.
    __atomic_store_n(s_late_flag, 2, __ATOMIC_SEQ_CST);
    linux_abi_crash_recovered();
    siglongjmp(s_crash_jmp, 1);
}

static void test_once(void) {
    pthread_t th[8];
    for (int i = 0; i < 8; i++) pthread_create(&th[i], NULL, once_thread, NULL);
    for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
    CHECK(atomic_load(&s_once_calls) == 1);
    CHECK(s_once_flag == 2);  // Bionic "complete"

    STUB(int (*)(void *, void (*)(void)), "pthread_once")(&s_rec_flag, recursive_init);
    CHECK(s_rec_flag == 2);

    // A control word already complete must not run the initializer.
    uint32_t done = 2;
    atomic_store(&s_once_calls, 0);
    STUB(int (*)(void *, void (*)(void)), "pthread_once")(&done, once_init);
    CHECK(atomic_load(&s_once_calls) == 0);

    // Crash recovery resets the in-progress control so it can be retried.
    s_crash_mode = 1;
    if (sigsetjmp(s_crash_jmp, 0) == 0) {
        STUB(int (*)(void *, void (*)(void)), "pthread_once")(&s_crash_flag, crashing_init);
    } else {
        linux_abi_crash_recovered();
    }
    CHECK(s_crash_flag == 0);
    s_crash_mode = 0;
    STUB(int (*)(void *, void (*)(void)), "pthread_once")(&s_crash_flag, crashing_init);
    CHECK(s_crash_flag == 2 && s_crash_runs == 2);

    // Recovery after the flag is already published leaves it complete.
    s_crash_mode = 2;
    uint32_t late = 0;
    s_late_flag = &late;
    if (sigsetjmp(s_crash_jmp, 0) == 0) {
        STUB(int (*)(void *, void (*)(void)), "pthread_once")(&late, late_init);
    }
    CHECK(late == 2);
    s_crash_mode = 0;
}

// ── scanf, mmap, rlimit, time, locale ───────────────────────────────────────

typedef struct {
    void *stack, *gr_top, *vr_top;
    int32_t gr_offs, vr_offs;
} LVaList;

static void test_scanf(void) {
    // Eight pointers: six in registers, two on the stack.
    typedef int (*SscanfFn)(const char *, const char *, void *, void *,
                            void *, void *, void *, void *, void *, void *,
                            void *, void *, void *, void *, void *, void *,
                            void *, void *, void *, void *, void *, void *,
                            void *, void *, void *, void *, void *, void *,
                            void *, void *, void *, void *, void *, void *);
    int v[8] = {0};
    char word[16] = {0};
    SscanfFn sscanf_ = STUB(SscanfFn, "sscanf");
    int n = sscanf_("1 2 3 4 5 6 7 abc", "%d %d %d %d %d %d %*s%n %d",
                    &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    0, 0, 0, 0, 0, 0);
    CHECK(n == 6 && v[0] == 1 && v[5] == 6);
    n = sscanf_("1 2 3 4 5 6 7 abc", "%d %d %d %d %d %d %d %15s",
                &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], word,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0);
    CHECK(n == 8 && v[6] == 7 && strcmp(word, "abc") == 0);

    // Positional conversions; positions past the slot limit are rejected.
    v[0] = v[1] = 0;
    n = sscanf_("5 9", "%2$d %1$d", &v[0], &v[1], 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0);
    CHECK(n == 2 && v[0] == 9 && v[1] == 5);
    errno = 0;
    n = sscanf_("1", "%40$d", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    CHECK(n == -1 && errno == 22);

    // vsscanf with a hand-built AAPCS64 va_list: two register slots
    // remaining, the third argument on the stack.
    int a = 0, b = 0, c = 0;
    void *gr_save[8] = { NULL, NULL, NULL, NULL, NULL, NULL, &a, &b };
    void *stack[1] = { &c };
    LVaList va = { stack, gr_save + 8, NULL, -16, 0 };
    int (*vsscanf_)(const char *, const char *, LVaList *) = sym("vsscanf");
    CHECK(vsscanf_("10 20 30", "%d %d %d", &va) == 3);
    CHECK(a == 10 && b == 20 && c == 30);
    CHECK(va.gr_offs == -16);  // caller's va_list untouched
}

// Calls through non-variadic prototypes place arguments exactly where an
// AAPCS64 variadic caller would: integers in x-registers, doubles in
// v-registers, the overflow in 8-byte stack slots.
typedef uint8_t Quad __attribute__((vector_size(16)));

static void test_printf(void) {
    char buf[128];
    typedef int (*Sn1)(char *, size_t, const char *, int64_t, const char *,
                       int64_t, int64_t, int64_t, double, double, int64_t);
    Sn1 sn1 = STUB(Sn1, "snprintf");
    int n = sn1(buf, sizeof(buf), "%d %s %lld %x %c|%.2f %g|%d",
                1, "ab", 1LL << 40, 255, 'z', 3.14159, 2.5, 42);
    CHECK(strcmp(buf, "1 ab 1099511627776 ff z|3.14 2.5|42") == 0);
    CHECK(n == (int)strlen(buf));

    // Nine doubles: eight in v-registers, the ninth on the stack.
    typedef int (*Sn2)(char *, size_t, const char *, double, double, double,
                       double, double, double, double, double, double);
    Sn2 sn2 = STUB(Sn2, "snprintf");
    sn2(buf, sizeof(buf), "%g %g %g %g %g %g %g %g %g",
        1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.5);
    CHECK(strcmp(buf, "1 2 3 4 5 6 7 8 9.5") == 0);

    // Positional and '*' arguments.
    typedef int (*Sn3)(char *, size_t, const char *, int64_t, int64_t);
    Sn3 sn3 = STUB(Sn3, "snprintf");
    sn3(buf, sizeof(buf), "%2$s-%1$d", 7, (int64_t)(intptr_t)"q");
    CHECK(strcmp(buf, "q-7") == 0);
    sn3(buf, sizeof(buf), "[%*d]", 5, 42);
    CHECK(strcmp(buf, "[   42]") == 0);

    // long double is binary128 in q0 on Linux: 1.5 = 0x3fff8000...0.
    typedef int (*Sn4)(char *, size_t, const char *, Quad);
    Sn4 sn4 = STUB(Sn4, "snprintf");
    Quad q = {0};
    q[15] = 0x3f; q[14] = 0xff; q[13] = 0x80;
    sn4(buf, sizeof(buf), "%.2Lf", q);
    CHECK(strcmp(buf, "1.50") == 0);

    typedef int (*Asp)(char **, const char *, const char *, int64_t);
    Asp asp = STUB(Asp, "asprintf");
    char *out = NULL;
    CHECK(asp(&out, "%s=%d", "k", 9) == 3 && out && strcmp(out, "k=9") == 0);
    free(out);

    // vsnprintf with a hand-built va_list: one int left in registers,
    // one double in v-registers, then one int and one double on the stack.
    uint64_t gr_save[8] = { 0, 0, 0, 0, 0, 0, 0, 11 };
    uint8_t vr_save[128] = {0};
    double d = 0.25;
    memcpy(vr_save + 112, &d, 8);
    uint64_t stack[2] = { 33, 0 };
    double d2 = 4.5;
    memcpy(&stack[1], &d2, 8);
    LVaList va = { stack, gr_save + 8, vr_save + 128, -8, -16 };
    int (*vsn)(char *, size_t, const char *, LVaList *) = sym("vsnprintf");
    vsn(buf, sizeof(buf), "%d %g %d %g", &va);
    CHECK(strcmp(buf, "11 0.25 33 4.5") == 0);
    CHECK(va.gr_offs == -8);  // caller's va_list untouched

    // %n in a writable format would make Darwin abort; it is rejected.
    char fmt_n[] = "ab%n";
    int written = -1;
    errno = 0;
    CHECK(sn3(buf, sizeof(buf), fmt_n, (int64_t)(intptr_t)&written, 0) == -1);
    CHECK(errno == 22 && written == -1 && buf[0] == '\0');

    // Positional width argument.
    sn3(buf, sizeof(buf), "%1$*2$d|", 42, 5);
    CHECK(strcmp(buf, "   42|") == 0);

    // Positions beyond the slot limit are rejected.
    errno = 0;
    CHECK(sn3(buf, sizeof(buf), "%65$d", 1, 2) == -1 && errno == 22);

    // long double after v-registers run out: the ninth double takes stack
    // slot 0, the long double is 16-byte aligned at stack offset 16.
    typedef int (*Sn5)(char *, size_t, const char *, double, double, double,
                       double, double, double, double, double, double, Quad);
    Sn5 sn5 = STUB(Sn5, "snprintf");
    sn5(buf, sizeof(buf), "%g %g %g %g %g %g %g %g %g %.1Lf",
        1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, q);
    CHECK(strcmp(buf, "1 2 3 4 5 6 7 8 9 1.5") == 0);

    // Host errors are reported with Linux errno values (EILSEQ = 84).
    wchar_t wide[] = { 0x4e2d, 0 };
    errno = 0;
    int r = sn3(buf, sizeof(buf), "%ls", (int64_t)(intptr_t)wide, 0);
    CHECK(r == -1 && errno == 84);
}

static char *read_all(FILE *fp) {
    fflush(fp);
    long size = ftell(fp);
    char *buf = calloc(1, (size_t)size + 1);
    rewind(fp);
    fread(buf, 1, (size_t)size, fp);
    return buf;
}

static char *capture_stderr(void (*fn)(void)) {
    FILE *tmp = tmpfile();
    int saved = dup(STDERR_FILENO);
    fflush(stderr);
    dup2(fileno(tmp), STDERR_FILENO);
    fn();
    dup2(saved, STDERR_FILENO);
    close(saved);
    fseek(tmp, 0, SEEK_END);
    char *out = read_all(tmp);
    fclose(tmp);
    return out;
}

static void log_star_width(void) {
    typedef int (*Fn)(int64_t, const char *, const char *, int64_t, int64_t,
                      double, const char *);
    STUB(Fn, "__android_log_print")(4, "Tag", "%*.*f|%-5s|", 9, 3, 3.14159265, "ab");
}

static void log_many_args(void) {
    typedef int (*Fn)(int64_t, const char *, const char *,
                      int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                      int64_t, double, double, double, double, double, double,
                      double, double, double);
    STUB(Fn, "__android_log_print")(4, "T",
        "%ld%ld%ld%ld%ld%ld%ld %g %g %g %g %g %g %g %g %g",
        1, 2, 3, 4, 5, 6, 7, 0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5);
}

// Plain libSystem imports get their errno translated after the call.
static void test_errno_wrappers(void) {
    int fds[2];
    CHECK(pipe(fds) == 0);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    char c;
    errno = 0;
    CHECK(STUB(ssize_t (*)(int, void *, size_t), "read")(fds[0], &c, 1) == -1);
    CHECK(errno == 11);  // Linux EAGAIN (Darwin 35)

    // A prior Linux errno equal to the new Darwin value is still translated.
    errno = 35;          // Linux EDEADLK, numerically Darwin EAGAIN
    CHECK(STUB(ssize_t (*)(int, void *, size_t), "read")(fds[0], &c, 1) == -1);
    CHECK(errno == 11);

    // Unchanged errno is left alone, even values Darwin would remap.
    errno = 35;          // Linux EDEADLK
    CHECK(STUB(ssize_t (*)(int, const void *, size_t), "write")(fds[1], "x", 1) == 1);
    CHECK(errno == 35);
    close(fds[0]);
    close(fds[1]);

    // Return values in x0 and d0 survive the wrapper.
    char *end = NULL;
    double d = STUB(double (*)(const char *, char **), "strtod")("2.5x", &end);
    CHECK(d == 2.5 && end && *end == 'x');
    CHECK(STUB(long (*)(const char *, char **, int), "strtol")("-42", NULL, 10) == -42);

    char *(*strerror_)(int) = sym("strerror");
    CHECK(strcmp(strerror_(11), "Resource temporarily unavailable") == 0);
    char buf[64];
    CHECK(STUB(int (*)(int, char *, size_t), "strerror_r")(110, buf, sizeof(buf)) == 0);
    CHECK(strcmp(buf, "Operation timed out") == 0);
}

// strtold_l returns Linux binary128 in q0.
typedef uint64_t QuadBits __attribute__((vector_size(16)));
static void test_strtold(void) {
    QuadBits (*strtold_l_)(const char *, char **, locale_t) = sym("strtold_l");
    locale_t loc = newlocale(LC_ALL_MASK, "C", NULL);
    char *end = NULL;
    QuadBits q = strtold_l_("2.5x", &end, loc);
    CHECK(q[1] == 0x4000400000000000ULL && q[0] == 0 && *end == 'x');
    q = strtold_l_("-1", NULL, loc);
    CHECK(q[1] == 0xbfff000000000000ULL && q[0] == 0);
    q = strtold_l_("0.1", NULL, loc);  // double 0.1 widened exactly
    CHECK(q[1] == 0x3ffb999999999999ULL && q[0] == 0xa000000000000000ULL);
    q = strtold_l_("-0", NULL, loc);
    CHECK(q[1] == 0x8000000000000000ULL && q[0] == 0);
    q = strtold_l_("0x1p-1074", NULL, loc);  // double subnormal
    CHECK(q[1] == ((uint64_t)(16383 - 1074) << 48) && q[0] == 0);
    q = strtold_l_("inf", NULL, loc);
    CHECK(q[1] == 0x7fff000000000000ULL && q[0] == 0);
    q = strtold_l_("nan", NULL, loc);
    CHECK((q[1] >> 48) == 0x7fff && (q[1] & 0xffffffffffffULL) != 0);
    errno = 0;
    strtold_l_("1e999", NULL, loc);
    CHECK(errno == 34);  // ERANGE
    freelocale(loc);
}

// Bionic stdin/stdout/stderr are FILE* variables, not FILE objects.
static void test_stdio_vars(void) {
    FILE **in = sym("stdin"), **out = sym("stdout"), **err = sym("stderr");
    CHECK(*in && *out && *err);
    CHECK(android_stubs_fixup_file(*in) == stdin);
    CHECK(android_stubs_fixup_file(*out) == stdout);
    CHECK(android_stubs_fixup_file(*err) == stderr);
    CHECK(android_stubs_fixup_file(NULL) == NULL);  // fflush(NULL) flushes all
    CHECK(STUB(int (*)(FILE *), "fflush")(NULL) == 0);

    // Unshimmed-by-libc FILE functions accept the fake streams.
    CHECK(STUB(int (*)(FILE *), "fileno")(*in) == 0);
    CHECK(STUB(int (*)(FILE *), "fileno")(*err) == 2);
    CHECK(STUB(int (*)(FILE *), "ferror")(*out) == 0);
    STUB(void (*)(FILE *), "clearerr")(*out);

    // Regular streams still work through the shims.
    FILE *fp = tmpfile();
    fputs("ab", fp);
    STUB(void (*)(FILE *), "rewind")(fp);
    CHECK(STUB(int (*)(FILE *), "getc")(fp) == 'a');
    CHECK(STUB(int (*)(int, FILE *), "ungetc")('z', fp) == 'z');
    char buf[8];
    CHECK(STUB(char *(*)(char *, int, FILE *), "fgets")(buf, sizeof(buf), fp) == buf);
    CHECK(strcmp(buf, "zb") == 0);
    CHECK(STUB(int (*)(FILE *), "feof")(fp) != 0);
    CHECK(STUB(long (*)(FILE *), "ftell")(fp) == 2);
    errno = 0;
    CHECK(STUB(int (*)(FILE *, long, int), "fseek")(fp, -1, SEEK_SET) == -1);
    CHECK(errno == 22);
    CHECK(STUB(int (*)(FILE *), "fclose")(fp) == 0);

    // stdio shims translate errno: empty non-blocking pipe gives EAGAIN.
    int fds[2];
    CHECK(pipe(fds) == 0);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    FILE *rp = fdopen(fds[0], "r");
    errno = 0;
    CHECK(STUB(size_t (*)(void *, size_t, size_t, FILE *), "fread")(buf, 1, 1, rp) == 0);
    CHECK(errno == 11);
    fclose(rp);
    close(fds[1]);
}

// Cases adapted from PR #1's AAPCS64 bridge tests.
static void test_printf_more(void) {
    // Both register classes overflow; stack order is L7, L8, L9, D9, D10.
    typedef int (*Fp1)(FILE *, const char *,
                       int64_t, double, int64_t, double, int64_t, double,
                       int64_t, double, int64_t, double, int64_t, double,
                       int64_t, double, int64_t, double, int64_t, double,
                       double);
    FILE *fp = tmpfile();
    STUB(Fp1, "fprintf")(fp,
        "%ld %.1f %ld %.1f %ld %.1f %ld %.1f %ld %.1f %ld %.1f %ld %.1f "
        "%ld %.1f %ld %.1f %.1f",
        1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5, 6, 6.5, 7, 7.5,
        8, 8.5, 9, 9.5, 10.5);
    char *out = read_all(fp);
    CHECK(strcmp(out, "1 1.5 2 2.5 3 3.5 4 4.5 5 5.5 6 6.5 7 7.5 "
                      "8 8.5 9 9.5 10.5") == 0);
    free(out); fclose(fp);

    // Length modifiers truncate and sign-extend register values.
    typedef int (*Fp2)(FILE *, const char *, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t);
    Fp2 fp2 = STUB(Fp2, "fprintf");
    fp = tmpfile();
    fp2(fp, "%hhd|%hu|%d|%u|%llx|%zu", 300, 70000, 0xFFFFFFFF, -1,
        0x1234567890abcdefLL, 12345);
    out = read_all(fp);
    CHECK(strcmp(out, "44|4464|-1|4294967295|1234567890abcdef|12345") == 0);
    free(out); fclose(fp);

    fp = tmpfile();
    fp2(fp, "[%-6s][%6.2s][%c][%%][%s][%05d]", (int64_t)(intptr_t)"abc",
        (int64_t)(intptr_t)"xyz", 'Q', 0, 42, 0);
    out = read_all(fp);
    CHECK(strcmp(out, "[abc   ][    xy][Q][%][(null)][00042]") == 0);
    free(out); fclose(fp);

    // Output longer than the Android log buffer.
    typedef int (*Fp3)(FILE *, const char *, const char *, double);
    char big[3001];
    memset(big, 'z', 3000); big[3000] = '\0';
    fp = tmpfile();
    int ret = STUB(Fp3, "fprintf")(fp, "<%s>%.3f", big, 2.0);
    out = read_all(fp);
    CHECK(ret == 3007 && strlen(out) == 3007);
    CHECK(out[0] == '<' && out[3001] == '>' && strcmp(out + 3002, "2.000") == 0);
    free(out); fclose(fp);

    out = capture_stderr(log_star_width);
    CHECK(strcmp(out, "[Tag]     3.142|ab   |\n") == 0);
    free(out);
    out = capture_stderr(log_many_args);
    CHECK(strcmp(out, "[T] 1234567 0.5 1.5 2.5 3.5 4.5 5.5 6.5 7.5 8.5\n") == 0);
    free(out);

    // %m prints strerror for the Linux errno; it consumes no argument.
    char buf[128];
    typedef int (*Sn)(char *, size_t, const char *, int64_t, int64_t);
    Sn sn = STUB(Sn, "snprintf");
    errno = 11;  // Linux EAGAIN
    sn(buf, sizeof(buf), "%d %m %d", 1, 2);
    CHECK(strcmp(buf, "1 Resource temporarily unavailable 2") == 0);
    CHECK(errno == 11);
    errno = 2;
    sn(buf, sizeof(buf), "[%.6m]%%m", 0, 0);
    CHECK(strcmp(buf, "[No suc]%m") == 0);
    errno = 0;
    CHECK(sn(buf, sizeof(buf), "%*m", 3, 0) == -1 && errno == 22);
    errno = 2;
    CHECK(sn(buf, sizeof(buf), "%lm", 0, 0) == -1 && errno == 22);
    errno = 2;
    CHECK(sn(buf, sizeof(buf), "%300m|", 0, 0) == 301);
    CHECK(strlen(buf) == sizeof(buf) - 1 && buf[0] == ' ');
    errno = 2;
    sn(buf, sizeof(buf), "[%-----------------------------------4.2m]", 0, 0);
    CHECK(strcmp(buf, "[No  ]") == 0);

    // NULL format prints nothing.
    CHECK(sn(buf, sizeof(buf), NULL, 0, 0) == 0 && buf[0] == '\0');

    // binary128 2^-1030 is below double's normal range: keep it subnormal.
    typedef int (*Sq)(char *, size_t, const char *, Quad);
    Quad q = {0};
    q[15] = 0x3b; q[14] = 0xf9;  // exponent 16383 - 1030
    STUB(Sq, "snprintf")(buf, sizeof(buf), "%La", q);
    char want[64];
    snprintf(want, sizeof(want), "%a", 0x1p-1030);
    CHECK(strcmp(buf, want) == 0);

    // NaN whose payload is only in the low 60 fraction bits.
    Quad nan = {0};
    nan[15] = 0x7f; nan[14] = 0xff; nan[0] = 1;
    STUB(Sq, "snprintf")(buf, sizeof(buf), "%Lf", nan);
    CHECK(strcmp(buf, "nan") == 0);
}

static void test_misc(void) {
    void *(*mmap_)(void *, size_t, int, int, int, off_t) = sym("mmap");
    // MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_POPULATE in Linux encoding
    void *p = mmap_(NULL, 65536, PROT_READ | PROT_WRITE, 0x02 | 0x20 | 0x4000 | 0x8000, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p != MAP_FAILED) {
        ((char *)p)[100] = 1;
        CHECK(STUB(int (*)(void *, size_t, int), "madvise")(p, 65536, 4) == 0);
        CHECK(STUB(int (*)(void *, size_t, int), "madvise")(p, 65536, 16) == 0);
        munmap(p, 65536);
    }
    CHECK(sym("mmap64") == sym("mmap"));

    uint64_t rl[2];
    int (*getrlimit_)(int, uint64_t *) = sym("getrlimit");
    CHECK(getrlimit_(7, rl) == 0 && rl[0] > 0);       // RLIMIT_NOFILE
    CHECK(getrlimit_(3, rl) == 0);                    // RLIMIT_STACK
    CHECK(getrlimit_(13, rl) == 0 && rl[0] == ~0ULL); // RLIMIT_NICE
    errno = 0;
    CHECK(getrlimit_(99, rl) == -1 && errno == 22);

    int64_t tv[2] = { -1, -1 };
    CHECK(STUB(int (*)(void *, void *), "gettimeofday")(tv, NULL) == 0);
    CHECK(tv[0] > 1600000000 && tv[1] >= 0 && tv[1] < 1000000);

    char *(*setlocale_)(int, const char *) = sym("setlocale");
    CHECK(setlocale_(1, NULL) != NULL);               // LC_NUMERIC
    CHECK(setlocale_(6, NULL) != NULL);               // LC_ALL
    CHECK(setlocale_(12, NULL) == NULL);
}

int main(void) {
    char dir[] = "/tmp/linux_abi_XXXXXX";
    if (!mkdtemp(dir)) return 1;

    test_errno();
    test_futex();
    test_semaphores();
    test_files(dir);
    test_uname_sysconf();
    test_signals();
    test_keys_attrs();
    test_mutexes();
    test_mutex_race();
    test_once();
    test_scanf();
    test_printf();
    test_printf_more();
    test_errno_wrappers();
    test_strtold();
    test_stdio_vars();
    test_misc();

    rmdir(dir);
    printf("linux abi: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
