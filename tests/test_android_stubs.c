// Tests for the AAPCS64 variadic ABI bridge in android_stubs.c.
//
// The Android .so calls variadic functions using the standard AArch64 PCS:
// variadic arguments go in x0-x7 / v0-v7 first, then in 8-byte stack slots,
// in argument order. On Apple arm64, calling a *non-variadic* function whose
// parameters are all 8-byte integers/pointers or doubles produces exactly the
// same register and stack layout, so the tests invoke the stubs through such
// prototypes to simulate calls from the .so. va_list-taking stubs receive a
// hand-built AAPCS64 va_list.

#include "engine/android_stubs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;

static void check_str(const char *name, const char *actual, const char *expected) {
    if (strcmp(actual, expected) == 0) {
        g_pass++;
        printf("  PASS: %s\n", name);
    } else {
        g_fail++;
        printf("  FAIL: %s\n    expected: \"%s\"\n    actual:   \"%s\"\n", name, expected, actual);
    }
}

static void check(const char *name, int cond) {
    if (cond) { g_pass++; printf("  PASS: %s\n", name); }
    else      { g_fail++; printf("  FAIL: %s\n", name); }
}

static void *stub(const char *name) {
    for (const SymEntry *e = android_stubs_table(); e->name; e++)
        if (strcmp(e->name, name) == 0) return e->addr;
    fprintf(stderr, "stub not found: %s\n", name);
    exit(1);
}

// Reads everything written to `fp` since it was opened.
static char *read_all(FILE *fp) {
    fflush(fp);
    long size = ftell(fp);
    char *buf = calloc(1, (size_t)size + 1);
    rewind(fp);
    fread(buf, 1, (size_t)size, fp);
    return buf;
}

// Captures what `fn` writes to STDERR_FILENO.
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

// Mirrors the AAPCS64 va_list layout.
typedef struct {
    void *stack;
    void *gr_top;
    void *vr_top;
    int   gr_offs;
    int   vr_offs;
} aapcs64_va_list;

typedef int (*vfprintf_fn)(FILE *, const char *, const aapcs64_va_list *);
typedef int (*log_vprint_fn)(int, const char *, const char *, const aapcs64_va_list *);

// ── fprintf: mixed integer / floating-point register arguments ───────────────

static void test_fprintf_registers(void) {
    printf("[fprintf_registers]\n");
    typedef int (*fn_t)(FILE *, const char *, long, double, const char *, double, long);
    fn_t f = (fn_t)stub("fprintf");
    FILE *fp = tmpfile();
    int ret = f(fp, "%d|%.2f|%s|%g|%ld", 42, 3.14159, "hi", 1e-5, -7L);
    char *out = read_all(fp);
    check_str("ints and doubles come from x and v registers", out, "42|3.14|hi|1e-05|-7");
    check("returns number of bytes written", ret == (int)strlen(out));
    free(out); fclose(fp);
}

// ── fprintf: arguments overflowing both register classes onto the stack ──────

static void test_fprintf_stack_overflow(void) {
    printf("[fprintf_stack_overflow]\n");
    // Named: x0 stream, x1 fmt. Variadic GPRs x2-x7 hold L1-L6, L7-L9 spill;
    // v0-v7 hold D1-D8, D9-D10 spill. Stack order: L7, L8, L9, D9, D10.
    typedef int (*fn_t)(FILE *, const char *,
                        long, double, long, double, long, double, long, double,
                        long, double, long, double, long, double, long, double,
                        long, double, double);
    fn_t f = (fn_t)stub("fprintf");
    FILE *fp = tmpfile();
    f(fp, "%ld %.1f %ld %.1f %ld %.1f %ld %.1f %ld %.1f %ld %.1f %ld %.1f "
          "%ld %.1f %ld %.1f %.1f",
      1L, 1.5, 2L, 2.5, 3L, 3.5, 4L, 4.5, 5L, 5.5, 6L, 6.5, 7L, 7.5,
      8L, 8.5, 9L, 9.5, 10.5);
    char *out = read_all(fp);
    check_str("arguments beyond x7 / v7 are read from the stack in order", out,
              "1 1.5 2 2.5 3 3.5 4 4.5 5 5.5 6 6.5 7 7.5 8 8.5 9 9.5 10.5");
    free(out); fclose(fp);
}

// ── fprintf: length modifiers, flags, special conversions ────────────────────

static void test_fprintf_conversions(void) {
    printf("[fprintf_conversions]\n");
    typedef int (*fn_t)(FILE *, const char *, long, long, long, long, long, long);
    fn_t f = (fn_t)stub("fprintf");

    FILE *fp = tmpfile();
    f(fp, "%hhd|%hu|%d|%u|%llx|%zu", 300L, 70000L, (long)0xFFFFFFFFL, -1L,
      (long)0x1234567890abcdefLL, 12345L);
    char *out = read_all(fp);
    check_str("integer length modifiers truncate and sign-extend", out,
              "44|4464|-1|4294967295|1234567890abcdef|12345");
    free(out); fclose(fp);

    fp = tmpfile();
    f(fp, "[%-6s][%6.2s][%c][%%][%s][%05d]", (long)"abc", (long)"xyz", (long)'Q',
      (long)NULL, 42L, 0L);
    out = read_all(fp);
    check_str("flags, width, precision, %c, %%, NULL %s", out,
              "[abc   ][    xy][Q][%][(null)][00042]");
    free(out); fclose(fp);

    fp = tmpfile();
    int marker = 0;
    f(fp, "%p", (long)&marker, 0, 0, 0, 0, 0);
    out = read_all(fp);
    char expected[64];
    snprintf(expected, sizeof(expected), "%p", (void *)&marker);
    check_str("%p prints the pointer", out, expected);
    free(out); fclose(fp);
}

// ── fprintf: output longer than the internal stack buffer ────────────────────

static void test_fprintf_long_output(void) {
    printf("[fprintf_long_output]\n");
    typedef int (*fn_t)(FILE *, const char *, const char *, double);
    fn_t f = (fn_t)stub("fprintf");
    char big[3001];
    memset(big, 'z', 3000); big[3000] = '\0';
    FILE *fp = tmpfile();
    int ret = f(fp, "<%s>%.3f", big, 2.0);
    char *out = read_all(fp);
    check("long output is not truncated", strlen(out) == 3000 + 2 + 5 && ret == 3007);
    check("long output content intact", out[0] == '<' && out[3001] == '>' &&
          strcmp(out + 3002, "2.000") == 0);
    free(out); fclose(fp);
}

// ── __android_log_print: * width / precision ─────────────────────────────────

static void log_star_width(void) {
    typedef int (*fn_t)(long, const char *, const char *, long, long, double, const char *);
    fn_t f = (fn_t)stub("__android_log_print");
    f(4, "Tag", "%*.*f|%-5s|", 9, 3, 3.14159265, "ab");
}

static void log_many_args(void) {
    typedef int (*fn_t)(long, const char *, const char *,
                        long, long, long, long, long, long, long,
                        double, double, double, double, double, double, double, double, double);
    fn_t f = (fn_t)stub("__android_log_print");
    f(4, "T", "%ld%ld%ld%ld%ld%ld%ld %g %g %g %g %g %g %g %g %g",
      1L, 2L, 3L, 4L, 5L, 6L, 7L, 0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5);
}

static void test_log_print(void) {
    printf("[android_log_print]\n");
    char *out = capture_stderr(log_star_width);
    check_str("* width and precision consume integer args", out, "[Tag]     3.142|ab   |\n");
    free(out);

    out = capture_stderr(log_many_args);
    check_str("log_print handles stack-spilled ints and doubles", out,
              "[T] 1234567 0.5 1.5 2.5 3.5 4.5 5.5 6.5 7.5 8.5\n");
    free(out);
}

// ── va_list stubs with a hand-built AAPCS64 va_list ──────────────────────────

static FILE *g_vfp;
static aapcs64_va_list g_va;

static void log_vprint_call(void) {
    ((log_vprint_fn)stub("__android_log_vprint"))(6, "V", "%d %s %.2f", &g_va);
}

static void test_va_list(void) {
    printf("[va_list]\n");
    // Two GPR slots and two FP slots remain in the save areas; the rest
    // comes from the stack, including a 16-byte aligned binary128 value.
    uint64_t gr[3] = { 0xdeadbeef /* already consumed */, 7, (uint64_t)"str" };
    uint8_t  vr[3][16] = {{0}};
    double d1 = 0.25, d2 = -1.75;
    memcpy(vr[1], &d1, 8);
    memcpy(vr[2], &d2, 8);
    _Alignas(16) uint64_t stack[4];
    double d3 = 9.5;
    stack[0] = (uint64_t)-123;              // long (GPRs exhausted)
    memcpy(&stack[1], &d3, 8);              // double (FPRs exhausted)
    stack[2] = 0;                            // binary128 1.5 (lo, hi) in the
    stack[3] = 0x3fff800000000000ULL;        // next 16-byte aligned slot

    aapcs64_va_list ap = {
        .stack = stack,
        .gr_top = gr + 3, .gr_offs = -16,
        .vr_top = vr + 3, .vr_offs = -32,
    };
    g_vfp = tmpfile();
    int ret = ((vfprintf_fn)stub("vfprintf"))(
        g_vfp, "%d|%.2f|%s|%.2f|%ld|%.1f|%Lg", &ap);
    char *out = read_all(g_vfp);
    check_str("vfprintf walks registers, then the stack", out,
              "7|0.25|str|-1.75|-123|9.5|1.5");
    check("vfprintf returns length", ret == (int)strlen(out));
    free(out); fclose(g_vfp);

    // The callee must not consume the caller's copy.
    check("va_list passed by reference is left intact",
          ap.gr_offs == -16 && ap.vr_offs == -32 && ap.stack == stack);

    uint64_t gr2[3] = { 5, (uint64_t)"ok", 0 };
    uint8_t  vr2[1][16] = {{0}};
    double d4 = 2.5;
    memcpy(vr2[0], &d4, 8);
    g_va = (aapcs64_va_list){ .stack = stack, .gr_top = gr2 + 3, .gr_offs = -24,
                              .vr_top = vr2 + 1, .vr_offs = -16 };
    out = capture_stderr(log_vprint_call);
    check_str("__android_log_vprint formats its va_list", out, "[V] 5 ok 2.50\n");
    free(out);
}

// ── %m and unsupported positional conversions ────────────────────────────────

static void test_misc(void) {
    printf("[misc]\n");
    typedef int (*fn_t)(FILE *, const char *, long);
    fn_t f = (fn_t)stub("fprintf");
    FILE *fp = tmpfile();
    errno = ENOENT;
    f(fp, "%m", 0);
    char *out = read_all(fp);
    check_str("%m prints strerror(errno)", out, strerror(ENOENT));
    free(out); fclose(fp);

    fp = tmpfile();
    f(fp, "a%1$db", 5);
    out = read_all(fp);
    check_str("positional conversions are emitted verbatim", out, "a%1$db");
    free(out); fclose(fp);
}

int main(void) {
    test_fprintf_registers();
    test_fprintf_stack_overflow();
    test_fprintf_conversions();
    test_fprintf_long_output();
    test_log_print();
    test_va_list();
    test_misc();
    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
