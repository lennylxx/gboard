#pragma once
#include <stdint.h>

// Exposes the symbol table that elf_loader uses for resolution.
// Call android_stubs_init() before elf_load().
void android_stubs_init(const char *asset_base_dir);

// Returns pointer to {name, addr} table terminated by {NULL,NULL}.
typedef struct { const char *name; void *addr; } SymEntry;
const SymEntry *android_stubs_table(void);

// Bionic TLS compatibility — set/restore TPIDR_EL0 for .so code.
void bionic_tls_setup(void);
void bionic_tls_enter(void);  // set TPIDR_EL0 to fake TLS
void bionic_tls_leave(void);  // restore original TPIDR_EL0
