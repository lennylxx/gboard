#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "engine/hmm_engine.h"

static int g_pass = 0, g_fail = 0;

static void check(const char *name, bool cond) {
    if (cond) { g_pass++; printf("  PASS: %s\n", name); }
    else      { g_fail++; printf("  FAIL: %s\n", name); }
}

// Helper: reset, append full pinyin string, get candidates
static int get_candidates_for(const char *pinyin, char **cands, int max) {
    hmm_engine_reset();
    for (size_t i = 0; i < strlen(pinyin); i++) {
        char ch[2] = { pinyin[i], '\0' };
        if (!hmm_engine_append(ch)) return -1;
    }
    return hmm_engine_get_candidates(cands, max);
}

// Helper: reset, append full string at once (like the IME does), get candidates
static int get_candidates_bulk(const char *pinyin, char **cands, int max) {
    hmm_engine_reset();
    if (!hmm_engine_append(pinyin)) return -1;
    return hmm_engine_get_candidates(cands, max);
}

static void free_cands(char **cands, int count) {
    for (int i = 0; i < count; i++) { free(cands[i]); cands[i] = NULL; }
}

// Check that expected string appears in candidate list
static bool has_candidate(char **cands, int count, const char *expected) {
    for (int i = 0; i < count; i++)
        if (cands[i] && strcmp(cands[i], expected) == 0) return true;
    return false;
}

// ── Tests ────────────────────────────────────────────────────────────────────

static void test_init(const char *so, const char *pack) {
    printf("[test_init]\n");
    bool ok = hmm_engine_init(so, pack);
    check("engine initializes", ok);
}

static void test_single_char(void) {
    printf("[test_single_char]\n");
    char *cands[9] = {0};
    int n = get_candidates_for("n", cands, 9);
    check("'n' returns candidates", n > 0);
    check("'n' first candidate is 你", n > 0 && strcmp(cands[0], "你") == 0);
    check("'n' has candidate 能", has_candidate(cands, n, "能"));
    free_cands(cands, n);
}

static void test_syllable(void) {
    printf("[test_syllable]\n");
    char *cands[9] = {0};
    int n = get_candidates_for("ni", cands, 9);
    check("'ni' returns candidates", n > 0);
    check("'ni' first candidate is 你", n > 0 && strcmp(cands[0], "你") == 0);
    check("'ni' has candidate 尼", has_candidate(cands, n, "尼"));
    // Verify these are 'ni' candidates, not just 'n' candidates
    check("'ni' has candidate 泥", has_candidate(cands, n, "泥"));
    free_cands(cands, n);
}

static void test_phrase_nihao(void) {
    printf("[test_phrase_nihao]\n");
    char *cands[9] = {0};
    int n = get_candidates_for("nihao", cands, 9);
    check("'nihao' returns candidates", n > 0);
    check("'nihao' first candidate is 你好", n > 0 && strcmp(cands[0], "你好") == 0);
    free_cands(cands, n);
}

static void test_phrase_zhongwen(void) {
    printf("[test_phrase_zhongwen]\n");
    char *cands[9] = {0};
    int n = get_candidates_for("zhongwen", cands, 9);
    check("'zhongwen' returns candidates", n > 0);
    check("'zhongwen' first candidate is 中文", n > 0 && strcmp(cands[0], "中文") == 0);
    free_cands(cands, n);
}

static void test_long_phrase(void) {
    printf("[test_long_phrase]\n");
    char *cands[9] = {0};
    int n = get_candidates_for("zhongwenshurufa", cands, 9);
    check("'zhongwenshurufa' returns candidates", n > 0);
    check("'zhongwenshurufa' first candidate is 中文输入法",
          n > 0 && strcmp(cands[0], "中文输入法") == 0);
    free_cands(cands, n);
}

static void test_bulk_append(void) {
    printf("[test_bulk_append]\n");
    // The IME calls hmm_engine_append with the full string at once.
    // Verify this produces the same results as per-char append.
    char *cands_bulk[9] = {0};
    int n_bulk = get_candidates_bulk("zhongwen", cands_bulk, 9);
    check("bulk 'zhongwen' returns candidates", n_bulk > 0);
    check("bulk 'zhongwen' first is 中文",
          n_bulk > 0 && strcmp(cands_bulk[0], "中文") == 0);
    free_cands(cands_bulk, n_bulk);

    char *cands_bulk2[9] = {0};
    int n2 = get_candidates_bulk("nihao", cands_bulk2, 9);
    check("bulk 'nihao' first is 你好",
          n2 > 0 && strcmp(cands_bulk2[0], "你好") == 0);
    free_cands(cands_bulk2, n2);
}

static void test_reset_between_inputs(void) {
    printf("[test_reset_between_inputs]\n");
    // Ensure reset properly clears state between different inputs
    char *cands1[9] = {0};
    int n1 = get_candidates_for("nihao", cands1, 9);
    check("first query 'nihao' works", n1 > 0 && strcmp(cands1[0], "你好") == 0);
    free_cands(cands1, n1);

    char *cands2[9] = {0};
    int n2 = get_candidates_for("zhongwen", cands2, 9);
    check("second query 'zhongwen' works after reset",
          n2 > 0 && strcmp(cands2[0], "中文") == 0);
    free_cands(cands2, n2);

    // Go back to the first input
    char *cands3[9] = {0};
    int n3 = get_candidates_for("nihao", cands3, 9);
    check("third query 'nihao' again works",
          n3 > 0 && strcmp(cands3[0], "你好") == 0);
    free_cands(cands3, n3);
}

static void test_incremental_append(void) {
    printf("[test_incremental_append]\n");
    // Simulate real IME behavior: each keystroke resets and re-appends full composition
    const char *keystrokes[] = {"z", "zh", "zho", "zhon", "zhong", "zhongw", "zhongwe", "zhongwen", NULL};
    for (int k = 0; keystrokes[k]; k++) {
        char *cands[9] = {0};
        int n = get_candidates_bulk(keystrokes[k], cands, 9);
        if (strcmp(keystrokes[k], "zhongwen") == 0) {
            check("incremental 'zhongwen' → 中文",
                  n > 0 && strcmp(cands[0], "中文") == 0);
        }
        free_cands(cands, n);
    }
    // Final check: after all incremental steps, a fresh query still works
    char *cands[9] = {0};
    int n = get_candidates_bulk("nihao", cands, 9);
    check("fresh query after incremental still works",
          n > 0 && strcmp(cands[0], "你好") == 0);
    free_cands(cands, n);
}

static void test_multiple_candidates(void) {
    printf("[test_multiple_candidates]\n");
    char *cands[9] = {0};
    int n = get_candidates_for("zhongwen", cands, 9);
    check("'zhongwen' has multiple candidates", n >= 3);
    // Candidate[0] should be the full phrase, others should be partial/alternative
    check("candidate[0] is full phrase 中文", n > 0 && strcmp(cands[0], "中文") == 0);
    check("candidate[1] exists and differs from [0]",
          n > 1 && strcmp(cands[1], cands[0]) != 0);
    free_cands(cands, n);
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    const char *so = "../source/resources/lib/arm64-v8a/libintegrated_shared_object.so";
    const char *pack = "../hmmoemdata/zh_cn_2025090307";
    if (argc > 2) { so = argv[1]; pack = argv[2]; }

    test_init(so, pack);
    if (g_fail > 0) { printf("\nEngine init failed — cannot continue.\n"); return 1; }

    test_single_char();
    test_syllable();
    test_phrase_nihao();
    test_phrase_zhongwen();
    test_long_phrase();
    test_bulk_append();
    test_reset_between_inputs();
    test_incremental_append();
    test_multiple_candidates();

    printf("\n══════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    printf("══════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
