#pragma once

// linux_abi: imports that macOS DOES provide under the same name, but
// with a Linux/Bionic (arm64, LP64) ABI the macOS version does not
// match. Without these shims the loader would bind them to libSystem
// via dlsym and the .so would silently get wrong behavior.
//
// Each shim translates one of:
// - constants: open/fcntl/mmap flags, clock IDs, signal numbers,
//   sysconf/rlimit/locale selectors, errno values
// - struct layouts: stat, statvfs, dirent, flock, sigaction,
//   pthread_attr_t, rusage/timeval, Bionic FILE* (via __sF)
// - object sizes: Bionic pthread mutex/cond/rwlock/once/key/sem_t are
//   smaller than macOS's, so real objects live in a side table
// - calling convention: scanf/printf varargs (AAPCS64 vs Darwin
//   variadics), via LINUX_ABI_VARIADIC trampolines
// - kernel interfaces: syscall(2), including futex via __ulock
//
// Symbols macOS lacks entirely belong in android_stubs.c. Entries in
// linux_abi_table() take precedence over android_stubs_table().

#include <stddef.h>
#include <stdint.h>
#include "android_stubs.h"

const SymEntry *linux_abi_table(void);

// Translates a macOS errno value to the Linux value the .so expects.
int linux_errno_from_darwin(int err);

// Resets this thread's in-progress pthread_once initializers to "not
// started". Call after a crash inside .so code is recovered via longjmp.
void linux_abi_crash_recovered(void);

// Darwin futex-style wait/wake on a 32-bit word (libSystem __ulock API).
// Returns 0 on wake or value mismatch, ETIMEDOUT/EINTR (macOS values) on
// failure. timeout_us == 0 waits forever.
int linux_abi_wait_on_address(volatile uint32_t *addr, uint32_t expected,
                              uint32_t timeout_us);
void linux_abi_wake_address(volatile uint32_t *addr, int all);

// AAPCS64 (Linux) va_list. As a 32-byte struct it is passed by reference,
// so .so calls like vsnprintf(..., ap) hand us a const LinuxVaList *.
typedef struct {
    void *stack;
    void *gr_top;
    void *vr_top;
    int32_t gr_offs;
    int32_t vr_offs;
} LinuxVaList;

// vsnprintf for a format and va_list coming from .so code.
int linux_abi_vsnprintf(char *buf, size_t size, const char *fmt,
                        const LinuxVaList *ap);

// Defines `name` as a static entry point for a variadic .so import.
// Linux callers pass varargs in x/q registers (AAPCS64); Darwin variadic
// functions expect them on the stack. The trampoline spills x0-x7 and
// q0-q7, builds a LinuxVaList over them and the caller's stack args, and
// calls impl(fixed args..., const LinuxVaList *). nfixed is the
// number of named (integer/pointer) parameters, 1-7. impl must be
// marked __attribute__((used)) if it is otherwise unreferenced.
#define LINUX_ABI_STR_(x) #x
#define LINUX_ABI_STR(x) LINUX_ABI_STR_(x)
#define LINUX_ABI_VARIADIC(name, impl, nfixed)                              \
    __attribute__((naked)) static int name(void) {                          \
        __asm__(                                                            \
            "sub sp, sp, #240\n"                                            \
            "stp x29, x30, [sp]\n"                                          \
            "mov x29, sp\n"                                                 \
            "stp x0, x1, [sp, #48]\n"                                       \
            "stp x2, x3, [sp, #64]\n"                                       \
            "stp x4, x5, [sp, #80]\n"                                       \
            "stp x6, x7, [sp, #96]\n"                                       \
            "stp q0, q1, [sp, #112]\n"                                      \
            "stp q2, q3, [sp, #144]\n"                                      \
            "stp q4, q5, [sp, #176]\n"                                      \
            "stp q6, q7, [sp, #208]\n"                                      \
            "add x9, sp, #240\n"   /* stack: caller's outgoing args */      \
            "str x9, [sp, #16]\n"                                           \
            "add x9, sp, #112\n"   /* gr_top: end of x save area */         \
            "str x9, [sp, #24]\n"                                           \
            "add x9, sp, #240\n"   /* vr_top: end of q save area */         \
            "str x9, [sp, #32]\n"                                           \
            "mov w9, #" LINUX_ABI_STR(((nfixed) - 8) * 8) "\n"              \
            "str w9, [sp, #40]\n"                                           \
            "mov w9, #-128\n"                                               \
            "str w9, [sp, #44]\n"                                           \
            "add x" LINUX_ABI_STR(nfixed) ", sp, #16\n"                     \
            "bl _" #impl "\n"                                               \
            "ldp x29, x30, [sp]\n"                                          \
            "add sp, sp, #240\n"                                            \
            "ret\n");                                                       \
    }
