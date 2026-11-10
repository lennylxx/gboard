// Build-time generator for libintegrated_shared_object.so.ctorprobe.
// Usage: gen-ctor-probe [--verify] <path/to/libintegrated_shared_object.so> <pack_dir>
//   --verify  load the library and exit 0 only if elf_load() used the file.
#include <stdio.h>
#include <string.h>
#include "engine/android_stubs.h"
#include "engine/elf_loader.h"

int main(int argc, char **argv) {
    int verify = argc == 4 && strcmp(argv[1], "--verify") == 0;
    if (argc != 3 + verify) {
        fprintf(stderr, "Usage: %s [--verify] <so_path> <pack_dir>\n", argv[0]);
        return 2;
    }
    const char *so_path = argv[1 + verify], *pack_dir = argv[2 + verify];
    android_stubs_init(pack_dir);
    if (verify) {
        ElfHandle *h = elf_load(so_path);
        if (!h || !elf_ctor_probe_cached(h)) {
            fprintf(stderr, "elf_load did not use the constructor probe for %s\n",
                    so_path);
            return 1;
        }
        return 0;
    }
    if (!elf_write_ctor_probe(so_path)) {
        fprintf(stderr, "Failed to generate constructor probe for %s\n", so_path);
        return 1;
    }
    return 0;
}
