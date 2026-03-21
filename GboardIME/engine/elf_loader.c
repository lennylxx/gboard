// ARM64 ELF shared library loader for macOS
// Loads an Android .so (ELF) into memory, resolves symbols, applies
// relocations, and runs init functions — all without dlopen.

#include "elf_loader.h"
#include "android_stubs.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/wait.h>

// ── ELF types ────────────────────────────────────────────────────────────────
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

#define EI_NIDENT 16
typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half    e_type, e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;
    Elf64_Off     e_phoff, e_shoff;
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize, e_phentsize, e_phnum;
    Elf64_Half    e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  p_type, p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr, p_paddr;
    Elf64_Xword p_filesz, p_memsz, p_align;
} Elf64_Phdr;

typedef struct {
    Elf64_Sxword d_tag;
    union { Elf64_Xword d_val; Elf64_Addr d_ptr; } d_un;
} Elf64_Dyn;

typedef struct {
    Elf64_Word st_name, st_info, st_other;  // note: st_info is uint8 packed
    Elf64_Half st_shndx;
    Elf64_Addr st_value;
    Elf64_Xword st_size;
} Elf64_Sym;

typedef struct {
    Elf64_Addr  r_offset;
    Elf64_Xword r_info;
    Elf64_Sxword r_addend;
} Elf64_Rela;

// Redefine to match actual ELF64 layout (st_info is 1 byte, not 4)
#pragma pack(push,1)
typedef struct {
    Elf64_Word  st_name;
    uint8_t     st_info;
    uint8_t     st_other;
    Elf64_Half  st_shndx;
    Elf64_Addr  st_value;
    Elf64_Xword st_size;
} Elf64_Sym_real;
#pragma pack(pop)

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_PLTGOT   3
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_STRSZ   10
#define DT_SYMENT  11
#define DT_INIT    12
#define DT_FINI    13
#define DT_JMPREL  23
#define DT_PLTREL  20
#define DT_INIT_ARRAY   25
#define DT_FINI_ARRAY   26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_GNU_HASH 0x6ffffef5
#define DT_ANDROID_RELA   0x60000011
#define DT_ANDROID_RELASZ 0x60000012

// Android packed relocation group flags
#define RELOCATION_GROUPED_BY_INFO_FLAG         1
#define RELOCATION_GROUPED_BY_OFFSET_DELTA_FLAG 2
#define RELOCATION_GROUPED_BY_ADDEND_FLAG       4
#define RELOCATION_GROUP_HAS_ADDEND_FLAG        8

// ARM64 relocation types
#define R_AARCH64_NONE        0
#define R_AARCH64_ABS64     257
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE  1027
#define R_AARCH64_IRELATIVE 1032
#define R_AARCH64_TLS_TPREL 1030

#define ELF64_R_SYM(i)  ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
#define STB_WEAK 2
#define STB_BIND(i) ((i) >> 4)

struct ElfHandle {
    void        *load_base;   // mmap base of reserved range
    size_t       load_size;   // total reserved size
    uint8_t     *bias;        // load_base - min_vaddr  (add to vaddr)

    // dynamic section cached fields
    const char       *strtab;
    Elf64_Sym_real   *symtab;
    size_t            symtab_count;
    Elf64_Rela       *rela;
    size_t            rela_count;
    Elf64_Rela       *jmprel;
    size_t            jmprel_count;

    void (**init_array)(void);
    size_t init_array_count;
};

// ── Constructor crash protection (sigsetjmp-based, used in fork child) ───────
static sigjmp_buf s_ctor_jmp;
static void ctor_crash_handler(int sig) { siglongjmp(s_ctor_jmp, sig); }

// ── Logging via os_log ───────────────────────────────────────────────────────
#include <os/log.h>
#include <stdarg.h>
#if DEBUG
static os_log_t elf_os_log(void) {
    static os_log_t log;
    static int once;
    if (!once) { log = os_log_create(BUNDLE_ID, "elf"); once = 1; }
    return log;
}
static void elf_log(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    os_log_debug(elf_os_log(), "%{public}s", buf);
}
#else
#define elf_log(...) ((void)0)
#endif

// ── Symbol resolution (hash table for O(1) stub lookup) ──────────────────────

#define STUB_HT_BUCKETS 256  // power of 2

typedef struct StubHTEntry {
    const char *name;
    void       *addr;
    struct StubHTEntry *next;
} StubHTEntry;

static StubHTEntry *s_stub_ht[STUB_HT_BUCKETS];
static StubHTEntry *s_stub_pool;
static int s_stub_ht_ready;

static uint32_t stub_hash(const char *s) {
    uint32_t h = 5381;
    for (; *s; s++) h = h * 33 + (uint8_t)*s;
    return h;
}

static void stub_ht_init(void) {
    if (s_stub_ht_ready) return;
    const SymEntry *stubs = android_stubs_table();
    if (!stubs) { s_stub_ht_ready = 1; return; }

    // Count entries
    size_t n = 0;
    for (const SymEntry *e = stubs; e->name; e++) n++;

    // Allocate pool
    s_stub_pool = calloc(n, sizeof(StubHTEntry));

    // Insert into hash table
    size_t idx = 0;
    for (const SymEntry *e = stubs; e->name; e++, idx++) {
        uint32_t bucket = stub_hash(e->name) & (STUB_HT_BUCKETS - 1);
        s_stub_pool[idx].name = e->name;
        s_stub_pool[idx].addr = e->addr;
        s_stub_pool[idx].next = s_stub_ht[bucket];
        s_stub_ht[bucket] = &s_stub_pool[idx];
    }
    s_stub_ht_ready = 1;
}

static void *resolve_symbol(const char *name) {
    // 1. Check Android stubs via hash table
    uint32_t bucket = stub_hash(name) & (STUB_HT_BUCKETS - 1);
    for (StubHTEntry *e = s_stub_ht[bucket]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e->addr;
    }

    // 2. Fall back to macOS dyld (handles all standard libc/pthread/math)
    void *sym = dlsym(RTLD_DEFAULT, name);
    if (sym) return sym;

    // 3. Some Bionic names differ slightly from macOS
    // pthread cleanup push/pop are macros in Bionic that expand to these
    if (strcmp(name, "__pthread_cleanup_push") == 0 ||
        strcmp(name, "__pthread_cleanup_pop")  == 0) return (void*)1; // no-op thunk

    // 4. Warn about unresolved but don't crash — weak symbols are OK
    elf_log("UNRESOLVED symbol: %s\n", name);
    return NULL;
}

// ── GNU hash lookup for exported symbols ─────────────────────────────────────

typedef struct {
    uint32_t nbuckets, symoffset, bloom_size, bloom_shift;
    // followed by: bloom[bloom_size], buckets[nbuckets], chains[]
} GnuHashHdr;

static void *gnu_hash_lookup(ElfHandle *h, const GnuHashHdr *gnu,
                              const char *name)
{
    uint32_t hash = 5381;
    for (const uint8_t *p = (const uint8_t *)name; *p; p++)
        hash = hash * 33 + *p;

    const uint64_t *bloom = (const uint64_t *)(gnu + 1);
    uint64_t word = bloom[(hash / 64) % gnu->bloom_size];
    uint64_t mask = (1ULL << (hash % 64)) |
                    (1ULL << ((hash >> gnu->bloom_shift) % 64));
    if ((word & mask) != mask) return NULL;

    const uint32_t *buckets = (const uint32_t *)(bloom + gnu->bloom_size);
    const uint32_t *chains  = buckets + gnu->nbuckets;

    uint32_t bucket = buckets[hash % gnu->nbuckets];
    if (bucket < gnu->symoffset) return NULL;

    for (uint32_t i = bucket; ; i++) {
        uint32_t chain_hash = chains[i - gnu->symoffset];
        const Elf64_Sym_real *sym = &h->symtab[i];
        const char *sym_name = h->strtab + sym->st_name;
        if (((chain_hash ^ hash) & ~1) == 0 && strcmp(sym_name, name) == 0) {
            if (sym->st_value == 0) return NULL;
            return h->bias + sym->st_value;
        }
        if (chain_hash & 1) break;
    }
    return NULL;
}

// We'll store gnu_hash ptr in handle for elf_sym
static const GnuHashHdr *g_gnu_hash;

void *elf_sym(ElfHandle *h, const char *name) {
    if (g_gnu_hash) return gnu_hash_lookup(h, g_gnu_hash, name);
    // Fallback: linear scan of symtab
    for (size_t i = 0; i < h->symtab_count; i++) {
        const Elf64_Sym_real *sym = &h->symtab[i];
        if (sym->st_value == 0) continue;
        if (strcmp(h->strtab + sym->st_name, name) == 0)
            return h->bias + sym->st_value;
    }
    return NULL;
}

// ── LEB128 decoding ──────────────────────────────────────────────────────────

static int64_t decode_sleb128(const uint8_t **pp, const uint8_t *end) {
    int64_t result = 0;
    unsigned shift = 0;
    const uint8_t *p = *pp;
    uint8_t byte = 0;
    while (p < end) {
        byte = *p++;
        result |= (int64_t)(byte & 0x7f) << shift;
        shift += 7;
        if ((byte & 0x80) == 0) break;
    }
    // Sign extend if the high bit of the last byte was set
    if (shift < 64 && (byte & 0x40))
        result |= -(1LL << shift);
    *pp = p;
    return result;
}

// ── Relocation application ────────────────────────────────────────────────────

static void apply_rela(ElfHandle *h, const Elf64_Rela *rela, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint64_t *target = (uint64_t *)(h->bias + rela[i].r_offset);
        uint32_t  type   = ELF64_R_TYPE(rela[i].r_info);
        uint32_t  sym_i  = ELF64_R_SYM(rela[i].r_info);
        int64_t   addend = rela[i].r_addend;

        void *sym_addr = NULL;
        if (sym_i != 0) {
            const Elf64_Sym_real *sym = &h->symtab[sym_i];
            if (sym->st_value != 0)
                sym_addr = h->bias + sym->st_value;
            else {
                const char *name = h->strtab + sym->st_name;
                sym_addr = resolve_symbol(name);
                if (!sym_addr && STB_BIND(sym->st_info) == STB_WEAK)
                    sym_addr = NULL; // weak unresolved is OK
            }
        }

        switch (type) {
        case R_AARCH64_RELATIVE:
            *target = (uint64_t)h->bias + (uint64_t)addend;
            break;
        case R_AARCH64_ABS64:
            *target = (uint64_t)sym_addr + (uint64_t)addend;
            break;
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
            if (sym_addr) *target = (uint64_t)sym_addr;
            break;
        case R_AARCH64_IRELATIVE:
            // GNU indirect function — call resolver at bias+addend
            { typedef void *(*ifunc_t)(void);
              ifunc_t fn = (ifunc_t)(h->bias + addend);
              *target = (uint64_t)fn(); }
            break;
        case R_AARCH64_NONE:
        case R_AARCH64_TLS_TPREL:
            break; // ignore TLS for now
        default:
            { char _eb[256]; int _en = snprintf(_eb,sizeof(_eb), "[elf_loader] unhandled reloc type %u\n", type); write(STDERR_FILENO,_eb,_en>0?(size_t)_en:0); }
            break;
        }
    }
}


// ── Android packed relocation decoder (DT_ANDROID_RELA / "APS2") ─────────────

static void decode_android_rela(ElfHandle *h, const uint8_t *data, size_t size) {
    const uint8_t *p = data;
    const uint8_t *end = data + size;

    // Verify "APS2" magic
    if (size < 4 || memcmp(p, "APS2", 4) != 0) {
        { char _eb[256]; int _en = snprintf(_eb,sizeof(_eb), "[elf_loader] android_rela: bad magic (expected APS2)\n"); write(STDERR_FILENO,_eb,_en>0?(size_t)_en:0); }
        return;
    }
    p += 4;

    size_t reloc_count = (size_t)decode_sleb128(&p, end);
    uint64_t r_offset = (uint64_t)decode_sleb128(&p, end);  // initial offset
    elf_log("android_rela: %zu packed relocations, initial_offset=0x%llx\n",
            reloc_count, (unsigned long long)r_offset);
    size_t applied = 0;
    int64_t addend = 0;  // persists across groups

    while (applied < reloc_count && p < end) {
        int64_t group_size   = decode_sleb128(&p, end);
        int64_t group_flags  = decode_sleb128(&p, end);

        uint64_t group_offset_delta = 0;
        uint64_t group_r_info = 0;

        if (group_flags & RELOCATION_GROUPED_BY_OFFSET_DELTA_FLAG)
            group_offset_delta = (uint64_t)decode_sleb128(&p, end);
        if (group_flags & RELOCATION_GROUPED_BY_INFO_FLAG)
            group_r_info = (uint64_t)decode_sleb128(&p, end);

        if ((group_flags & RELOCATION_GROUPED_BY_ADDEND_FLAG) &&
            (group_flags & RELOCATION_GROUP_HAS_ADDEND_FLAG))
            addend = decode_sleb128(&p, end);  // group sets addend directly
        else if (!(group_flags & RELOCATION_GROUP_HAS_ADDEND_FLAG))
            addend = 0;  // no addend for this group

        for (int64_t i = 0; i < group_size && applied < reloc_count; i++, applied++) {
            Elf64_Rela rela;

            // r_offset: accumulates across all relocations
            if (group_flags & RELOCATION_GROUPED_BY_OFFSET_DELTA_FLAG) {
                r_offset += group_offset_delta;
            } else {
                r_offset += (uint64_t)decode_sleb128(&p, end);
            }
            rela.r_offset = r_offset;

            // r_info
            if (group_flags & RELOCATION_GROUPED_BY_INFO_FLAG) {
                rela.r_info = group_r_info;
            } else {
                rela.r_info = (uint64_t)decode_sleb128(&p, end);
            }

            // r_addend
            if (group_flags & RELOCATION_GROUP_HAS_ADDEND_FLAG) {
                if (!(group_flags & RELOCATION_GROUPED_BY_ADDEND_FLAG)) {
                    addend += decode_sleb128(&p, end);
                }
                rela.r_addend = addend;
            } else {
                rela.r_addend = 0;
            }

            apply_rela(h, &rela, 1);
        }
    }

    elf_log("android_rela: applied %zu of %zu packed relocations\n", applied, reloc_count);
    // Log a few sample RELATIVE relocs that were applied
    elf_log("android_rela: final r_offset=0x%llx\n", (unsigned long long)r_offset);
}

// ── TPIDR_EL0 binary patching ────────────────────────────────────────────────
// macOS kernel resets TPIDR_EL0 on every syscall, so we can't change it.
// Instead, patch all `mrs xN, TPIDR_EL0` instructions in the .so to
// `ldr xN, [PC + offset]` loading from a literal containing our fake TLS address.

extern uint8_t s_fake_tls[];  // from android_stubs.c

// Segment range for targeted TPIDR scanning
typedef struct {
    uint32_t *code;
    size_t    n_insns;
} CodeRange;

static void elf_patch_tpidr(uint8_t *bias, const Elf64_Phdr *phdrs, int phnum,
                             void *load_base, size_t load_size) {
#if defined(__aarch64__) || defined(__arm64__)
    // Strategy: replace every `mrs xN, TPIDR_EL0` with `b trampoline_i`.
    // Each patched instruction gets its own trampoline that:
    //   1. Loads the fake TLS address into xN (ADRP + ADD)
    //   2. Branches back to the instruction after the original mrs (B return)
    //
    // CRITICAL: We use B (not BL) to avoid clobbering LR (x30).
    //
    // OPTIMIZATION: Only scan executable segments (PF_X), not the entire
    // load range. Typically ~8-16MB of code vs ~80MB total.

    uint64_t tls_addr = (uint64_t)s_fake_tls;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);

    // Collect executable segment ranges
    CodeRange ranges[32];
    int n_ranges = 0;
    size_t total_code_bytes = 0;
    for (int i = 0; i < phnum && n_ranges < 32; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (!(phdrs[i].p_flags & 1)) continue;  // PF_X = 1
        Elf64_Addr seg_start = phdrs[i].p_vaddr & ~(page - 1);
        Elf64_Addr seg_end = (phdrs[i].p_vaddr + phdrs[i].p_memsz + page - 1) & ~(page - 1);
        size_t seg_size = (size_t)(seg_end - seg_start);
        ranges[n_ranges].code = (uint32_t *)(bias + seg_start);
        ranges[n_ranges].n_insns = seg_size / 4;
        total_code_bytes += seg_size;
        n_ranges++;
    }

    if (n_ranges == 0) return;

    // Single pass: pre-allocate generous trampoline area.
    // Worst case: every instruction is mrs → 12 bytes per trampoline.
    // Realistic: ~100 mrs in ~16MB code → need ~1200 bytes.
    // Pre-allocate 64KB which handles up to ~5400 trampolines.
    size_t tramp_size = 0x10000;  // 64KB

    uintptr_t hint = (uintptr_t)load_base + load_size;
    hint = (hint + 0x3FFF) & ~0x3FFFULL;
    void *tramp_page = mmap((void *)hint, tramp_size, PROT_READ | PROT_WRITE,
                            MAP_ANON | MAP_PRIVATE, -1, 0);
    if (tramp_page == MAP_FAILED) {
        tramp_page = mmap(NULL, tramp_size, PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
    }
    if (tramp_page == MAP_FAILED) {
        elf_log("TPIDR patch: failed to alloc trampoline area\n");
        return;
    }

    elf_log("TPIDR patch: scanning %d exec segments (%zu bytes of %zu total), tramp at %p\n",
            n_ranges, total_code_bytes, load_size, tramp_page);

    // Single pass: scan and patch simultaneously
    uint32_t *tp = (uint32_t *)tramp_page;
    uint32_t *tp_end = (uint32_t *)((uint8_t *)tramp_page + tramp_size - 12);
    int patched = 0, unreachable = 0;

    for (int r = 0; r < n_ranges; r++) {
        uint32_t *code = ranges[r].code;
        size_t n_insns = ranges[r].n_insns;

        for (size_t i = 0; i < n_insns; i++) {
            uint32_t insn = code[i];
            if ((insn & 0xFFFFFFE0) != 0xd53bd040) continue;

            if (tp > tp_end) { unreachable++; continue; }

            int rd = insn & 0x1F;
            uintptr_t insn_pc = (uintptr_t)&code[i];
            uintptr_t tramp_pc = (uintptr_t)&tp[0];
            uintptr_t return_pc = insn_pc + 4;

            // Check branch ranges (±128MB)
            intptr_t fwd_offset = (intptr_t)(tramp_pc - insn_pc);
            if (fwd_offset < -0x8000000 || fwd_offset > 0x7FFFFFC) {
                unreachable++;
                continue;
            }
            intptr_t ret_offset = (intptr_t)(return_pc - (uintptr_t)&tp[2]);
            if (ret_offset < -0x8000000 || ret_offset > 0x7FFFFFC) {
                unreachable++;
                continue;
            }

            // Build trampoline: adrp + add + b_return
            intptr_t page_diff = (intptr_t)((tls_addr & ~0xFFFULL) - (tramp_pc & ~0xFFFULL));
            int64_t immval = page_diff >> 12;
            uint32_t adrp = 0x90000000 | (((uint32_t)(immval & 3)) << 29) |
                            (((uint32_t)((immval >> 2) & 0x7FFFF)) << 5) | (uint32_t)rd;
            uint32_t add = 0x91000000 | (((uint32_t)(tls_addr & 0xFFF)) << 10) |
                           ((uint32_t)rd << 5) | (uint32_t)rd;
            uint32_t b_ret = 0x14000000 | ((uint32_t)((ret_offset >> 2) & 0x3FFFFFF));

            tp[0] = adrp;
            tp[1] = add;
            tp[2] = b_ret;
            tp += 3;

            // Patch mrs → B trampoline
            code[i] = 0x14000000 | ((uint32_t)((fwd_offset >> 2) & 0x3FFFFFF));
            patched++;
        }
    }

    // Make trampoline area executable
    mprotect(tramp_page, tramp_size, PROT_READ | PROT_EXEC);

    // Flush instruction cache (only for exec segments + trampoline)
    for (int r = 0; r < n_ranges; r++) {
        __builtin___clear_cache((char *)ranges[r].code,
                                (char *)ranges[r].code + ranges[r].n_insns * 4);
    }
    __builtin___clear_cache((char *)tramp_page, (char *)tramp_page + tramp_size);

    elf_log("TPIDR patch: %d patched, %d unreachable (scanned %zu bytes of code)\n",
            patched, unreachable, total_code_bytes);
#endif
}

// ── Main loader ───────────────────────────────────────────────────────────────

ElfHandle *elf_load(const char *path) {
    stub_ht_init();

    int fd = open(path, O_RDONLY);
    if (fd < 0) { elf_log("open(%s) failed: %s\n", path, strerror(errno)); return NULL; }

    struct stat st;
    fstat(fd, &st);
    size_t file_size = (size_t)st.st_size;

    uint8_t *file = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    // Keep fd open — we need it for file-backed segment mappings below.
    if (file == MAP_FAILED) { elf_log("mmap file failed: %s\n", strerror(errno)); close(fd); return NULL; }
    elf_log("elf_load: path=%s size=%zu\n", path, file_size);

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)file;
    if (memcmp(ehdr->e_ident, "\x7f""ELF", 4) != 0) {
        { char _eb[256]; int _en = snprintf(_eb,sizeof(_eb), "[elf_loader] not an ELF file\n"); write(STDERR_FILENO,_eb,_en>0?(size_t)_en:0); }
        munmap(file, file_size); return NULL;
    }
    if (ehdr->e_machine != 0xb7 /* EM_AARCH64 */) {
        { char _eb[256]; int _en = snprintf(_eb,sizeof(_eb), "[elf_loader] not ARM64\n"); write(STDERR_FILENO,_eb,_en>0?(size_t)_en:0); }
        munmap(file, file_size); return NULL;
    }

    // Pass 1: find virtual address range
    const Elf64_Phdr *phdrs = (const Elf64_Phdr *)(file + ehdr->e_phoff);
    Elf64_Addr min_vaddr = UINT64_MAX, max_vaddr = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (phdrs[i].p_vaddr < min_vaddr) min_vaddr = phdrs[i].p_vaddr;
        Elf64_Addr end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (end > max_vaddr) max_vaddr = end;
    }

    // Align reservation
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    min_vaddr &= ~(page - 1);
    max_vaddr  = (max_vaddr + page - 1) & ~(page - 1);
    size_t load_size = (size_t)(max_vaddr - min_vaddr);

    // Reserve the whole range (PROT_NONE for now)
    void *load_base = mmap(NULL, load_size, PROT_NONE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
    if (load_base == MAP_FAILED) { elf_log("mmap reserve failed: %s\n", strerror(errno)); munmap(file, file_size); return NULL; }
    elf_log("reserved %zu bytes at %p, bias=%p\n", load_size, load_base, (void*)((uint8_t*)load_base - min_vaddr));

    uint8_t *bias = (uint8_t *)load_base - min_vaddr;

    // Pass 2: map each PT_LOAD segment
    for (int i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        Elf64_Addr seg_start = ph->p_vaddr & ~(page - 1);
        Elf64_Addr seg_end   = (ph->p_vaddr + ph->p_memsz + page - 1) & ~(page - 1);
        size_t     seg_size  = (size_t)(seg_end - seg_start);

        // File-backed portion: map from the ELF file so the kernel can
        // verify the pages came from a file (required for PROT_EXEC in sandbox
        // without allow-unsigned-executable-memory entitlement).
        off_t file_off = (off_t)(ph->p_offset & ~(page - 1));
        // How many bytes of the file fall within this segment's page range
        size_t file_end_in_seg = (size_t)(ph->p_offset + ph->p_filesz - (uint64_t)file_off);
        size_t file_pages = (file_end_in_seg + page - 1) & ~(page - 1);
        if (file_pages > seg_size) file_pages = seg_size;

        elf_log("seg %d: vaddr=0x%llx memsz=0x%llx filesz=0x%llx flags=0x%x file_pages=%zu seg_size=%zu\n",
                i, (unsigned long long)ph->p_vaddr, (unsigned long long)ph->p_memsz,
                (unsigned long long)ph->p_filesz, ph->p_flags, file_pages, seg_size);

        if (file_pages > 0 && (uint64_t)file_off < file_size) {
            void *seg_file = mmap(bias + seg_start, file_pages,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_FIXED,
                                  fd, file_off);
            if (seg_file == MAP_FAILED) {
                elf_log("mmap segment %d file portion failed: %s\n", i, strerror(errno));
                munmap(load_base, load_size);
                munmap(file, file_size);
                close(fd);
                return NULL;
            }
        }

        // BSS portion: if memsz > filesz, the excess pages need anonymous mapping
        if (seg_size > file_pages) {
            void *seg_bss = mmap(bias + seg_start + file_pages,
                                 seg_size - file_pages,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_FIXED | MAP_ANON, -1, 0);
            if (seg_bss == MAP_FAILED) {
                elf_log("mmap BSS failed: %s\n", strerror(errno));
            }
        }

        // Zero partial-page BSS within the file-mapped region
        if (ph->p_memsz > ph->p_filesz) {
            uint8_t *bss_start = bias + ph->p_vaddr + ph->p_filesz;
            uint8_t *page_end = bias + seg_start + file_pages;
            if (bss_start < page_end) {
                memset(bss_start, 0, (size_t)(page_end - bss_start));
            }
        }
    }

    // NOTE: don't munmap(file) yet — we still need ehdr/phdrs below.

    // Parse dynamic section
    ElfHandle *h = calloc(1, sizeof(ElfHandle));
    h->load_base = load_base;
    h->load_size = load_size;
    h->bias      = bias;

    Elf64_Addr rela_off = 0, jmprel_off = 0;
    size_t rela_sz = 0, jmprel_sz = 0;
    Elf64_Addr android_rela_off = 0;
    size_t android_rela_sz = 0;
    Elf64_Addr init_array_off = 0;
    size_t init_array_sz = 0;
    Elf64_Addr gnu_hash_off = 0;
    Elf64_Addr strtab_off = 0, symtab_off = 0, strsz = 0;
    Elf64_Addr init_off = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_DYNAMIC) continue;
        const Elf64_Dyn *dyn = (const Elf64_Dyn *)(bias + phdrs[i].p_vaddr);
        for (; dyn->d_tag != DT_NULL; dyn++) {
            switch (dyn->d_tag) {
            case DT_STRTAB:      strtab_off   = dyn->d_un.d_ptr; break;
            case DT_SYMTAB:      symtab_off   = dyn->d_un.d_ptr; break;
            case DT_STRSZ:       strsz        = dyn->d_un.d_val; break;
            case DT_RELA:        rela_off     = dyn->d_un.d_ptr; break;
            case DT_RELASZ:      rela_sz      = dyn->d_un.d_val; break;
            case DT_JMPREL:      jmprel_off   = dyn->d_un.d_ptr; break;
            case DT_PLTRELSZ:    jmprel_sz    = dyn->d_un.d_val; break;
            case DT_INIT:        init_off     = dyn->d_un.d_ptr; break;
            case DT_INIT_ARRAY:  init_array_off = dyn->d_un.d_ptr; break;
            case DT_INIT_ARRAYSZ:init_array_sz  = dyn->d_un.d_val; break;
            case DT_GNU_HASH:    gnu_hash_off = dyn->d_un.d_ptr; break;
            case DT_ANDROID_RELA:    android_rela_off = dyn->d_un.d_ptr; break;
            case DT_ANDROID_RELASZ:  android_rela_sz  = dyn->d_un.d_val; break;
            }
        }
        break;
    }

    h->strtab = (const char *)(bias + strtab_off);
    h->symtab = (Elf64_Sym_real *)(bias + symtab_off);
    // Estimate symtab count from strtab boundary
    h->symtab_count = (strsz > 0 && symtab_off < strtab_off)
        ? (strtab_off - symtab_off) / sizeof(Elf64_Sym_real) : 4096;

    if (rela_off)   { h->rela    = (Elf64_Rela *)(bias + rela_off);
                      h->rela_count = rela_sz / sizeof(Elf64_Rela); }
    if (jmprel_off) { h->jmprel  = (Elf64_Rela *)(bias + jmprel_off);
                      h->jmprel_count = jmprel_sz / sizeof(Elf64_Rela); }
    if (init_array_off) {
        h->init_array = (void(**)(void))(bias + init_array_off);
        h->init_array_count = init_array_sz / sizeof(void*);
    }
    if (gnu_hash_off) g_gnu_hash = (const GnuHashHdr *)(bias + gnu_hash_off);

    // Apply relocations (need write access, already set above)
    if (h->rela)    apply_rela(h, h->rela,    h->rela_count);
    if (h->jmprel)  apply_rela(h, h->jmprel,  h->jmprel_count);
    if (android_rela_off && android_rela_sz)
        decode_android_rela(h, bias + android_rela_off, android_rela_sz);

    // Patch TPIDR_EL0 accesses BEFORE setting segment permissions.
    // Pages are still rw from the initial load, so we can write freely.
    bionic_tls_setup();
    elf_patch_tpidr(bias, phdrs, ehdr->e_phnum, load_base, load_size);

    // Now set proper segment permissions (rx for code, rw for data).
    for (int i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;
        int prot = 0;
        if (ph->p_flags & 4) prot |= PROT_READ;
        if (ph->p_flags & 2) prot |= PROT_WRITE;
        if (ph->p_flags & 1) prot |= PROT_EXEC;
        Elf64_Addr seg_start = ph->p_vaddr & ~(page - 1);
        Elf64_Addr seg_end   = (ph->p_vaddr + ph->p_memsz + page - 1) & ~(page - 1);
        int mp_ret = mprotect(bias + seg_start, (size_t)(seg_end - seg_start), prot);
        elf_log("mprotect seg %d prot=0x%x: %s\n", i, prot,
                mp_ret == 0 ? "OK" : strerror(errno));
        if (mp_ret != 0 && (prot & PROT_EXEC)) {
            elf_log("FATAL: cannot make code executable\n");
            munmap(file, file_size); close(fd);
            munmap(load_base, load_size); free(h);
            return NULL;
        }
    }

    // Run DT_INIT
    if (init_off) {
        void (*init_fn)(void) = (void(*)(void))(bias + init_off);
        elf_log("calling DT_INIT at %p (offset=0x%llx)\n", (void*)init_fn, (unsigned long long)init_off);
        init_fn();
        elf_log("DT_INIT returned OK\n");
    }
    elf_log("Bionic TLS: TPIDR_EL0 set to fake TLS block\n");

    // Run constructors — use cached probe results when available.
    // Cache key: file size + mtime + constructor count.
    // Cache file: /tmp/gboard_ctor_cache_<size>_<mtime>_<count>
    if (h->init_array && h->init_array_count > 0) {
        size_t n = h->init_array_count;
        uint8_t *ctor_safe = mmap(NULL, n, PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_ANON, -1, 0);

        // Try loading from cache
        char cache_path[256];
        snprintf(cache_path, sizeof(cache_path),
                 "/tmp/gboard_ctor_cache_%zu_%lld_%zu",
                 file_size, (long long)st.st_mtime, n);

        int cached = 0;
        int cache_fd = open(cache_path, O_RDONLY);
        if (cache_fd >= 0) {
            ssize_t rd = read(cache_fd, ctor_safe, n);
            close(cache_fd);
            if ((size_t)rd == n) {
                cached = 1;
                elf_log("DT_INIT_ARRAY: loaded probe cache (%zu ctors)\n", n);
            }
        }

        if (!cached) {
            // Fork-probe: child tests each constructor with sigsetjmp
            memset(ctor_safe, 0, n);
            elf_log("DT_INIT_ARRAY: probing %zu constructors via fork...\n", n);

            pid_t pid = fork();
            if (pid == 0) {
                for (size_t i = 0; i < n; i++) {
                    void (*ctor)(void) = h->init_array[i];
                    if (!ctor || (uintptr_t)ctor < 0x10000) { ctor_safe[i] = 1; continue; }

                    int sig = sigsetjmp(s_ctor_jmp, 1);
                    if (sig != 0) {
                        struct itimerval zero = {{0,0},{0,0}};
                        setitimer(ITIMER_REAL, &zero, NULL);
                        ctor_safe[i] = (sig == SIGALRM) ? 3 : 2;
                        elf_log("  ctor[%zu] at %p: %s (sig=%d)\n", i, (void*)ctor,
                                sig == SIGALRM ? "HUNG" : "CRASHED", sig);
                        continue;
                    }

                    struct sigaction csa = {0};
                    csa.sa_handler = ctor_crash_handler;
                    sigemptyset(&csa.sa_mask);
                    struct sigaction old_segv, old_bus, old_ill, old_alrm, old_abrt;
                    sigaction(SIGSEGV, &csa, &old_segv);
                    sigaction(SIGBUS,  &csa, &old_bus);
                    sigaction(SIGILL,  &csa, &old_ill);
                    sigaction(SIGALRM, &csa, &old_alrm);
                    sigaction(SIGABRT, &csa, &old_abrt);

                    struct itimerval timer = {{0,0},{2,0}};
                    setitimer(ITIMER_REAL, &timer, NULL);
                    ctor();
                    struct itimerval zero = {{0,0},{0,0}};
                    setitimer(ITIMER_REAL, &zero, NULL);
                    ctor_safe[i] = 1;

                    sigaction(SIGSEGV, &old_segv, NULL);
                    sigaction(SIGBUS,  &old_bus,  NULL);
                    sigaction(SIGILL,  &old_ill,  NULL);
                    sigaction(SIGALRM, &old_alrm, NULL);
                    sigaction(SIGABRT, &old_abrt, NULL);
                }
                _exit(0);
            } else if (pid > 0) {
                int status;
                waitpid(pid, &status, 0);
                elf_log("DT_INIT_ARRAY probe: child exit=%d\n", WEXITSTATUS(status));

                // Save cache for next time
                int wfd = open(cache_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (wfd >= 0) {
                    write(wfd, ctor_safe, n);
                    close(wfd);
                    elf_log("DT_INIT_ARRAY: saved probe cache to %s\n", cache_path);
                }
            }
        }

        // Run only safe constructors
        int run_ok = 0, skipped = 0;
        for (size_t i = 0; i < n; i++) {
            if (ctor_safe[i] != 1) { skipped++; continue; }
            void (*ctor)(void) = h->init_array[i];
            if (!ctor || (uintptr_t)ctor < 0x10000) continue;
            ctor();
            run_ok++;
        }
        elf_log("DT_INIT_ARRAY: ran %d safe, skipped %d (cached=%d)\n",
                run_ok, skipped, cached);
        munmap(ctor_safe, n);
    }

    // Release file mapping and close fd (no longer needed after constructors)
    munmap(file, file_size);
    close(fd);

    elf_log("elf_load complete: %s @ %p\n", path, load_base);
    return h;
}

void *elf_bias(ElfHandle *h) { return h ? h->bias : NULL; }

void elf_unload(ElfHandle *h) {
    if (!h) return;
    // Run captured __cxa_atexit destructors while pages are still mapped
    android_stubs_run_atexit();
    munmap(h->load_base, h->load_size);
    free(h);
}
