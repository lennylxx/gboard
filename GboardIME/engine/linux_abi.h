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
// - calling convention: scanf varargs (AAPCS64 vs Darwin variadics)
// - kernel interfaces: syscall(2), including futex via __ulock
//
// Symbols macOS lacks entirely belong in android_stubs.c. Entries in
// linux_abi_table() take precedence over android_stubs_table().

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
