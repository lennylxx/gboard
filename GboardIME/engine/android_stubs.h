#pragma once

// android_stubs: symbols the Android .so imports that do NOT exist on
// macOS, plus the loader-lifetime state they need.
//
// - Bionic-only libc internals: __sF/stdin/stdout/stderr, __errno,
//   __pthread_cleanup_push/pop, __register_atfork, __cxa_atexit...
// - Linux-only calls macOS lacks: gettid, prctl, epoll, eventfd,
//   timerfd, inotify, mremap, sched_*affinity, getauxval...
// - Android NDK APIs: __android_log_*, __system_property_*, AAsset*,
//   AAudio*, AndroidBitmap*, AStatus*.
// - App-specific hooks: dlopen/dlsym for the data bundle, ICU data,
//   no-op internals (absl leak checks, gcov, MallocExtension...).
// - Bionic TLS (TPIDR_EL0) and captured static destructors.
//
// Functions that macOS DOES provide under the same name but with a
// different ABI (constants, struct layouts, errno, varargs) belong in
// linux_abi.c instead.

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

// Call android_stubs_init() before elf_load().
void android_stubs_init(const char *asset_base_dir);

// Returns pointer to {name, addr} table terminated by {NULL,NULL}.
typedef struct { const char *name; void *addr; } SymEntry;
const SymEntry *android_stubs_table(void);

// Bionic TLS compatibility — set/restore TPIDR_EL0 for .so code.
void bionic_tls_setup(void);
void bionic_tls_enter(void);  // set TPIDR_EL0 to fake TLS
void bionic_tls_leave(void);  // restore original TPIDR_EL0

// Run captured __cxa_atexit destructors. Call before elf_unload().
void android_stubs_run_atexit(void);

// Bionic's stdin/stdout/stderr live in a fake __sF; maps them to macOS.
FILE *android_stubs_fixup_file(FILE *f);

// Linux gettid(): the kernel thread ID of the calling thread.
pid_t android_stubs_gettid(void);
