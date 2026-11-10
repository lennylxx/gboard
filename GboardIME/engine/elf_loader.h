#pragma once
#include <stddef.h>

typedef struct ElfHandle ElfHandle;

// Load an ARM64 ELF shared library into memory on macOS.
// Resolves symbols against the provided table + macOS libc.
ElfHandle *elf_load(const char *path);

// Build-time helper: load the library, fork-probe every constructor, scan
// the code for TPIDR_EL0 reads, and write "<path>.ctorprobe" for elf_load()
// to reuse. The library stays loaded,
// so call this from a short-lived tool process. Returns 1 on success.
int elf_write_ctor_probe(const char *path);

// Returns 1 if elf_load() took constructor results and TPIDR sites from the
// build-time "<path>.ctorprobe" file instead of probing and scanning at
// runtime.
int elf_ctor_probe_cached(const ElfHandle *h);

// Look up an exported symbol by name.
void *elf_sym(ElfHandle *h, const char *name);

void elf_unload(ElfHandle *h);

// Get the load bias (for accessing data at known file offsets)
void *elf_bias(ElfHandle *h);
