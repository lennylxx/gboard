#pragma once
#include <stddef.h>

typedef struct ElfHandle ElfHandle;

// Load an ARM64 ELF shared library into memory on macOS.
// Resolves symbols against the provided table + macOS libc.
ElfHandle *elf_load(const char *path);

// Look up an exported symbol by name.
void *elf_sym(ElfHandle *h, const char *name);

void elf_unload(ElfHandle *h);

// Get the load bias (for accessing data at known file offsets)
void *elf_bias(ElfHandle *h);
