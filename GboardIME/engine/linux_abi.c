// Linux/Bionic ABI translation for imports macOS provides under the same
// name. See linux_abi.h for how this differs from android_stubs.c.
//
// Values in comments are Linux arm64 (Bionic, LP64) unless noted.

#include "linux_abi.h"
#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <os/lock.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <xlocale.h>

// ── errno ────────────────────────────────────────────────────────────────────

enum {
    L_EINTR = 4, L_EBADF = 9, L_EAGAIN = 11, L_ENOMEM = 12, L_EFAULT = 14,
    L_EEXIST = 17, L_EINVAL = 22, L_ENOTTY = 25, L_ERANGE = 34,
    L_ENOSYS = 38, L_ENOTSUP = 95, L_ETIMEDOUT = 110,
};

int linux_errno_from_darwin(int err) {
    if (err == EDEADLK) return 35;  // macOS 11 is Linux EAGAIN
    if (err <= 34) return err;      // the rest of 1-34 are identical
    switch (err) {
    case EAGAIN:          return 11;
    case EINPROGRESS:     return 115;
    case EALREADY:        return 114;
    case ENOTSOCK:        return 88;
    case EDESTADDRREQ:    return 89;
    case EMSGSIZE:        return 90;
    case EPROTOTYPE:      return 91;
    case ENOPROTOOPT:     return 92;
    case EPROTONOSUPPORT: return 93;
    case ESOCKTNOSUPPORT: return 94;
    case ENOTSUP:         return 95;
    case EOPNOTSUPP:      return 95;
    case EPFNOSUPPORT:    return 96;
    case EAFNOSUPPORT:    return 97;
    case EADDRINUSE:      return 98;
    case EADDRNOTAVAIL:   return 99;
    case ENETDOWN:        return 100;
    case ENETUNREACH:     return 101;
    case ENETRESET:       return 102;
    case ECONNABORTED:    return 103;
    case ECONNRESET:      return 104;
    case ENOBUFS:         return 105;
    case EISCONN:         return 106;
    case ENOTCONN:        return 107;
    case ESHUTDOWN:       return 108;
    case ETOOMANYREFS:    return 109;
    case ETIMEDOUT:       return 110;
    case ECONNREFUSED:    return 111;
    case ELOOP:           return 40;
    case ENAMETOOLONG:    return 36;
    case EHOSTDOWN:       return 112;
    case EHOSTUNREACH:    return 113;
    case ENOTEMPTY:       return 39;
    case EUSERS:          return 87;
    case EDQUOT:          return 122;
    case ESTALE:          return 116;
    case ENOLCK:          return 37;
    case ENOSYS:          return 38;
    case EOVERFLOW:       return 75;
    case ECANCELED:       return 125;
    case EIDRM:           return 43;
    case ENOMSG:          return 42;
    case EILSEQ:          return 84;
    case EBADMSG:         return 74;
    case EMULTIHOP:       return 72;
    case ENODATA:         return 61;
    case ENOLINK:         return 67;
    case ENOSR:           return 63;
    case ENOSTR:          return 60;
    case EPROTO:          return 71;
    case ETIME:           return 62;
    case EOWNERDEAD:      return 130;
    case ENOTRECOVERABLE: return 131;
    default:              return err;
    }
}

// Sets errno to the Linux translation of the current macOS errno.
static void fix_errno(void) { errno = linux_errno_from_darwin(errno); }

// ── Futex-style wait/wake (libSystem __ulock, available since 10.12) ─────────

extern int __ulock_wait(uint32_t operation, void *addr, uint64_t value,
                        uint32_t timeout_us);
extern int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);
#define UL_COMPARE_AND_WAIT 1
#define ULF_WAKE_ALL        0x00000100

int linux_abi_wait_on_address(volatile uint32_t *addr, uint32_t expected,
                              uint32_t timeout_us) {
    int r = __ulock_wait(UL_COMPARE_AND_WAIT, (void *)addr, expected,
                         timeout_us);
    if (r >= 0) return 0;
    return errno == ETIMEDOUT || errno == EINTR ? errno : 0;
}

void linux_abi_wake_address(volatile uint32_t *addr, int all) {
    __ulock_wake(UL_COMPARE_AND_WAIT | (all ? ULF_WAKE_ALL : 0),
                 (void *)addr, 0);
}

// Wakes up to `max` waiters one at a time; returns how many were woken.
static long wake_count(volatile uint32_t *addr, long max) {
    long woken = 0;
    while (woken < max &&
           __ulock_wake(UL_COMPARE_AND_WAIT, (void *)addr, 0) == 0) {
        woken++;
    }
    return woken;
}

static int64_t clock_ns(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

typedef struct { int64_t tv_sec; int64_t tv_nsec; } LinuxTimespec;

static int timespec_valid(const LinuxTimespec *ts) {
    return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000;
}

static int64_t timespec_ns(const LinuxTimespec *ts) {
    if (ts->tv_sec > INT64_MAX / 1000000000LL - 1) return INT64_MAX;
    return ts->tv_sec * 1000000000LL + ts->tv_nsec;
}

// Waits while *addr == expected until `deadline_ns` on `clock`
// (deadline_ns < 0 = forever). Returns 0 or a Linux errno.
static int wait_until(volatile uint32_t *addr, uint32_t expected,
                      clockid_t clock, int64_t deadline_ns) {
    for (;;) {
        if (atomic_load((_Atomic uint32_t *)addr) != expected) return 0;
        uint32_t timeout_us = 0;
        if (deadline_ns >= 0) {
            int64_t remaining = deadline_ns - clock_ns(clock);
            if (remaining <= 0) return L_ETIMEDOUT;
            int64_t us = (remaining + 999) / 1000;
            timeout_us = us >= UINT32_MAX ? UINT32_MAX - 1 : (uint32_t)us;
        }
        int r = linux_abi_wait_on_address(addr, expected, timeout_us);
        if (r == 0) return 0;
        if (r == EINTR) return L_EINTR;
        // ETIMEDOUT: loop to re-check the deadline.
    }
}

// futex(2) operations used by libc++abi guards, absl and TFLite.
enum {
    FUTEX_WAIT = 0, FUTEX_WAKE = 1, FUTEX_REQUEUE = 3, FUTEX_CMP_REQUEUE = 4,
    FUTEX_WAIT_BITSET = 9, FUTEX_WAKE_BITSET = 10,
    FUTEX_PRIVATE_FLAG = 128, FUTEX_CLOCK_REALTIME = 256,
};

static long linux_futex(uint32_t *addr, int op, uint32_t val,
                        const LinuxTimespec *ts, uint32_t *addr2,
                        uint32_t val3) {
    (void)addr2;
    int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    switch (cmd) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        if (cmd == FUTEX_WAIT_BITSET && val3 == 0) {
            errno = L_EINVAL;
            return -1;
        }
        clockid_t clock = CLOCK_MONOTONIC;
        int64_t deadline = -1;
        if (ts) {
            if (!timespec_valid(ts)) { errno = L_EINVAL; return -1; }
            if (cmd == FUTEX_WAIT) {
                // FUTEX_WAIT timeouts are relative.
                int64_t rel = timespec_ns(ts);
                int64_t now = clock_ns(clock);
                deadline = rel > INT64_MAX - now ? INT64_MAX : now + rel;
            } else {
                if (op & FUTEX_CLOCK_REALTIME) clock = CLOCK_REALTIME;
                deadline = timespec_ns(ts);
            }
        }
        if (atomic_load((_Atomic uint32_t *)addr) != val) {
            errno = L_EAGAIN;
            return -1;
        }
        int r = wait_until(addr, val, clock, deadline);
        if (r) { errno = r; return -1; }
        return 0;
    }
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
        // Bitsets are treated as "match any": extra wakeups are spurious
        // wakeups, which futex users must tolerate.
        if (cmd == FUTEX_WAKE_BITSET && val3 == 0) {
            errno = L_EINVAL;
            return -1;
        }
        return wake_count(addr, (int)val);
    case FUTEX_CMP_REQUEUE:
        if (atomic_load((_Atomic uint32_t *)addr) != val3) {
            errno = L_EAGAIN;
            return -1;
        }
        // fallthrough
    case FUTEX_REQUEUE: {
        // Waiters cannot be moved between addresses with __ulock, so the
        // ones that would be requeued (up to `ts` as a count) get a
        // spurious wakeup instead and re-wait on their own.
        long requeue = (long)(uintptr_t)ts;
        if ((int)val < 0 || requeue < 0) { errno = L_EINVAL; return -1; }
        long woken = wake_count(addr, (int)val);
        return woken + wake_count(addr, requeue);
    }
    default:
        errno = L_ENOSYS;
        return -1;
    }
}

// ── Signals (virtualized) ────────────────────────────────────────────────────
// The .so's handlers expect Linux siginfo/ucontext layouts and would also
// replace the engine's CRASH_PROTECT handlers, so sigaction is recorded but
// never installed, and synchronous/watchdog signals are never blocked.

typedef struct {
    int sa_flags;
    void *handler;  // sa_handler or sa_sigaction
    uint64_t sa_mask;
    void *sa_restorer;
} LinuxSigaction;
_Static_assert(sizeof(LinuxSigaction) == 32, "Bionic LP64 sigaction");

#define LINUX_NSIG 64
static LinuxSigaction s_sigactions[LINUX_NSIG + 1];
static pthread_mutex_t s_sig_lock = PTHREAD_MUTEX_INITIALIZER;

// Linux signal number → macOS (0 = no equivalent).
static int darwin_signal(int sig) {
    static const int map[32] = {
        0, SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGBUS,
        SIGFPE, SIGKILL, SIGUSR1, SIGSEGV, SIGUSR2, SIGPIPE, SIGALRM,
        SIGTERM, 0 /* STKFLT */, SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP,
        SIGTTIN, SIGTTOU, SIGURG, SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF,
        SIGWINCH, SIGIO, 0 /* PWR */, SIGSYS,
    };
    return sig > 0 && sig < 32 ? map[sig] : 0;
}

static int linux_signal(int dsig) {
    for (int sig = 1; sig < 32; sig++) {
        if (darwin_signal(sig) == dsig) return sig;
    }
    return 0;
}

static int never_blocked(int dsig) {
    return dsig == SIGSEGV || dsig == SIGBUS || dsig == SIGILL ||
           dsig == SIGFPE || dsig == SIGABRT || dsig == SIGTRAP ||
           dsig == SIGALRM;
}

static int stub_sigemptyset(uint64_t *set) { *set = 0; return 0; }
static int stub_sigfillset(uint64_t *set) { *set = ~0ULL; return 0; }
static int stub_sigaddset(uint64_t *set, int sig) {
    if (sig < 1 || sig > LINUX_NSIG) { errno = L_EINVAL; return -1; }
    *set |= 1ULL << (sig - 1);
    return 0;
}
static int stub_sigdelset(uint64_t *set, int sig) {
    if (sig < 1 || sig > LINUX_NSIG) { errno = L_EINVAL; return -1; }
    *set &= ~(1ULL << (sig - 1));
    return 0;
}
static int stub_sigismember(const uint64_t *set, int sig) {
    if (sig < 1 || sig > LINUX_NSIG) { errno = L_EINVAL; return -1; }
    return (*set >> (sig - 1)) & 1;
}

static int stub_sigaction(int sig, const LinuxSigaction *act,
                          LinuxSigaction *old) {
    if (sig < 1 || sig > LINUX_NSIG ||
        (act && (sig == 9 || sig == 19))) {
        errno = L_EINVAL;
        return -1;
    }
    pthread_mutex_lock(&s_sig_lock);
    if (old) *old = s_sigactions[sig];
    if (act) {
        s_sigactions[sig] = *act;
        // Ignoring SIGPIPE is safe to honor for real and avoids the
        // process dying on a write to a closed pipe or socket. Other
        // dispositions stay virtual so the host's setting is kept.
        if (sig == 13 && act->handler == (void *)1) signal(SIGPIPE, SIG_IGN);
    }
    pthread_mutex_unlock(&s_sig_lock);
    return 0;
}

// Returns 0 or a Linux errno. how: 0 BLOCK, 1 UNBLOCK, 2 SETMASK.
static int linux_sigmask(int how, const uint64_t *set, uint64_t *old) {
    int dhow;
    switch (how) {
    case 0: dhow = SIG_BLOCK; break;
    case 1: dhow = SIG_UNBLOCK; break;
    case 2: dhow = SIG_SETMASK; break;
    default: if (set) return L_EINVAL; dhow = SIG_BLOCK; break;
    }
    sigset_t dset, dold;
    sigemptyset(&dset);
    if (set) {
        for (int sig = 1; sig < 32; sig++) {
            int dsig = darwin_signal(sig);
            if (dsig && !never_blocked(dsig) && ((*set >> (sig - 1)) & 1)) {
                sigaddset(&dset, dsig);
            }
        }
    }
    int r = pthread_sigmask(dhow, set ? &dset : NULL, &dold);
    if (r) return linux_errno_from_darwin(r);
    if (old) {
        *old = 0;
        for (int dsig = 1; dsig < NSIG; dsig++) {
            int sig = linux_signal(dsig);
            if (sig && sigismember(&dold, dsig)) *old |= 1ULL << (sig - 1);
        }
    }
    return 0;
}

static int stub_pthread_sigmask(int how, const uint64_t *set,
                                uint64_t *old) {
    return linux_sigmask(how, set, old);
}

typedef struct { void *ss_sp; int ss_flags; size_t ss_size; } LinuxStack;

static int stub_sigaltstack(const LinuxStack *ss, LinuxStack *old) {
    (void)ss;
    if (old) {
        old->ss_sp = NULL;
        old->ss_flags = 2;  // SS_DISABLE
        old->ss_size = 0;
    }
    return 0;
}

static int stub_kill(pid_t pid, int sig) {
    if (sig < 0 || sig > LINUX_NSIG) { errno = L_EINVAL; return -1; }
    if (pid == 0 || pid == getpid()) {
        // Only signals that terminate are delivered to ourselves; the
        // .so's own handlers are virtual and never run.
        if (sig != 6 && sig != 9) return 0;
    }
    int dsig = sig == 0 ? 0 : darwin_signal(sig);
    if (sig && !dsig) { errno = L_EINVAL; return -1; }
    if (kill(pid, dsig) != 0) { fix_errno(); return -1; }
    return 0;
}

static int stub_pthread_kill(pthread_t thread, int sig) {
    if (sig < 0 || sig > LINUX_NSIG) return L_EINVAL;
    if (sig != 0) return 0;
    return linux_errno_from_darwin(pthread_kill(thread, 0));
}

// ── syscall(2) ───────────────────────────────────────────────────────────────
// Bionic's syscall() is variadic, but AAPCS64 passes the arguments in
// x1-x6 exactly like a fixed-argument call.

void *linux_mmap(void *addr, size_t length, int prot, int flags, int fd,
                 off_t offset);


static long stub_syscall(long nr, long a1, long a2, long a3, long a4,
                         long a5, long a6) {
    switch (nr) {
    case 98:   // futex
        return linux_futex((uint32_t *)a1, (int)a2, (uint32_t)a3,
                           (const LinuxTimespec *)a4, (uint32_t *)a5,
                           (uint32_t)a6);
    case 178:  // gettid
        return android_stubs_gettid();
    case 172:  // getpid
        return getpid();
    case 124:  // sched_yield
        sched_yield();
        return 0;
    case 168:  // getcpu
        if (a1) *(unsigned *)a1 = 0;
        if (a2) *(unsigned *)a2 = 0;
        return 0;
    case 215:  // munmap
        if (munmap((void *)a1, (size_t)a2) != 0) { fix_errno(); return -1; }
        return 0;
    case 222: {  // mmap
        void *p = linux_mmap((void *)a1, (size_t)a2, (int)a3, (int)a4,
                             (int)a5, (off_t)a6);
        return p == MAP_FAILED ? -1 : (long)p;
    }
    case 135: {  // rt_sigprocmask
        int r = linux_sigmask((int)a1, (const uint64_t *)a2,
                              (uint64_t *)a3);
        if (r) { errno = r; return -1; }
        return 0;
    }
    case 278:  // getrandom
        arc4random_buf((void *)a1, (size_t)a2);
        return a2;
    default:
        errno = L_ENOSYS;
        return -1;
    }
}

// ── File descriptors: open / fcntl / ioctl ───────────────────────────────────

enum {
    L_O_CREAT = 0x40, L_O_EXCL = 0x80, L_O_NOCTTY = 0x100,
    L_O_TRUNC = 0x200, L_O_APPEND = 0x400, L_O_NONBLOCK = 0x800,
    L_O_DSYNC = 0x1000, L_O_DIRECTORY = 0x4000, L_O_NOFOLLOW = 0x8000,
    L_O_CLOEXEC = 0x80000, L_O_SYNC = 0x101000, L_O_PATH = 0x200000,
    L_O_TMPFILE_BIT = 0x400000,
};

static const struct { int linux_flag; int darwin_flag; } k_open_flags[] = {
    { L_O_CREAT, O_CREAT }, { L_O_EXCL, O_EXCL }, { L_O_NOCTTY, O_NOCTTY },
    { L_O_TRUNC, O_TRUNC }, { L_O_APPEND, O_APPEND },
    { L_O_NONBLOCK, O_NONBLOCK }, { L_O_DIRECTORY, O_DIRECTORY },
    { L_O_NOFOLLOW, O_NOFOLLOW }, { L_O_CLOEXEC, O_CLOEXEC },
};

static int darwin_open_flags(int flags) {
    int out = flags & O_ACCMODE;
    for (size_t i = 0; i < sizeof(k_open_flags) / sizeof(*k_open_flags); i++) {
        if (flags & k_open_flags[i].linux_flag) out |= k_open_flags[i].darwin_flag;
    }
    if ((flags & L_O_SYNC) == L_O_SYNC) out |= O_SYNC;
    else if (flags & L_O_DSYNC) out |= O_DSYNC;
    return out;  // O_DIRECT, O_LARGEFILE and O_NOATIME have no equivalent
}

static int linux_open_flags(int dflags) {
    int out = dflags & O_ACCMODE;
    for (size_t i = 0; i < sizeof(k_open_flags) / sizeof(*k_open_flags); i++) {
        if (dflags & k_open_flags[i].darwin_flag) out |= k_open_flags[i].linux_flag;
    }
    if (dflags & O_SYNC) out |= L_O_SYNC;
    else if (dflags & O_DSYNC) out |= L_O_DSYNC;
    return out;
}

// open() is variadic in C, but the mode arrives in w2 under AAPCS64.
static int stub_open(const char *path, int flags, unsigned mode) {
    if (flags & L_O_TMPFILE_BIT) { errno = L_ENOTSUP; return -1; }
    int dflags = darwin_open_flags(flags);
    if (flags & L_O_PATH) dflags = (dflags & ~O_ACCMODE) | O_RDONLY;
    int fd = open(path, dflags, (mode_t)mode);
    if (fd < 0) fix_errno();
    return fd;
}

typedef struct {
    int16_t l_type;
    int16_t l_whence;
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
} LinuxFlock;
_Static_assert(sizeof(LinuxFlock) == 32, "Linux arm64 struct flock");

static int fcntl_lock(int fd, int dcmd, LinuxFlock *lf) {
    if (!lf) { errno = L_EFAULT; return -1; }
    struct flock df = {0};
    switch (lf->l_type) {
    case 0: df.l_type = F_RDLCK; break;
    case 1: df.l_type = F_WRLCK; break;
    case 2: df.l_type = F_UNLCK; break;
    default: errno = L_EINVAL; return -1;
    }
    df.l_whence = lf->l_whence;
    df.l_start = lf->l_start;
    df.l_len = lf->l_len;
    df.l_pid = lf->l_pid;
    if (fcntl(fd, dcmd, &df) != 0) { fix_errno(); return -1; }
    if (dcmd == F_GETLK) {
        lf->l_type = df.l_type == F_RDLCK ? 0 : df.l_type == F_WRLCK ? 1 : 2;
        lf->l_whence = df.l_whence;
        lf->l_start = df.l_start;
        lf->l_len = df.l_len;
        lf->l_pid = df.l_pid;
    }
    return 0;
}

static int stub_fcntl(int fd, int cmd, long arg) {
    int r;
    switch (cmd) {
    case 0:    r = fcntl(fd, F_DUPFD, (int)arg); break;
    case 1030: r = fcntl(fd, F_DUPFD_CLOEXEC, (int)arg); break;
    case 1:    r = fcntl(fd, F_GETFD); break;
    case 2:    r = fcntl(fd, F_SETFD, (int)arg); break;
    case 3:
        r = fcntl(fd, F_GETFL);
        if (r >= 0) return linux_open_flags(r);
        break;
    case 4:    r = fcntl(fd, F_SETFL, darwin_open_flags((int)arg)); break;
    case 5:    return fcntl_lock(fd, F_GETLK, (LinuxFlock *)arg);
    case 6:    return fcntl_lock(fd, F_SETLK, (LinuxFlock *)arg);
    case 7:    return fcntl_lock(fd, F_SETLKW, (LinuxFlock *)arg);
    default:   errno = L_EINVAL; return -1;
    }
    if (r < 0) fix_errno();
    return r;
}

static int stub_ioctl(int fd, int request, void *arg) {
    unsigned long dreq;
    switch ((unsigned)request) {
    case 0x541B: dreq = FIONREAD; break;
    case 0x5421: dreq = FIONBIO; break;
    case 0x5413: dreq = TIOCGWINSZ; break;
    case 0x5451: dreq = FIOCLEX; break;
    case 0x5450: dreq = FIONCLEX; break;
    default: errno = L_ENOTTY; return -1;
    }
    int r = ioctl(fd, dreq, arg);
    if (r < 0) fix_errno();
    return r;
}

// ── Memory: mmap / madvise ───────────────────────────────────────────────────

void *linux_mmap(void *addr, size_t length, int prot, int flags, int fd,
                 off_t offset) {
    enum {
        L_MAP_SHARED = 0x01, L_MAP_PRIVATE = 0x02, L_MAP_SHARED_VALIDATE = 0x03,
        L_MAP_FIXED = 0x10, L_MAP_ANONYMOUS = 0x20, L_MAP_NORESERVE = 0x4000,
        L_MAP_FIXED_NOREPLACE = 0x100000,
    };
    int dflags = 0;
    int type = flags & 0x0f;
    if (type == L_MAP_SHARED || type == L_MAP_SHARED_VALIDATE) dflags |= MAP_SHARED;
    else if (type == L_MAP_PRIVATE) dflags |= MAP_PRIVATE;
    else { errno = L_EINVAL; return MAP_FAILED; }
    if (flags & L_MAP_FIXED) dflags |= MAP_FIXED;
    if (flags & L_MAP_ANONYMOUS) dflags |= MAP_ANON;
    if (flags & L_MAP_NORESERVE) dflags |= MAP_NORESERVE;
    // MAP_POPULATE, MAP_STACK, MAP_GROWSDOWN, MAP_LOCKED and MAP_HUGETLB
    // are hints without a macOS equivalent and are dropped.
    void *p = mmap(addr, length, prot, dflags, fd, offset);
    if (p == MAP_FAILED) { fix_errno(); return p; }
    if ((flags & L_MAP_FIXED_NOREPLACE) && !(flags & L_MAP_FIXED) &&
        p != addr) {
        munmap(p, length);
        errno = L_EEXIST;
        return MAP_FAILED;
    }
    return p;
}

static int stub_madvise(void *addr, size_t length, int advice) {
    int dadvice;
    switch (advice) {
    case 0: case 1: case 2: case 3: case 4: dadvice = advice; break;
    case 8: dadvice = MADV_FREE; break;
    default:
        if (advice < 0) { errno = L_EINVAL; return -1; }
        return 0;  // MADV_DONTFORK, HUGEPAGE, DONTDUMP... are hints
    }
    if (madvise(addr, length, dadvice) != 0) { fix_errno(); return -1; }
    return 0;
}

// ── sysconf ──────────────────────────────────────────────────────────────────

static long sysctl_long(const char *name) {
    int64_t value = 0;
    size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, NULL, 0) != 0) return 0;
    if (size == sizeof(int32_t)) return (long)*(int32_t *)&value;
    return (long)value;
}

static long stub_sysconf(int name) {
    switch (name) {
    case 0x00: return sysconf(_SC_ARG_MAX);
    case 0x06: return 100;  // _SC_CLK_TCK (Linux USER_HZ)
    case 0x0b: return sysconf(_SC_OPEN_MAX);
    case 0x27: case 0x28: return sysconf(_SC_PAGESIZE);
    case 0x60: return sysconf(_SC_NPROCESSORS_CONF);
    case 0x61: return sysconf(_SC_NPROCESSORS_ONLN);
    case 0x62: return sysconf(_SC_PHYS_PAGES);
    case 0x63: return sysctl_long("vm.page_free_count");
    case 0x8f: return sysctl_long("hw.l1icachesize");
    case 0x91: case 0x94: case 0x97: case 0x9a:
        return sysctl_long("hw.cachelinesize");
    case 0x92: return sysctl_long("hw.l1dcachesize");
    case 0x95: return sysctl_long("hw.l2cachesize");
    case 0x98: return sysctl_long("hw.l3cachesize");
    case 0x90: case 0x93: case 0x96: case 0x99:
    case 0x9b: case 0x9c: case 0x9d:
        return 0;  // unknown cache geometry, as on Linux
    default:
        errno = L_EINVAL;
        return -1;
    }
}

// ── scanf family ─────────────────────────────────────────────────────────────
// Linux callers pass scanf varargs per AAPCS64 (x2-x7, then 8-byte stack
// slots); Darwin's variadic convention puts all of them on the stack.
// Count the pointer arguments the format consumes and rebuild a Darwin
// va_list (an array of 8-byte slots).

#define SCANF_MAX_ARGS 32

// Returns the number of argument slots the format reads: the count of
// sequential conversions or the highest %n$ position, whichever is larger.
static int scanf_arg_count(const char *fmt) {
    int n = 0, max_pos = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        int suppress = 0, pos = 0;
        if (*p == '*') { suppress = 1; p++; }
        const char *digits = p;
        while (*p >= '0' && *p <= '9') {
            if (pos <= SCANF_MAX_ARGS) pos = pos * 10 + (*p - '0');
            p++;
        }
        if (*p == '$' && p > digits) {
            p++;
            if (pos > max_pos) max_pos = pos;
            if (*p == '*') { suppress = 1; p++; }
            while (*p >= '0' && *p <= '9') p++;
            suppress = 1;  // counted via max_pos
        }
        while (*p && strchr("hlLqjzt", *p)) p++;
        if (*p == '[') {
            p++;
            if (*p == '^') p++;
            if (*p == ']') p++;
            while (*p && *p != ']') p++;
        }
        if (!*p) break;
        if (!suppress) n++;
    }
    return n > max_pos ? n : max_pos;
}

#define SCANF_STACK_PARAMS \
    void *s0, void *s1, void *s2, void *s3, void *s4, void *s5, void *s6, \
    void *s7, void *s8, void *s9, void *s10, void *s11, void *s12, \
    void *s13, void *s14, void *s15, void *s16, void *s17, void *s18, \
    void *s19, void *s20, void *s21, void *s22, void *s23, void *s24, \
    void *s25
#define SCANF_SLOTS(r0, r1, r2, r3, r4, r5) { \
    r0, r1, r2, r3, r4, r5, s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, \
    s11, s12, s13, s14, s15, s16, s17, s18, s19, s20, s21, s22, s23, s24, \
    s25 }

static int scanf_check(const char *fmt) {
    if (scanf_arg_count(fmt) > SCANF_MAX_ARGS) { errno = L_EINVAL; return 0; }
    return 1;
}

static int stub_sscanf(const char *str, const char *fmt, void *r0, void *r1,
                       void *r2, void *r3, void *r4, void *r5,
                       SCANF_STACK_PARAMS) {
    if (!scanf_check(fmt)) return -1;
    void *slots[SCANF_MAX_ARGS] = SCANF_SLOTS(r0, r1, r2, r3, r4, r5);
    return vsscanf(str, fmt, (va_list)(void *)slots);
}

static int stub_fscanf(FILE *stream, const char *fmt, void *r0, void *r1,
                       void *r2, void *r3, void *r4, void *r5,
                       SCANF_STACK_PARAMS) {
    if (!scanf_check(fmt)) return -1;
    void *slots[SCANF_MAX_ARGS] = SCANF_SLOTS(r0, r1, r2, r3, r4, r5);
    return vfscanf(android_stubs_fixup_file(stream), fmt,
                   (va_list)(void *)slots);
}

static int stub_vsscanf(const char *str, const char *fmt,
                        const LinuxVaList *ap) {
    int n = scanf_arg_count(fmt);
    if (n > SCANF_MAX_ARGS) { errno = L_EINVAL; return -1; }
    LinuxVaList va = *ap;
    void *slots[SCANF_MAX_ARGS] = {0};
    for (int i = 0; i < n; i++) {
        if (va.gr_offs < 0) {
            slots[i] = *(void **)((char *)va.gr_top + va.gr_offs);
            va.gr_offs += 8;
        } else {
            slots[i] = *(void **)va.stack;
            va.stack = (char *)va.stack + 8;
        }
    }
    return vsscanf(str, fmt, (va_list)(void *)slots);
}

// ── Semaphores ───────────────────────────────────────────────────────────────
// macOS has no unnamed semaphores (sem_init returns ENOSYS). Bionic's
// sem_t is 16 bytes; the first word holds the count.

typedef struct { _Atomic uint32_t count; int32_t reserved[3]; } LinuxSem;
#define BIONIC_SEM_VALUE_MAX 0x3fffffff

static int stub_sem_init(LinuxSem *sem, int pshared, unsigned value) {
    (void)pshared;
    if (value > BIONIC_SEM_VALUE_MAX) { errno = L_EINVAL; return -1; }
    atomic_store(&sem->count, value);
    return 0;
}

static int stub_sem_destroy(LinuxSem *sem) { (void)sem; return 0; }

static int stub_sem_post(LinuxSem *sem) {
    uint32_t c = atomic_load(&sem->count);
    do {
        if (c >= BIONIC_SEM_VALUE_MAX) {
            errno = 75;  // EOVERFLOW
            return -1;
        }
    } while (!atomic_compare_exchange_weak(&sem->count, &c, c + 1));
    linux_abi_wake_address((volatile uint32_t *)&sem->count, 0);
    return 0;
}

static int sem_try_take(LinuxSem *sem) {
    uint32_t c = atomic_load(&sem->count);
    while (c > 0) {
        if (atomic_compare_exchange_weak(&sem->count, &c, c - 1)) return 1;
    }
    return 0;
}

static int sem_wait_deadline(LinuxSem *sem, int64_t deadline_ns) {
    for (;;) {
        if (sem_try_take(sem)) return 0;
        int r = wait_until((volatile uint32_t *)&sem->count, 0,
                           CLOCK_REALTIME, deadline_ns);
        if (r) { errno = r; return -1; }  // ETIMEDOUT or EINTR
    }
}

static int stub_sem_wait(LinuxSem *sem) { return sem_wait_deadline(sem, -1); }

static int stub_sem_trywait(LinuxSem *sem) {
    if (sem_try_take(sem)) return 0;
    errno = L_EAGAIN;
    return -1;
}

static int stub_sem_timedwait(LinuxSem *sem, const LinuxTimespec *abs) {
    if (sem_try_take(sem)) return 0;
    if (!abs || !timespec_valid(abs)) { errno = L_EINVAL; return -1; }
    return sem_wait_deadline(sem, timespec_ns(abs));
}

// ── stat / statvfs / uname ───────────────────────────────────────────────────

typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t pad1;
    int64_t st_size;
    int32_t st_blksize;
    int32_t pad2;
    int64_t st_blocks;
    LinuxTimespec st_atim;
    LinuxTimespec st_mtim;
    LinuxTimespec st_ctim;
    uint32_t unused[2];
} LinuxStat;
_Static_assert(sizeof(LinuxStat) == 128, "Linux arm64 struct stat");

static void to_linux_stat(const struct stat *st, LinuxStat *out) {
    memset(out, 0, sizeof(*out));
    out->st_dev = (uint64_t)(uint32_t)st->st_dev;
    out->st_ino = st->st_ino;
    out->st_mode = st->st_mode;  // S_IF* and permission bits match
    out->st_nlink = st->st_nlink;
    out->st_uid = st->st_uid;
    out->st_gid = st->st_gid;
    out->st_rdev = (uint64_t)(uint32_t)st->st_rdev;
    out->st_size = st->st_size;
    out->st_blksize = st->st_blksize;
    out->st_blocks = st->st_blocks;
    out->st_atim = (LinuxTimespec){ st->st_atimespec.tv_sec, st->st_atimespec.tv_nsec };
    out->st_mtim = (LinuxTimespec){ st->st_mtimespec.tv_sec, st->st_mtimespec.tv_nsec };
    out->st_ctim = (LinuxTimespec){ st->st_ctimespec.tv_sec, st->st_ctimespec.tv_nsec };
}

static int stub_stat(const char *path, LinuxStat *out) {
    struct stat st;
    if (stat(path, &st) != 0) { fix_errno(); return -1; }
    to_linux_stat(&st, out);
    return 0;
}

static int stub_fstat(int fd, LinuxStat *out) {
    struct stat st;
    if (fstat(fd, &st) != 0) { fix_errno(); return -1; }
    to_linux_stat(&st, out);
    return 0;
}

typedef struct {
    unsigned long f_bsize;
    unsigned long f_frsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint64_t f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
    uint32_t reserved[6];
} LinuxStatvfs;
_Static_assert(sizeof(LinuxStatvfs) == 112, "Bionic LP64 struct statvfs");

static int stub_statvfs(const char *path, LinuxStatvfs *out) {
    struct statvfs sv;
    if (statvfs(path, &sv) != 0) { fix_errno(); return -1; }
    memset(out, 0, sizeof(*out));
    out->f_bsize = sv.f_bsize;
    out->f_frsize = sv.f_frsize;
    out->f_blocks = sv.f_blocks;
    out->f_bfree = sv.f_bfree;
    out->f_bavail = sv.f_bavail;
    out->f_files = sv.f_files;
    out->f_ffree = sv.f_ffree;
    out->f_favail = sv.f_favail;
    out->f_fsid = sv.f_fsid;
    out->f_flag = sv.f_flag & (ST_RDONLY | ST_NOSUID);  // same bit values
    out->f_namemax = sv.f_namemax;
    return 0;
}

typedef struct { char field[6][65]; } LinuxUtsname;

// Reports a conservative Linux kernel so version checks keep to features
// these shims implement.
static int stub_uname(LinuxUtsname *out) {
    memset(out, 0, sizeof(*out));
    strlcpy(out->field[0], "Linux", 65);
    if (gethostname(out->field[1], 64) != 0) strlcpy(out->field[1], "localhost", 65);
    strlcpy(out->field[2], "4.19.0", 65);
    strlcpy(out->field[3], "#1 SMP", 65);
    strlcpy(out->field[4], "aarch64", 65);
    strlcpy(out->field[5], "(none)", 65);
    return 0;
}

// ── Directories ──────────────────────────────────────────────────────────────

typedef struct {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
} LinuxDirent;
_Static_assert(sizeof(LinuxDirent) == 280, "Bionic struct dirent");

typedef struct { DIR *dir; LinuxDirent entry; } LinuxDir;

static LinuxDir *stub_opendir(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) { fix_errno(); return NULL; }
    LinuxDir *ld = calloc(1, sizeof(*ld));
    if (!ld) { closedir(dir); errno = L_ENOMEM; return NULL; }
    ld->dir = dir;
    return ld;
}

static int read_linux_dirent(LinuxDir *ld, LinuxDirent *out) {
    int saved = errno;
    errno = 0;
    struct dirent *e = readdir(ld->dir);
    if (!e) {
        if (errno) { fix_errno(); return errno; }
        errno = saved;
        return -1;
    }
    errno = saved;
    memset(out, 0, sizeof(*out));
    out->d_ino = e->d_ino;
    out->d_off = (int64_t)e->d_seekoff;
    out->d_reclen = sizeof(*out);
    out->d_type = e->d_type;  // DT_* values match
    strlcpy(out->d_name, e->d_name, sizeof(out->d_name));
    return 0;
}

static LinuxDirent *stub_readdir(LinuxDir *ld) {
    if (!ld) { errno = L_EBADF; return NULL; }
    return read_linux_dirent(ld, &ld->entry) == 0 ? &ld->entry : NULL;
}

static int stub_readdir_r(LinuxDir *ld, LinuxDirent *entry,
                          LinuxDirent **result) {
    *result = NULL;
    if (!ld) return L_EBADF;
    int r = read_linux_dirent(ld, entry);
    if (r > 0) return r;
    if (r == 0) *result = entry;
    return 0;
}

static int stub_closedir(LinuxDir *ld) {
    if (!ld) { errno = L_EBADF; return -1; }
    int r = closedir(ld->dir);
    free(ld);
    if (r != 0) fix_errno();
    return r;
}

// ── Threads: attributes, keys, names, scheduling ─────────────────────────────

typedef struct {
    uint32_t flags;
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
    char reserved[16];
} LinuxPthreadAttr;
_Static_assert(sizeof(LinuxPthreadAttr) == 56, "Bionic LP64 pthread_attr_t");

#define L_ATTR_DETACHED 0x1
#define L_PTHREAD_STACK_MIN 16384

static int stub_pthread_attr_init(LinuxPthreadAttr *a) {
    memset(a, 0, sizeof(*a));
    a->stack_size = 1024 * 1024;
    a->guard_size = 4096;
    return 0;
}

static int stub_pthread_attr_destroy(LinuxPthreadAttr *a) {
    memset(a, 0x42, sizeof(*a));
    return 0;
}

static int stub_pthread_attr_setdetachstate(LinuxPthreadAttr *a, int state) {
    if (state == 1) a->flags |= L_ATTR_DETACHED;
    else if (state == 0) a->flags &= ~L_ATTR_DETACHED;
    else return L_EINVAL;
    return 0;
}

static int stub_pthread_attr_setstacksize(LinuxPthreadAttr *a, size_t size) {
    if (size < L_PTHREAD_STACK_MIN) return L_EINVAL;
    a->stack_size = size;
    return 0;
}

static int stub_pthread_attr_setguardsize(LinuxPthreadAttr *a, size_t size) {
    a->guard_size = size;
    return 0;
}

static int stub_pthread_attr_getguardsize(const LinuxPthreadAttr *a,
                                          size_t *size) {
    *size = a->guard_size;
    return 0;
}

static int stub_pthread_attr_setschedparam(LinuxPthreadAttr *a,
                                           const int *param) {
    a->sched_priority = *param;
    return 0;
}

static int stub_pthread_attr_getschedparam(const LinuxPthreadAttr *a,
                                           int *param) {
    *param = a->sched_priority;
    return 0;
}

static int stub_pthread_attr_setschedpolicy(LinuxPthreadAttr *a, int policy) {
    a->sched_policy = policy;
    return 0;
}

static int stub_pthread_attr_setscope(LinuxPthreadAttr *a, int scope) {
    (void)a;
    if (scope == 0) return 0;          // PTHREAD_SCOPE_SYSTEM
    return scope == 1 ? L_ENOTSUP : L_EINVAL;
}

static size_t round_to_page(size_t size) {
    size_t page = (size_t)getpagesize();
    return (size + page - 1) & ~(page - 1);
}

static int stub_pthread_create(pthread_t *thread, const LinuxPthreadAttr *la,
                               void *(*start)(void *), void *arg) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (la) {
        if (la->flags & L_ATTR_DETACHED) {
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        }
        size_t stack = la->stack_size < L_PTHREAD_STACK_MIN
            ? L_PTHREAD_STACK_MIN : la->stack_size;
        pthread_attr_setstacksize(&attr, round_to_page(stack));
        pthread_attr_setguardsize(&attr, round_to_page(la->guard_size));
    }
    int r = pthread_create(thread, &attr, start, arg);
    pthread_attr_destroy(&attr);
    return linux_errno_from_darwin(r);
}

// Bionic's pthread_key_t is a 4-byte int; macOS uses an unsigned long.
static int stub_pthread_key_create(int *key, void (*dtor)(void *)) {
    pthread_key_t k;
    int r = pthread_key_create(&k, dtor);
    if (r) return linux_errno_from_darwin(r);
    if (k > INT_MAX) { pthread_key_delete(k); return L_EAGAIN; }
    *key = (int)k;
    return 0;
}

static int stub_pthread_key_delete(int key) {
    return linux_errno_from_darwin(pthread_key_delete((pthread_key_t)(unsigned)key));
}

static void *stub_pthread_getspecific(int key) {
    return pthread_getspecific((pthread_key_t)(unsigned)key);
}

static int stub_pthread_setspecific(int key, const void *value) {
    return linux_errno_from_darwin(
        pthread_setspecific((pthread_key_t)(unsigned)key, value));
}

// Linux names any thread; macOS can only name the calling one.
static int stub_pthread_setname_np(pthread_t thread, const char *name) {
    if (strlen(name) > 15) return L_ERANGE;
    if (pthread_equal(thread, pthread_self())) pthread_setname_np(name);
    return 0;
}

// Policies: Linux OTHER 0, FIFO 1, RR 2, BATCH 3, IDLE 5.
static int stub_pthread_getschedparam(pthread_t thread, int *policy,
                                      int *param) {
    int dpolicy;
    struct sched_param sp;
    int r = pthread_getschedparam(thread, &dpolicy, &sp);
    if (r) return linux_errno_from_darwin(r);
    *policy = dpolicy == SCHED_FIFO ? 1 : dpolicy == SCHED_RR ? 2 : 0;
    *param = *policy == 0 ? 0 : sp.sched_priority;
    return 0;
}

static int stub_sched_get_priority_max(int policy) {
    switch (policy) {
    case 0: case 3: case 5: return 0;
    case 1: case 2: return 99;
    default: errno = L_EINVAL; return -1;
    }
}

// ── Resource limits and time ─────────────────────────────────────────────────

typedef struct { uint64_t rlim_cur; uint64_t rlim_max; } LinuxRlimit;

static int darwin_rlimit_resource(int resource) {
    switch (resource) {
    case 0: return RLIMIT_CPU;
    case 1: return RLIMIT_FSIZE;
    case 2: return RLIMIT_DATA;
    case 3: return RLIMIT_STACK;
    case 4: return RLIMIT_CORE;
    case 5: return RLIMIT_RSS;
    case 6: return RLIMIT_NPROC;
    case 7: return RLIMIT_NOFILE;
    case 8: return RLIMIT_MEMLOCK;
    case 9: return RLIMIT_AS;
    default: return -1;
    }
}

static uint64_t linux_rlim(rlim_t value) {
    return value == RLIM_INFINITY ? ~0ULL : (uint64_t)value;
}

static rlim_t darwin_rlim(uint64_t value) {
    return value == ~0ULL || value > (uint64_t)RLIM_INFINITY
        ? RLIM_INFINITY : (rlim_t)value;
}

static int stub_getrlimit(int resource, LinuxRlimit *out) {
    if (resource >= 10 && resource <= 15) {
        // LOCKS, SIGPENDING, MSGQUEUE, NICE, RTPRIO, RTTIME: unlimited.
        out->rlim_cur = out->rlim_max = ~0ULL;
        return 0;
    }
    int dres = darwin_rlimit_resource(resource);
    if (dres < 0) { errno = L_EINVAL; return -1; }
    struct rlimit rl;
    if (getrlimit(dres, &rl) != 0) { fix_errno(); return -1; }
    out->rlim_cur = linux_rlim(rl.rlim_cur);
    out->rlim_max = linux_rlim(rl.rlim_max);
    return 0;
}

static int stub_setrlimit(int resource, const LinuxRlimit *in) {
    if (resource >= 10 && resource <= 15) return 0;
    int dres = darwin_rlimit_resource(resource);
    if (dres < 0) { errno = L_EINVAL; return -1; }
    struct rlimit rl = { darwin_rlim(in->rlim_cur), darwin_rlim(in->rlim_max) };
    if (setrlimit(dres, &rl) != 0) { fix_errno(); return -1; }
    return 0;
}

// Linux clock IDs differ from macOS (e.g. MONOTONIC is 1 vs 6).
static int stub_clock_gettime(int android_clock_id, struct timespec *time) {
    clockid_t native_clock_id;
    switch (android_clock_id) {
        case 0:  // CLOCK_REALTIME
            native_clock_id = CLOCK_REALTIME;
            break;
        case 1:  // CLOCK_MONOTONIC
        case 6:  // CLOCK_MONOTONIC_COARSE
        case 7:  // CLOCK_BOOTTIME
            native_clock_id = CLOCK_MONOTONIC;
            break;
        case 2:  // CLOCK_PROCESS_CPUTIME_ID
            native_clock_id = CLOCK_PROCESS_CPUTIME_ID;
            break;
        case 3:  // CLOCK_THREAD_CPUTIME_ID
            native_clock_id = CLOCK_THREAD_CPUTIME_ID;
            break;
        case 4:  // CLOCK_MONOTONIC_RAW
            native_clock_id = CLOCK_MONOTONIC_RAW;
            break;
        case 5:  // CLOCK_REALTIME_COARSE
            native_clock_id = CLOCK_REALTIME;
            break;
        default:
            errno = L_EINVAL;
            return -1;
    }
    return clock_gettime(native_clock_id, time);
}

// macOS suseconds_t is 32-bit; Linux reads a 64-bit tv_usec.
typedef struct { int64_t tv_sec; int64_t tv_usec; } LinuxTimeval;

static int stub_gettimeofday(LinuxTimeval *tv, struct timezone *tz) {
    struct timeval dtv;
    if (gettimeofday(&dtv, tz) != 0) { fix_errno(); return -1; }
    if (tv) {
        tv->tv_sec = dtv.tv_sec;
        tv->tv_usec = dtv.tv_usec;
    }
    return 0;
}

typedef struct {
    LinuxTimeval ru_utime;
    LinuxTimeval ru_stime;
    long fields[14];
} LinuxRusage;
_Static_assert(sizeof(LinuxRusage) == sizeof(struct rusage), "rusage size");

static int stub_getrusage(int who, LinuxRusage *out) {
    struct rusage ru;
    if (who == 1) {  // RUSAGE_THREAD: report this thread's CPU time only
        memset(out, 0, sizeof(*out));
        int64_t ns = clock_ns(CLOCK_THREAD_CPUTIME_ID);
        out->ru_utime.tv_sec = ns / 1000000000LL;
        out->ru_utime.tv_usec = (ns % 1000000000LL) / 1000;
        return 0;
    }
    if (getrusage(who, &ru) != 0) { fix_errno(); return -1; }
    memcpy(out, &ru, sizeof(ru));
    out->ru_utime = (LinuxTimeval){ ru.ru_utime.tv_sec, ru.ru_utime.tv_usec };
    out->ru_stime = (LinuxTimeval){ ru.ru_stime.tv_sec, ru.ru_stime.tv_usec };
    return 0;
}

// ── Locale categories ────────────────────────────────────────────────────────
// Linux: CTYPE 0, NUMERIC 1, TIME 2, COLLATE 3, MONETARY 4, MESSAGES 5, ALL 6.

static int darwin_locale_category(int category) {
    static const int map[7] = {
        LC_CTYPE, LC_NUMERIC, LC_TIME, LC_COLLATE, LC_MONETARY,
        LC_MESSAGES, LC_ALL,
    };
    return category >= 0 && category < 7 ? map[category] : -1;
}

static char *stub_setlocale(int category, const char *locale) {
    int dcat = darwin_locale_category(category);
    if (dcat < 0) { errno = L_EINVAL; return NULL; }
    return setlocale(dcat, locale);
}

static locale_t stub_newlocale(int mask, const char *locale, locale_t base) {
    static const int map[6] = {
        LC_CTYPE_MASK, LC_NUMERIC_MASK, LC_TIME_MASK, LC_COLLATE_MASK,
        LC_MONETARY_MASK, LC_MESSAGES_MASK,
    };
    int dmask = 0;
    for (int i = 0; i < 6; i++) {
        if (mask & (1 << i)) dmask |= map[i];
    }
    locale_t result = newlocale(dmask, locale, base);
    if (!result) fix_errno();
    return result;
}

// ── stdio (Bionic FILE* from __sF → macOS FILE*) ───────────────────────────
static size_t stub_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, android_stubs_fixup_file(stream));
}
static int stub_fflush(FILE *stream) {
    return fflush(android_stubs_fixup_file(stream));
}
static int stub_fputs(const char *s, FILE *stream) {
    return fputs(s, android_stubs_fixup_file(stream));
}
static int stub_fputc(int c, FILE *stream) {
    return fputc(c, android_stubs_fixup_file(stream));
}
static size_t stub_fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fread(ptr, size, nmemb, android_stubs_fixup_file(stream));
}

// ── printf family ────────────────────────────────────────────────────────────
// Bound directly to libSystem, these read AAPCS64 register varargs from
// the stack and crash or print garbage. Walk the format to learn each
// argument's class, pull it from the LinuxVaList, and rebuild a Darwin
// va_list (8-byte slots; Darwin long double is double).

#define PRINTF_MAX_ARGS 64
enum { PA_INT = 1, PA_DBL, PA_LDBL };

static int printf_pos(const char **pp) {
    const char *q = *pp;
    int n = 0;
    if (*q < '1' || *q > '9') return -1;
    while (*q >= '0' && *q <= '9') n = n * 10 + (*q++ - '0');
    if (*q != '$') return -1;
    *pp = q + 1;
    return n - 1;
}

static int printf_set(uint8_t *types, int *count, int idx, uint8_t t) {
    if (idx < 0 || idx >= PRINTF_MAX_ARGS) return 0;
    types[idx] = t;
    if (idx + 1 > *count) *count = idx + 1;
    return 1;
}

// Returns the number of argument slots the format consumes, or -1 for
// %n or more than PRINTF_MAX_ARGS arguments.
static int printf_arg_types(const char *fmt, uint8_t *types) {
    int count = 0, next = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        int pos = printf_pos(&p);
        while (*p && strchr("-+ #0'", *p)) p++;
        if (*p == '*') {
            p++;
            int wp = printf_pos(&p);
            if (!printf_set(types, &count, wp >= 0 ? wp : next++, PA_INT))
                return -1;
        } else {
            while (*p >= '0' && *p <= '9') p++;
        }
        if (*p == '.') {
            p++;
            if (*p == '*') {
                p++;
                int pp = printf_pos(&p);
                if (!printf_set(types, &count, pp >= 0 ? pp : next++, PA_INT))
                    return -1;
            } else {
                while (*p >= '0' && *p <= '9') p++;
            }
        }
        int is_long_double = 0;
        while (*p && strchr("hlLqjzt", *p)) {
            if (*p == 'L') is_long_double = 1;
            p++;
        }
        if (!*p) break;
        // Darwin aborts on %n in a writable format; Bionic rejects %n too.
        if (*p == 'n') return -1;
        uint8_t t;
        if (strchr("diouxXcCpsSDOU", *p)) t = PA_INT;
        else if (strchr("eEfFgGaA", *p)) t = is_long_double ? PA_LDBL : PA_DBL;
        else continue;
        if (!printf_set(types, &count, pos >= 0 ? pos : next++, t)) return -1;
    }
    return count;
}

static uint64_t va_take_gp(LinuxVaList *va) {
    uint64_t v;
    if (va->gr_offs < 0) {
        memcpy(&v, (char *)va->gr_top + va->gr_offs, 8);
        va->gr_offs += 8;
    } else {
        memcpy(&v, va->stack, 8);
        va->stack = (char *)va->stack + 8;
    }
    return v;
}

static double va_take_double(LinuxVaList *va) {
    double d;
    if (va->vr_offs < 0) {
        memcpy(&d, (char *)va->vr_top + va->vr_offs, 8);
        va->vr_offs += 16;
    } else {
        memcpy(&d, va->stack, 8);
        va->stack = (char *)va->stack + 8;
    }
    return d;
}

// Linux arm64 long double is IEEE binary128; narrow it to double
// (truncating the low mantissa bits). Values below double's normal
// range become double subnormals instead of zero.
static double quad_to_double(const uint8_t q[16]) {
    uint64_t lo, hi;
    memcpy(&lo, q, 8);
    memcpy(&hi, q + 8, 8);
    int exp = (int)((hi >> 48) & 0x7fff);
    double frac = (double)(((hi & 0xffffffffffffULL) << 4) | (lo >> 60)) / 0x1p52;
    int nan = (hi & 0xffffffffffffULL) != 0 || lo != 0;
    double v = exp == 0x7fff ? (nan ? NAN : INFINITY)
             : exp == 0      ? ldexp(frac, -16382)
                             : ldexp(1.0 + frac, exp - 16383);
    return (hi >> 63) ? -v : v;
}

static double va_take_long_double(LinuxVaList *va) {
    uint8_t q[16];
    if (va->vr_offs < 0) {
        memcpy(q, (char *)va->vr_top + va->vr_offs, 16);
        va->vr_offs += 16;
    } else {
        uintptr_t s = ((uintptr_t)va->stack + 15) & ~(uintptr_t)15;
        memcpy(q, (void *)s, 16);
        va->stack = (void *)(s + 16);
    }
    return quad_to_double(q);
}

static int darwin_errno_from_linux(int err) {
    for (int d = 1; d <= ELAST; d++)
        if (linux_errno_from_darwin(d) == err) return d;
    return err;
}

// Appends strerror text for a %m spec (flags/width/precision only),
// escaping '%' so the result is a literal in the rewritten format.
// Returns 0, or -1 with a Linux errno.
static int printf_put_m(FILE *out, const char *spec, size_t len, int err) {
    if (strspn(spec + 1, "0123456789-+ #'.") != len - 1) {
        errno = L_EINVAL;
        return -1;
    }
    char *sfmt = malloc(len + 2);
    if (!sfmt) { fix_errno(); return -1; }
    memcpy(sfmt, spec, len);
    memcpy(sfmt + len, "s", 2);
    const char *msg = strerror(darwin_errno_from_linux(err));
    int n = snprintf(NULL, 0, sfmt, msg);
    char *text = n < 0 ? NULL : malloc((size_t)n + 1);
    if (!text) {
        int e = linux_errno_from_darwin(n < 0 ? errno : ENOMEM);
        free(sfmt);
        errno = e;
        return -1;
    }
    snprintf(text, (size_t)n + 1, sfmt, msg);
    free(sfmt);
    for (const char *t = text; *t; t++) {
        if (*t == '%') fputc('%', out);
        fputc(*t, out);
    }
    free(text);
    return 0;
}

// Darwin has no %m (Bionic: strerror(errno)). Sets *out to fmt if it has
// none, or to a malloc'd rewrite (*heap) with the message inlined.
// Returns 0, or -1 with a Linux errno: EINVAL if a %m spec has '*', '$'
// or a length modifier, otherwise the allocation/stream error.
static int printf_expand_m(const char *fmt, int err, const char **out,
                           char **heap) {
    *heap = NULL;
    *out = fmt;
    const char *p = fmt, *m = NULL;
    for (; *p; p++) {
        if (*p != '%') continue;
        const char *q = p + 1;
        if (*q == '%') { p = q; continue; }
        q += strspn(q, "0123456789$-+ #'.*hlLqjzt");
        if (*q == 'm') { m = p; break; }
        if (!*q) break;
        p = q;
    }
    if (!m) return 0;

    size_t size = 0;
    FILE *ms = open_memstream(heap, &size);
    if (!ms) { fix_errno(); return -1; }
    fwrite(fmt, 1, (size_t)(m - fmt), ms);
    int rc = 0;
    for (p = m; *p; p++) {
        if (*p != '%') { fputc(*p, ms); continue; }
        const char *q = p + 1;
        if (*q == '%') { fputs("%%", ms); p = q; continue; }
        q += strspn(q, "0123456789$-+ #'.*hlLqjzt");
        if (*q == 'm') {
            if ((rc = printf_put_m(ms, p, (size_t)(q - p), err)) < 0) break;
        } else {
            fwrite(p, 1, (size_t)(q - p) + (*q != 0), ms);
            if (!*q) break;
        }
        p = q;
    }
    int saved = errno;
    if (rc == 0 && ferror(ms)) { rc = -1; saved = L_ENOMEM; }
    if (fclose(ms) != 0 && rc == 0) { fix_errno(); rc = -1; saved = errno; }
    if (rc < 0) {
        free(*heap);
        *heap = NULL;
        errno = saved;
        return -1;
    }
    *out = *heap;
    return 0;
}

// Translated arguments for one printf-family call.
typedef struct {
    const char *fmt;
    char *heap;
    uint64_t slots[PRINTF_MAX_ARGS];
} PrintfCall;

// Prepares fmt and Darwin va_list slots; returns 0, or -1 with a Linux
// errno (EINVAL for an unsupported format). Pair with printf_end() on 0.
static int printf_begin(PrintfCall *c, const char *fmt, const LinuxVaList *ap) {
    int err = errno;
    if (printf_expand_m(fmt ? fmt : "", err, &c->fmt, &c->heap) < 0)
        return -1;
    uint8_t types[PRINTF_MAX_ARGS] = {0};
    int n = printf_arg_types(c->fmt, types);
    if (n < 0) {
        free(c->heap);
        c->heap = NULL;
        errno = L_EINVAL;
        return -1;
    }
    LinuxVaList va = *ap;
    for (int i = 0; i < n; i++) {
        double d;
        switch (types[i]) {
        case PA_DBL:
            d = va_take_double(&va);
            memcpy(&c->slots[i], &d, 8);
            break;
        case PA_LDBL:
            d = va_take_long_double(&va);
            memcpy(&c->slots[i], &d, 8);
            break;
        default:
            c->slots[i] = va_take_gp(&va);
            break;
        }
    }
    errno = err;
    return 0;
}

static int printf_end(PrintfCall *c, int ret) {
    if (ret < 0) fix_errno();
    int err = errno;
    free(c->heap);
    errno = err;
    return ret;
}

#define DARWIN_VA(c) ((va_list)(void *)(c).slots)

int linux_abi_vsnprintf(char *buf, size_t size, const char *fmt,
                        const LinuxVaList *ap) {
    PrintfCall c;
    if (printf_begin(&c, fmt, ap) < 0) {
        if (size) buf[0] = '\0';
        return -1;
    }
    return printf_end(&c, vsnprintf(buf, size, c.fmt, DARWIN_VA(c)));
}

__attribute__((used)) static int abi_vasprintf(char **out, const char *fmt, const LinuxVaList *ap) {
    PrintfCall c;
    if (printf_begin(&c, fmt, ap) < 0) { *out = NULL; return -1; }
    int ret = vasprintf(out, c.fmt, DARWIN_VA(c));
    if (ret < 0) *out = NULL;
    return printf_end(&c, ret);
}

__attribute__((used)) static int abi_vfprintf(FILE *stream, const char *fmt, const LinuxVaList *ap) {
    PrintfCall c;
    if (printf_begin(&c, fmt, ap) < 0) return -1;
    return printf_end(&c, vfprintf(android_stubs_fixup_file(stream), c.fmt,
                                   DARWIN_VA(c)));
}

__attribute__((used)) static int abi_vprintf(const char *fmt, const LinuxVaList *ap) {
    PrintfCall c;
    if (printf_begin(&c, fmt, ap) < 0) return -1;
    return printf_end(&c, vprintf(c.fmt, DARWIN_VA(c)));
}

__attribute__((used)) static int abi_vsyslog(int prio, const char *fmt, const LinuxVaList *ap) {
    PrintfCall c;
    if (printf_begin(&c, fmt, ap) == 0) {
        vsyslog(prio, c.fmt, DARWIN_VA(c));
        printf_end(&c, 0);
    }
    return 0;
}

LINUX_ABI_VARIADIC(abi_snprintf, linux_abi_vsnprintf, 3)
LINUX_ABI_VARIADIC(abi_asprintf, abi_vasprintf, 2)
LINUX_ABI_VARIADIC(abi_fprintf, abi_vfprintf, 2)
LINUX_ABI_VARIADIC(abi_printf, abi_vprintf, 1)
LINUX_ABI_VARIADIC(abi_syslog, abi_vsyslog, 2)

// ── pthread mutex/cond/rwlock/once via side table ────────────────────────────
// CRITICAL: Bionic pthread_mutex_t = 40 bytes, macOS = 64 bytes.
//           Bionic pthread_rwlock_t = 56 bytes, macOS = 200 bytes.
//           Passing .so's bionic-sized structs to macOS pthread functions causes
//           buffer overflow and memory corruption. We use a side-table to store
//           macOS-sized objects separately from the .so's memory.

#define SIDE_TABLE_SIZE 16384
#define SIDE_TOMB ((void *)1)  // deleted slot; keeps probe chains intact

enum { SIDE_MUTEX = 1, SIDE_COND = 2, SIDE_RWLOCK = 3 };

// Bionic mutex kinds, stored in bits 14-15 of the first 16-bit word.
enum { BIONIC_MUTEX_NORMAL = 0, BIONIC_MUTEX_RECURSIVE = 1,
       BIONIC_MUTEX_ERRORCHECK = 2 };

typedef struct {
    void *addr;         // bionic struct address in .so memory
    int type;           // SIDE_MUTEX, SIDE_COND or SIDE_RWLOCK
    int kind;           // bionic mutex kind
    union {
        pthread_mutex_t mutex;
        pthread_cond_t cond;
        pthread_rwlock_t rwlock;
    };
} SideEntry;

static SideEntry s_side_table[SIDE_TABLE_SIZE];
static os_unfair_lock s_side_lock = OS_UNFAIR_LOCK_INIT;

static void side_init_entry(SideEntry *e, void *addr, int type, int kind) {
    e->addr = addr;
    e->type = type;
    e->kind = kind;
    if (type == SIDE_MUTEX) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr,
            kind == BIONIC_MUTEX_RECURSIVE ? PTHREAD_MUTEX_RECURSIVE :
            kind == BIONIC_MUTEX_ERRORCHECK ? PTHREAD_MUTEX_ERRORCHECK :
            PTHREAD_MUTEX_NORMAL);
        pthread_mutex_init(&e->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
    } else if (type == SIDE_COND) {
        pthread_cond_init(&e->cond, NULL);
    } else {
        pthread_rwlock_init(&e->rwlock, NULL);
    }
}

static void side_destroy_entry(SideEntry *e) {
    if (e->type == SIDE_MUTEX) pthread_mutex_destroy(&e->mutex);
    else if (e->type == SIDE_COND) pthread_cond_destroy(&e->cond);
    else if (e->type == SIDE_RWLOCK) pthread_rwlock_destroy(&e->rwlock);
    e->addr = SIDE_TOMB;
    e->type = 0;
}

// Finds or creates the macOS object shadowing a bionic one. `replace`
// discards any existing entry (re-init after reuse of the memory); a
// type mismatch means the address was recycled for another primitive.
static SideEntry *side_lookup(void *addr, int type, int kind, int replace) {
    uint32_t hash = (uint32_t)(((uintptr_t)addr >> 3) % SIDE_TABLE_SIZE);
    SideEntry *free_slot = NULL;
    os_unfair_lock_lock(&s_side_lock);
    for (uint32_t i = 0; i < SIDE_TABLE_SIZE; i++) {
        SideEntry *e = &s_side_table[(hash + i) % SIDE_TABLE_SIZE];
        if (e->addr == addr) {
            if (replace || e->type != type) {
                side_destroy_entry(e);
                side_init_entry(e, addr, type, kind);
            }
            os_unfair_lock_unlock(&s_side_lock);
            return e;
        }
        if (e->addr == SIDE_TOMB) {
            if (!free_slot) free_slot = e;
            continue;
        }
        if (e->addr == NULL) {
            if (!free_slot) free_slot = e;
            break;
        }
    }
    if (free_slot) side_init_entry(free_slot, addr, type, kind);
    os_unfair_lock_unlock(&s_side_lock);
    return free_slot;  // NULL only if the table is full
}

static SideEntry *side_get(void *addr, int type) {
    return side_lookup(addr, type, 0, 0);
}

static void side_remove(void *addr) {
    uint32_t hash = (uint32_t)(((uintptr_t)addr >> 3) % SIDE_TABLE_SIZE);
    os_unfair_lock_lock(&s_side_lock);
    for (uint32_t i = 0; i < SIDE_TABLE_SIZE; i++) {
        SideEntry *e = &s_side_table[(hash + i) % SIDE_TABLE_SIZE];
        if (e->addr == addr) {
            side_destroy_entry(e);
            break;
        }
        if (e->addr == NULL) break;
    }
    os_unfair_lock_unlock(&s_side_lock);
}

// Statically initialized bionic mutexes encode their kind in the state word
// (PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP sets bit 14).
static int mutex_static_kind(void *m) {
    int kind = (*(volatile uint16_t *)m >> 14) & 3;
    return kind <= BIONIC_MUTEX_ERRORCHECK ? kind : BIONIC_MUTEX_NORMAL;
}

static SideEntry *mutex_entry(void *m) {
    return side_lookup(m, SIDE_MUTEX, mutex_static_kind(m), 0);
}

// Bionic pthread_mutexattr_t is a long holding the kind in its low bits.
static int stub_pthread_mutexattr_init(long *attr) {
    *attr = BIONIC_MUTEX_NORMAL;
    return 0;
}
static int stub_pthread_mutexattr_settype(long *attr, int type) {
    if (type < BIONIC_MUTEX_NORMAL || type > BIONIC_MUTEX_ERRORCHECK) {
        return 22;  // EINVAL
    }
    *attr = (*attr & ~0xfL) | type;
    return 0;
}
static int stub_pthread_mutexattr_destroy(long *attr) {
    *attr = -1;
    return 0;
}

// Mutex wrappers — .so passes bionic-sized (40-byte) mutex pointers.
// We look up/create a macOS mutex in the side table.
static int stub_pthread_mutex_lock(void *m) {
    SideEntry *e = mutex_entry(m);
    return e ? linux_errno_from_darwin(pthread_mutex_lock(&e->mutex)) : 22;
}
static int stub_pthread_mutex_unlock(void *m) {
    SideEntry *e = mutex_entry(m);
    return e ? linux_errno_from_darwin(pthread_mutex_unlock(&e->mutex)) : 22;
}
static int stub_pthread_mutex_trylock(void *m) {
    SideEntry *e = mutex_entry(m);
    return e ? linux_errno_from_darwin(pthread_mutex_trylock(&e->mutex)) : 22;
}
static int stub_pthread_mutex_init(void *m, const long *attr) {
    int kind = attr ? (int)(*attr & 0xf) : BIONIC_MUTEX_NORMAL;
    if (kind > BIONIC_MUTEX_ERRORCHECK) return 22;
    memset(m, 0, 40);
    *(uint16_t *)m = (uint16_t)(kind << 14);
    return side_lookup(m, SIDE_MUTEX, kind, 1) ? 0 : 11;  // EAGAIN
}
static int stub_pthread_mutex_destroy(void *m) {
    side_remove(m);
    return 0;
}

// Condition variable wrappers (bionic cond = 48 bytes, macOS = 48 — same size
// but different internal layout, so still use side table for correctness)
static int stub_pthread_cond_init(void *c, const void *attr) {
    (void)attr;
    return side_lookup(c, SIDE_COND, 0, 1) ? 0 : 11;  // EAGAIN
}
static int stub_pthread_cond_destroy(void *c) {
    side_remove(c);
    return 0;
}
static int stub_pthread_cond_wait(void *c, void *m) {
    SideEntry *ce = side_get(c, SIDE_COND);
    SideEntry *me = mutex_entry(m);
    if (!ce || !me) return 22;
    return linux_errno_from_darwin(pthread_cond_wait(&ce->cond, &me->mutex));
}
static int stub_pthread_cond_signal(void *c) {
    SideEntry *e = side_get(c, SIDE_COND);
    return e ? linux_errno_from_darwin(pthread_cond_signal(&e->cond)) : 22;
}
static int stub_pthread_cond_broadcast(void *c) {
    SideEntry *e = side_get(c, SIDE_COND);
    return e ? linux_errno_from_darwin(pthread_cond_broadcast(&e->cond)) : 22;
}
static int stub_pthread_cond_timedwait(void *c, void *m, const struct timespec *t) {
    SideEntry *ce = side_get(c, SIDE_COND);
    SideEntry *me = mutex_entry(m);
    if (!ce || !me) return 22;
    return linux_errno_from_darwin(
        pthread_cond_timedwait(&ce->cond, &me->mutex, t));
}

// pthread_once — Bionic: 4 bytes (int), macOS: 16 bytes.
// Bionic states: 0 = not started, 1 = underway, 2 = complete.
enum { ONCE_NEW = 0, ONCE_RUNNING = 1, ONCE_DONE = 2 };
#define ONCE_STACK_MAX 32
static __thread _Atomic uint32_t *t_once_stack[ONCE_STACK_MAX];
static __thread int t_once_depth;

static void once_publish(_Atomic uint32_t *flag, uint32_t state) {
    atomic_store(flag, state);
    linux_abi_wake_address((volatile uint32_t *)flag, 1);
}

static int stub_pthread_once(void *once, void (*init_routine)(void)) {
    _Atomic uint32_t *flag = (_Atomic uint32_t *)once;
    for (;;) {
        uint32_t state = atomic_load(flag);
        if (state == ONCE_DONE) return 0;
        if (state == ONCE_NEW &&
            atomic_compare_exchange_strong(flag, &state, ONCE_RUNNING)) {
            int depth = t_once_depth;
            if (depth < ONCE_STACK_MAX) t_once_stack[depth] = flag;
            t_once_depth = depth + 1;
            init_routine();
            // Publish before popping so a crash in between is still seen
            // by linux_abi_crash_recovered().
            once_publish(flag, ONCE_DONE);
            t_once_depth = depth;
            return 0;
        }
        if (state != ONCE_RUNNING) continue;
        // Recursive call from inside this thread's init_routine: Linux would
        // deadlock; returning lets the outer call finish.
        for (int i = 0; i < t_once_depth && i < ONCE_STACK_MAX; i++) {
            if (t_once_stack[i] == flag) return 0;
        }
        linux_abi_wait_on_address((volatile uint32_t *)flag, ONCE_RUNNING, 0);
    }
}

void linux_abi_crash_recovered(void) {
    // A crash longjmp'd out of init routines. Reset them to "not started"
    // so waiters retry the initializer instead of hanging forever or
    // using half-initialized state.
    int depth = t_once_depth < ONCE_STACK_MAX ? t_once_depth : ONCE_STACK_MAX;
    for (int i = depth - 1; i >= 0; i--) {
        uint32_t running = ONCE_RUNNING;
        if (atomic_compare_exchange_strong(t_once_stack[i], &running, ONCE_NEW)) {
            linux_abi_wake_address((volatile uint32_t *)t_once_stack[i], 1);
        }
    }
    t_once_depth = 0;
}

// Rwlock wrappers — Bionic rwlock = 56 bytes, macOS = 200 bytes!
static int stub_pthread_rwlock_rdlock(void *rw) {
    SideEntry *e = side_get(rw, SIDE_RWLOCK);
    return e ? linux_errno_from_darwin(pthread_rwlock_rdlock(&e->rwlock)) : 22;
}
static int stub_pthread_rwlock_wrlock(void *rw) {
    SideEntry *e = side_get(rw, SIDE_RWLOCK);
    return e ? linux_errno_from_darwin(pthread_rwlock_wrlock(&e->rwlock)) : 22;
}
static int stub_pthread_rwlock_unlock(void *rw) {
    SideEntry *e = side_get(rw, SIDE_RWLOCK);
    return e ? linux_errno_from_darwin(pthread_rwlock_unlock(&e->rwlock)) : 22;
}
static int stub_pthread_rwlock_tryrdlock(void *rw) {
    SideEntry *e = side_get(rw, SIDE_RWLOCK);
    return e ? linux_errno_from_darwin(pthread_rwlock_tryrdlock(&e->rwlock)) : 22;
}
static int stub_pthread_rwlock_trywrlock(void *rw) {
    SideEntry *e = side_get(rw, SIDE_RWLOCK);
    return e ? linux_errno_from_darwin(pthread_rwlock_trywrlock(&e->rwlock)) : 22;
}

// ── Symbol table ─────────────────────────────────────────────────────────────

#define E(name, fn) { name, (void *)(fn) }

static const SymEntry s_abi_table[] = {
    E("fwrite",                          stub_fwrite),
    E("fread",                           stub_fread),
    E("fflush",                          stub_fflush),
    E("fputs",                           stub_fputs),
    E("fputc",                           stub_fputc),
    E("fprintf",                         abi_fprintf),
    E("vfprintf",                        abi_vfprintf),
    E("printf",                          abi_printf),
    E("snprintf",                        abi_snprintf),
    E("vsnprintf",                       linux_abi_vsnprintf),
    E("asprintf",                        abi_asprintf),
    E("vasprintf",                       abi_vasprintf),
    E("syslog",                          abi_syslog),


    E("pthread_mutex_lock",              stub_pthread_mutex_lock),
    E("pthread_mutex_unlock",            stub_pthread_mutex_unlock),
    E("pthread_mutex_trylock",           stub_pthread_mutex_trylock),
    E("pthread_mutex_init",              stub_pthread_mutex_init),
    E("pthread_mutex_destroy",           stub_pthread_mutex_destroy),
    E("pthread_mutexattr_init",          stub_pthread_mutexattr_init),
    E("pthread_mutexattr_settype",       stub_pthread_mutexattr_settype),
    E("pthread_mutexattr_destroy",       stub_pthread_mutexattr_destroy),
    E("pthread_cond_init",               stub_pthread_cond_init),
    E("pthread_cond_destroy",            stub_pthread_cond_destroy),
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

    E("syscall",                     stub_syscall),
    E("open",                        stub_open),
    E("fcntl",                       stub_fcntl),
    E("ioctl",                       stub_ioctl),
    E("mmap",                        linux_mmap),
    E("mmap64",                      linux_mmap),
    E("madvise",                     stub_madvise),
    E("sysconf",                     stub_sysconf),
    E("sscanf",                      stub_sscanf),
    E("fscanf",                      stub_fscanf),
    E("vsscanf",                     stub_vsscanf),

    E("sem_init",                    stub_sem_init),
    E("sem_destroy",                 stub_sem_destroy),
    E("sem_post",                    stub_sem_post),
    E("sem_wait",                    stub_sem_wait),
    E("sem_trywait",                 stub_sem_trywait),
    E("sem_timedwait",               stub_sem_timedwait),

    E("sigaction",                   stub_sigaction),
    E("sigemptyset",                 stub_sigemptyset),
    E("sigfillset",                  stub_sigfillset),
    E("sigaddset",                   stub_sigaddset),
    E("sigdelset",                   stub_sigdelset),
    E("sigismember",                 stub_sigismember),
    E("pthread_sigmask",             stub_pthread_sigmask),
    E("sigaltstack",                 stub_sigaltstack),
    E("kill",                        stub_kill),
    E("pthread_kill",                stub_pthread_kill),

    E("stat",                        stub_stat),
    E("fstat",                       stub_fstat),
    E("statvfs",                     stub_statvfs),
    E("uname",                       stub_uname),
    E("opendir",                     stub_opendir),
    E("readdir",                     stub_readdir),
    E("readdir_r",                   stub_readdir_r),
    E("closedir",                    stub_closedir),

    E("pthread_attr_init",           stub_pthread_attr_init),
    E("pthread_attr_destroy",        stub_pthread_attr_destroy),
    E("pthread_attr_setdetachstate", stub_pthread_attr_setdetachstate),
    E("pthread_attr_setstacksize",   stub_pthread_attr_setstacksize),
    E("pthread_attr_setguardsize",   stub_pthread_attr_setguardsize),
    E("pthread_attr_getguardsize",   stub_pthread_attr_getguardsize),
    E("pthread_attr_setschedparam",  stub_pthread_attr_setschedparam),
    E("pthread_attr_getschedparam",  stub_pthread_attr_getschedparam),
    E("pthread_attr_setschedpolicy", stub_pthread_attr_setschedpolicy),
    E("pthread_attr_setscope",       stub_pthread_attr_setscope),
    E("pthread_create",              stub_pthread_create),
    E("pthread_key_create",          stub_pthread_key_create),
    E("pthread_key_delete",          stub_pthread_key_delete),
    E("pthread_getspecific",         stub_pthread_getspecific),
    E("pthread_setspecific",         stub_pthread_setspecific),
    E("pthread_setname_np",          stub_pthread_setname_np),
    E("pthread_getschedparam",       stub_pthread_getschedparam),
    E("sched_get_priority_max",      stub_sched_get_priority_max),

    E("getrlimit",                   stub_getrlimit),
    E("setrlimit",                   stub_setrlimit),
    E("clock_gettime",               stub_clock_gettime),
    E("gettimeofday",                stub_gettimeofday),
    E("getrusage",                   stub_getrusage),
    E("setlocale",                   stub_setlocale),
    E("newlocale",                   stub_newlocale),
    { NULL, NULL }
};

const SymEntry *linux_abi_table(void) { return s_abi_table; }
