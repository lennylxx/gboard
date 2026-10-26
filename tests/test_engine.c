#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "engine/hmm_engine.h"
#include "engine/hmm_user_dict.h"
#include "engine/hmm_internal.h"

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
    hmm_engine_prepare_input(pinyin);
    hmm_engine_reset();
    if (!hmm_engine_append(pinyin)) return -1;
    return hmm_engine_get_candidates(cands, max);
}

static void free_cands(char **cands, int count) {
    for (int i = 0; i < count; i++) { free(cands[i]); cands[i] = NULL; }
}

static void set_editor_context(const char *text_before_cursor) {
    hmm_engine_update_input_context(text_before_cursor, "", "");
    hmm_engine_set_context(text_before_cursor);
}

// Check that expected string appears in candidate list
static bool has_candidate(char **cands, int count, const char *expected) {
    for (int i = 0; i < count; i++)
        if (cands[i] && strcmp(cands[i], expected) == 0) return true;
    return false;
}

// ── Tests ────────────────────────────────────────────────────────────────────

static void test_native_stub_a(void) {}
static void test_native_stub_b(void) {}

static void test_jni_native_lookup(void) {
    printf("[test_jni_native_lookup]\n");
    JNIEnv *env = jni_env_create();
    typedef struct { const char *name; const char *sig; void *fn; } TestNativeMethod;
    typedef jint (*RegisterNatives)(JNIEnv *, jclass, const void *, jint);
    RegisterNatives register_natives = (RegisterNatives)((void **)*env)[215];
    const TestNativeMethod methods[] = {
        {"testSingle", "(J)V", test_native_stub_a},
        {"testShared", "(J)V", test_native_stub_a},
        {"testShared", "(J)V", test_native_stub_a},
        {"testShared", "(I)V", test_native_stub_b},
        {"testSharedPrefix", "(J)V", test_native_stub_b},
        {"testConflict", "(J)V", test_native_stub_a},
        {"testConflict", "(J)V", test_native_stub_b},
        {"testInvalid", "(J)V", NULL},
    };
    check("test natives register", register_natives(env, NULL, methods, 8) == JNI_OK);
    check("single exact match resolves",
          jni_find_registered_native_unique("testSingle", "(J)V") == test_native_stub_a);
    check("same-address duplicates resolve",
          jni_find_registered_native_unique("testShared", "(J)V") == test_native_stub_a);
    check("overloads match their full signature",
          jni_find_registered_native_unique("testShared", "(I)V") == test_native_stub_b);
    check("signature substrings do not match",
          !jni_find_registered_native_unique("testSingle", "(J)"));
    check("name substrings do not match",
          !jni_find_registered_native_unique("testShare", "(J)V"));
    check("conflicting addresses are rejected",
          !jni_find_registered_native_unique("testConflict", "(J)V"));
    check("null native addresses are rejected",
          !jni_find_registered_native_unique("testInvalid", "(J)V"));
    check("missing native is rejected",
          !jni_find_registered_native_unique("testMissing", "(J)V"));
    jni_env_destroy(env);
    env = jni_env_create();
    check("new JNI environment clears stale registrations",
          !jni_find_registered_native_unique("testSingle", "(J)V"));
    jni_env_destroy(env);
}

static fn_EnrollSettingScheme s_real_enroll_setting;
static bool s_fail_enrollment;
static bool s_fail_accessor_enrollment;
static bool s_fail_decoder_rollback;
static fn_RefreshData s_real_refresh_data;
static int s_refresh_failures_remaining;
static int s_refresh_calls;

static void fail_settings_refresh(JNIEnv *env, jclass cls, jlong handle) {
    s_refresh_calls++;
    if (s_refresh_failures_remaining > 0) {
        s_refresh_failures_remaining--;
        raise(SIGSEGV);
        return;
    }
    s_real_refresh_data(env, cls, handle);
}

static jboolean fail_context_setting(JNIEnv *env, jclass cls, jlong manager,
                                       jstring type, jstring locale, jbyteArray bytes) {
    if (s_fail_enrollment && strstr(jni_get_string(type), "-context")) {
        s_fail_enrollment = false;
        return JNI_FALSE;
    }
    if (s_fail_accessor_enrollment &&
        strcmp(jni_get_string(type), "user_dictionary_accessor_for_ime") == 0) {
        s_fail_accessor_enrollment = false;
        if (s_fail_decoder_rollback) {
            s_fail_decoder_rollback = false;
            s_fail_enrollment = true;
        }
        return JNI_FALSE;
    }
    return s_real_enroll_setting(env, cls, manager, type, locale, bytes);
}

static void test_fuzzy_pinyin(void) {
    printf("[test_fuzzy_pinyin]\n");
    static const struct {
        uint32_t group;
        const char *pinyin;
        const char *expected;
    } cases[] = {
        {HMM_FUZZY_Z_ZH, "zidao", "知道"},
        {HMM_FUZZY_C_CH, "cifan", "吃饭"},
        {HMM_FUZZY_S_SH, "sijie", "世界"},
        {HMM_FUZZY_AN_ANG, "shanhai", "上海"},
        {HMM_FUZZY_EN_ENG, "penyou", "朋友"},
        {HMM_FUZZY_IN_ING, "beijin", "北京"},
        {HMM_FUZZY_L_N, "hulan", "湖南"},
        {HMM_FUZZY_F_H, "feise", "黑色"},
        {HMM_FUZZY_R_L, "lenmin", "人民"},
        {HMM_FUZZY_K_G, "kaoxin", "高兴"},
        {HMM_FUZZY_IAN_IANG, "xianfa", "想法"},
        {HMM_FUZZY_UAN_UANG, "huanjin", "黄金"},
    };
    set_editor_context("");
    check("fuzzy pinyin defaults to disabled", hmm_engine_get_fuzzy_pinyin() == 0);
    check("shared decoder refresh resolves without a class name",
          g_refreshData && g_refreshData == hmm_get_user_dict_natives()->refreshData &&
          g_refreshData == jni_find_registered_native_unique("nativeRefreshData", "(J)V"));
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *baseline[9] = {0};
        int baseline_count = get_candidates_bulk(cases[i].pinyin, baseline, 9);
        jlong original_engine = g_engine;
        check("enable one fuzzy group", hmm_engine_set_fuzzy_pinyin(cases[i].group));
        check("fuzzy setting does not rebuild the decoder", g_engine == original_engine);
        char *unchanged[9] = {0};
        int unchanged_count = hmm_engine_get_candidates(unchanged, 9);
        bool deferred = unchanged_count == baseline_count;
        for (int j = 0; j < unchanged_count && j < baseline_count; j++) {
            if (strcmp(unchanged[j], baseline[j]) != 0) deferred = false;
        }
        check("enrollment preserves candidates until the next reset", deferred);
        free_cands(unchanged, unchanged_count);
        check("active fuzzy mask matches", hmm_engine_get_fuzzy_pinyin() == cases[i].group);
        char *cands[50] = {0};
        int count = get_candidates_bulk(cases[i].pinyin, cands, 50);
        printf("    fuzzy %s ->", cases[i].pinyin);
        for (int j = 0; j < count && j < 10; j++) printf(" %s", cands[j]);
        printf("\n");
        check(cases[i].expected, has_candidate(cands, count, cases[i].expected));
        bool changed = count < baseline_count;
        for (int j = 0; j < baseline_count && j < count; j++) {
            if (strcmp(baseline[j], cands[j]) != 0) changed = true;
        }
        check("enabled group changes candidate ranking", changed);
        free_cands(cands, count);
        set_editor_context("我在");
        count = get_candidates_bulk(cases[i].pinyin, cands, 50);
        check("context decoder also uses fuzzy group",
              has_candidate(cands, count, cases[i].expected));
        free_cands(cands, count);
        set_editor_context("");
        check("disable individual fuzzy group", hmm_engine_set_fuzzy_pinyin(0));
        count = get_candidates_bulk(cases[i].pinyin, cands, 9);
        bool restored = count == baseline_count;
        for (int j = 0; j < baseline_count && j < count; j++) {
            if (strcmp(baseline[j], cands[j]) != 0) restored = false;
        }
        check("disabling restores original candidates", restored);
        free_cands(cands, count);
        free_cands(baseline, baseline_count);
    }
    check("all fuzzy groups can be combined", hmm_engine_set_fuzzy_pinyin(HMM_FUZZY_ALL));
    check("disable all fuzzy groups", hmm_engine_set_fuzzy_pinyin(0));

    char *cands[9] = {0};
    int count = get_candidates_bulk("nihao", cands, 9);
    free_cands(cands, count);
    jlong original = g_engine;
    check("unchanged settings preserve the decoder",
          hmm_engine_set_fuzzy_pinyin(0) && g_engine == original);
    check("invalid fuzzy mask is rejected",
          !hmm_engine_set_fuzzy_pinyin(1u << 12) && g_engine == original &&
          hmm_engine_get_fuzzy_pinyin() == 0);
    s_real_enroll_setting = g_enrollSettingScheme;
    s_fail_enrollment = true;
    g_enrollSettingScheme = fail_context_setting;
    bool switched = hmm_engine_set_fuzzy_pinyin(HMM_FUZZY_L_N);
    g_enrollSettingScheme = s_real_enroll_setting;
    check("failed enrollment keeps previous decoder and mask",
          !switched && g_engine == original && hmm_engine_get_fuzzy_pinyin() == 0);
    count = hmm_engine_get_candidates(cands, 9);
    check("failed enrollment preserves active composition",
          count > 0 && strcmp(cands[0], "你好") == 0);
    free_cands(cands, count);
    check("retry after enrollment failure succeeds",
          hmm_engine_set_fuzzy_pinyin(HMM_FUZZY_L_N));
    check("final fuzzy reset succeeds", hmm_engine_set_fuzzy_pinyin(0));
}

static void test_shuangpin(void) {
    printf("[test_shuangpin]\n");
    static const struct {
        HmmPinyinScheme scheme;
        const char *input;
        const char *reading;
    } cases[] = {
        {HMM_PINYIN_SHUANGPIN_ABC, "nihk", "ni'hk"},
        {HMM_PINYIN_SHUANGPIN_FLYPY, "nihc", "ni'hc"},
        {HMM_PINYIN_SHUANGPIN_JIAJIA, "nihd", "ni'hd"},
        {HMM_PINYIN_SHUANGPIN_MS, "nihk", "ni'hk"},
        {HMM_PINYIN_SHUANGPIN_ZIGUANG, "nihq", "ni'hq"},
        {HMM_PINYIN_SHUANGPIN_ZIRANMA, "nihk", "ni'hk"},
    };
    set_editor_context("");
    check("default scheme is full pinyin",
          hmm_engine_get_pinyin_scheme() == HMM_PINYIN_FULL);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        check("enable native shuangpin scheme",
              hmm_engine_set_pinyin_scheme(cases[i].scheme) &&
              hmm_engine_get_pinyin_scheme() == cases[i].scheme);
        char *cands[50] = {0};
        int count = get_candidates_bulk(cases[i].input, cands, 50);
        check("native shuangpin phrase is first without correction",
              count > 0 && strcmp(cands[0], "你好") == 0);
        HmmTokenReading corrected[32];
        check("scheme keys are not mistaken for spelling corrections",
              hmm_engine_get_corrected_candidate_reading(0, corrected, 32) == 0);
        char reading[256];
        hmm_engine_get_segmented_pinyin(reading, sizeof(reading));
        check("reading displays native shuangpin keys",
              strcmp(reading, cases[i].reading) == 0);
        check("full phrase consumes four original keys",
              hmm_engine_get_candidate_consumed(0) == 4);
        for (int j = 0; j < count; j++) {
            if (strcmp(cands[j], "你") == 0) {
                check("partial phrase consumes two original keys",
                      hmm_engine_get_candidate_consumed(j) == 2);
                break;
            }
        }
        char tokens[32][16];
        int types[32];
        int token_count = hmm_user_dict_extract_tokens(0, tokens, types, 32);
        const char *token_ptrs[32];
        for (int t = 0; t < token_count; t++) token_ptrs[t] = tokens[t];
        check("learn using the scheme-matched accessor",
              token_count == 2 &&
              hmm_user_dict_learn(token_ptrs, types, token_count, "你好", true));
        check("learned entries survive accessor changes", hmm_user_dict_get_size() > 0);
        free_cands(cands, count);
        set_editor_context("我说");
        count = get_candidates_bulk(cases[i].input, cands, 50);
        check("context decoder also uses the selected scheme",
              has_candidate(cands, count, "你好") &&
              hmm_engine_get_candidate_consumed(0) == 4);
        free_cands(cands, count);
        set_editor_context("");
        if (cases[i].scheme == HMM_PINYIN_SHUANGPIN_MS ||
            cases[i].scheme == HMM_PINYIN_SHUANGPIN_ZIGUANG) {
            count = get_candidates_bulk("b;", cands, 50);
            check("semicolon final is decoded natively",
                  has_candidate(cands, count, "冰"));
            free_cands(cands, count);
        }
    }
    check("enable flypy for edge cases",
          hmm_engine_set_pinyin_scheme(HMM_PINYIN_SHUANGPIN_FLYPY));
    char *cands[50] = {0};
    int count = get_candidates_bulk("aa", cands, 50);
    check("zero-initial syllable is native", has_candidate(cands, count, "啊"));
    free_cands(cands, count);
    count = get_candidates_bulk("n", cands, 50);
    check("half syllable retains candidates", has_candidate(cands, count, "你"));
    free_cands(cands, count);
    count = get_candidates_bulk("github", cands, 50);
    check("English mixed-input dictionary remains available",
          has_candidate(cands, count, "GitHub"));
    free_cands(cands, count);
    check("fuzzy setting can coexist with shuangpin",
          hmm_engine_set_fuzzy_pinyin(HMM_FUZZY_L_N));
    count = get_candidates_bulk("hulj", cands, 50);
    check("shuangpin uses enabled fuzzy expansions",
          has_candidate(cands, count, "湖南"));
    free_cands(cands, count);
    check("reset fuzzy pinyin after shuangpin test", hmm_engine_set_fuzzy_pinyin(0));

    count = get_candidates_bulk("nihc", cands, 50);
    free_cands(cands, count);
    jlong original = g_engine;
    check("invalid pinyin scheme is rejected",
          !hmm_engine_set_pinyin_scheme((HmmPinyinScheme)-1) &&
          !hmm_engine_set_pinyin_scheme((HmmPinyinScheme)7) &&
          hmm_engine_get_pinyin_scheme() == HMM_PINYIN_SHUANGPIN_FLYPY);
    s_real_enroll_setting = g_enrollSettingScheme;
    g_enrollSettingScheme = fail_context_setting;
    s_fail_enrollment = true;
    bool switched = hmm_engine_set_pinyin_scheme(HMM_PINYIN_SHUANGPIN_MS);
    check("failed decoder enrollment retains the old scheme",
          !switched && g_engine == original &&
          hmm_engine_get_pinyin_scheme() == HMM_PINYIN_SHUANGPIN_FLYPY);
    s_fail_accessor_enrollment = true;
    switched = hmm_engine_set_pinyin_scheme(HMM_PINYIN_SHUANGPIN_MS);
    g_enrollSettingScheme = s_real_enroll_setting;
    check("failed accessor enrollment retains the old scheme",
          !switched && hmm_engine_get_pinyin_scheme() == HMM_PINYIN_SHUANGPIN_FLYPY);
    count = hmm_engine_get_candidates(cands, 50);
    check("failed switch preserves active composition",
          count > 0 && strcmp(cands[0], "你好") == 0);
    free_cands(cands, count);
    count = get_candidates_bulk("nihc", cands, 50);
    check("rollback also survives the next decoder refresh",
          count > 0 && strcmp(cands[0], "你好") == 0);
    free_cands(cands, count);

    s_real_refresh_data = g_refreshData;
    g_refreshData = fail_settings_refresh;
    s_refresh_failures_remaining = 1;
    s_refresh_calls = 0;
    switched = hmm_engine_set_pinyin_scheme(HMM_PINYIN_SHUANGPIN_MS);
    check("accessor rollback refreshes the restored settings after a crash",
          !switched && s_refresh_calls == 2 &&
          hmm_user_dict_pinyin_settings_valid());
    g_refreshData = s_real_refresh_data;
    count = get_candidates_bulk("nihc", cands, 50);
    check("accessor refresh recovery preserves the previous scheme",
          count > 0 && strcmp(cands[0], "你好") == 0);
    free_cands(cands, count);

    g_refreshData = fail_settings_refresh;
    s_refresh_failures_remaining = 2;
    s_refresh_calls = 0;
    switched = hmm_engine_set_pinyin_scheme(HMM_PINYIN_SHUANGPIN_MS);
    check("failed accessor rollback is recorded explicitly",
          !switched && s_refresh_calls == 2 &&
          !hmm_user_dict_pinyin_settings_valid());
    g_refreshData = s_real_refresh_data;
    hmm_engine_prepare_input("nihc");
    hmm_engine_reset();
    check("failed accessor rollback blocks further input",
          !hmm_engine_append("nihc"));
    const char *blocked_tokens[] = {"ni", "hc"};
    int blocked_types[] = {16, 16};
    check("failed accessor rollback blocks learning with uncertain tokens",
          !hmm_user_dict_learn(blocked_tokens, blocked_types, 2, "你好", true));
    check("fuzzy settings cannot bypass failed accessor rollback",
          !hmm_engine_set_fuzzy_pinyin(0));
    check("retrying the current scheme repairs the accessor",
          hmm_engine_set_pinyin_scheme(HMM_PINYIN_SHUANGPIN_FLYPY) &&
          hmm_user_dict_pinyin_settings_valid());

    g_enrollSettingScheme = fail_context_setting;
    s_fail_accessor_enrollment = true;
    s_fail_decoder_rollback = true;
    switched = hmm_engine_set_pinyin_scheme(HMM_PINYIN_SHUANGPIN_MS);
    g_enrollSettingScheme = s_real_enroll_setting;
    check("failed decoder rollback retains the previous scheme selection",
          !switched &&
          hmm_engine_get_pinyin_scheme() == HMM_PINYIN_SHUANGPIN_FLYPY);
    g_refreshData = fail_settings_refresh;
    s_refresh_calls = 0;
    hmm_engine_prepare_input("nihc");
    hmm_engine_reset();
    check("failed rollback blocks personalized decoder refresh and input",
          s_refresh_calls == 0 && !hmm_engine_append("nihc"));
    set_editor_context("我说");
    hmm_engine_prepare_input("nihc");
    hmm_engine_reset();
    check("failed rollback also blocks context decoder refresh and input",
          s_refresh_calls == 0 && !hmm_engine_append("nihc"));
    check("dictionary snapshots cannot bypass inconsistent settings",
          !hmm_engine_refresh_user_dictionary() && s_refresh_calls == 0);
    g_refreshData = s_real_refresh_data;
    check("unchanged fuzzy selection reenrolls a complete configuration to recover",
          hmm_engine_set_fuzzy_pinyin(0));
    count = get_candidates_bulk("nihc", cands, 50);
    check("recovered context decoder uses the old scheme",
          count > 0 && strcmp(cands[0], "你好") == 0);
    free_cands(cands, count);
    set_editor_context("");
    count = get_candidates_bulk("nihc", cands, 50);
    check("recovered personalized decoder uses the old scheme",
          count > 0 && strcmp(cands[0], "你好") == 0);
    free_cands(cands, count);

    const char *learn_tokens[] = {"ce", "ui"};
    int learn_types[] = {16, 16};
    check("learn a distinctive shuangpin phrase",
          hmm_user_dict_learn(learn_tokens, learn_types, 2, "侧试", true));
    check("restore full pinyin", hmm_engine_set_pinyin_scheme(HMM_PINYIN_FULL));
    count = get_candidates_bulk("nihao", cands, 50);
    check("full pinyin works after switching back",
          count > 0 && strcmp(cands[0], "你好") == 0);
    free_cands(cands, count);
    char *learned[500] = {0};
    count = get_candidates_bulk("ceshi", learned, 500);
    check("shuangpin learning remains usable after switching to full pinyin",
          has_candidate(learned, count, "侧试"));
    free_cands(learned, count);
}

static void test_init(const char *so, const char *pack) {
    printf("[test_init]\n");
    bool ok = hmm_engine_init_with_user_data(so, pack, "tests/test_user_data");
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

static void test_long_sentence_candidate_regression(void) {
    printf("[test_long_sentence_candidate_regression]\n");
    char *cands[50] = {0};
    int n = get_candidates_bulk("jintiantianqihenhao", cands, 50);

    check("long sentence returns a full candidate list", n >= 24);
    check("long sentence first candidate is 今天天气很好",
          n > 0 && strcmp(cands[0], "今天天气很好") == 0);
    check("long sentence second candidate is 今天天气",
          n > 1 && strcmp(cands[1], "今天天气") == 0);
    check("long sentence includes partial candidate 今天",
          has_candidate(cands, n, "今天"));
    check("long sentence first candidate consumes all input",
          n > 0 && hmm_engine_get_candidate_consumed(0) == 19);

    free_cands(cands, n);
}

static void test_neural_activation_boundary(void) {
    printf("[test_neural_activation_boundary]\n");
    const char *input = "zhongwen";
    bool appended = true;

    hmm_engine_reset();
    for (size_t i = 0; input[i]; i++) {
        char ch[2] = {input[i], '\0'};
        if (!hmm_engine_append(ch)) {
            appended = false;
            break;
        }
        if (i == 6) {
            char *boundary_cands[9] = {0};
            int boundary_count =
                hmm_engine_get_candidates(boundary_cands, 9);
            check("7th character neural boundary returns candidates",
                  boundary_count > 0);
            free_cands(boundary_cands, boundary_count);
        }
    }

    check("all characters append through neural activation", appended);
    char *cands[9] = {0};
    int count = appended ? hmm_engine_get_candidates(cands, 9) : 0;
    check("neural boundary composition finishes as 中文",
          count > 0 && strcmp(cands[0], "中文") == 0);
    free_cands(cands, count);
}

static void test_neural_bulk_incremental_parity(void) {
    printf("[test_neural_bulk_incremental_parity]\n");
    char *incremental[20] = {0};
    char *bulk[20] = {0};
    int incremental_count =
        get_candidates_for("jintiantianqihenhao", incremental, 20);
    int bulk_count =
        get_candidates_bulk("jintiantianqihenhao", bulk, 20);

    check("neural incremental and bulk paths both return candidates",
          incremental_count > 0 && bulk_count > 0);
    check("neural incremental and bulk paths agree on first candidate",
          incremental_count > 0 && bulk_count > 0 &&
          strcmp(incremental[0], bulk[0]) == 0 &&
          strcmp(bulk[0], "今天天气很好") == 0);

    free_cands(incremental, incremental_count);
    free_cands(bulk, bulk_count);
}

static void test_neural_reranker_reset_stress(void) {
    printf("[test_neural_reranker_reset_stress]\n");
    const struct {
        const char *input;
        const char *expected;
    } cases[] = {
        {"zhongwen", "中文"},
        {"zhongwenshurufa", "中文输入法"},
        {"jintiantianqihenhao", "今天天气很好"}
    };
    bool stable = true;

    for (int round = 0; round < 25 && stable; round++) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            char *cands[9] = {0};
            int count = get_candidates_bulk(cases[i].input, cands, 9);
            if (count <= 0 || strcmp(cands[0], cases[i].expected) != 0) {
                stable = false;
            }
            free_cands(cands, count);
            if (!stable) break;
        }
    }

    check("neural reranker survives 25 reset/reuse rounds", stable);
}

static void test_context_language_scoring(void) {
    printf("[test_context_language_scoring]\n");
    char *cands[20] = {0};

    set_editor_context("");
    int baseline_count = get_candidates_bulk("bushu", cands, 20);
    check("context baseline prefers 部署",
          baseline_count > 0 && strcmp(cands[0], "部署") == 0);
    free_cands(cands, baseline_count);

    set_editor_context("我对这里很");
    int contextual_count = get_candidates_bulk("bushu", cands, 20);
    check("Chinese context reranks bushu to 不熟",
          contextual_count > 0 && strcmp(cands[0], "不熟") == 0);
    check("context candidate range excludes committed prefix",
          contextual_count > 0 &&
          hmm_engine_get_candidate_consumed(0) == 5);
    free_cands(cands, contextual_count);

    set_editor_context("无关前缀我对这里很");
    int truncated_count = get_candidates_bulk("bushu", cands, 20);
    check("Chinese context keeps the trailing five characters",
          truncated_count > 0 && strcmp(cands[0], "不熟") == 0);
    free_cands(cands, truncated_count);

    set_editor_context("我对这里很，");
    int boundary_count = get_candidates_bulk("bushu", cands, 20);
    check("punctuation ends the Java-compatible context window",
          boundary_count > 0 && strcmp(cands[0], "部署") == 0);
    free_cands(cands, boundary_count);

    set_editor_context("");
    hmm_engine_reset();
}

static void test_continuous_phrase_language_scoring(void) {
    printf("[test_continuous_phrase_language_scoring]\n");
    char *cands[20] = {0};

    set_editor_context("");
    int count = get_candidates_bulk("woduizhelihenbushu", cands, 20);
    check("continuous phrase prefers 我对这里很不熟",
          count > 0 && strcmp(cands[0], "我对这里很不熟") == 0);
    if (count > 0 && strcmp(cands[0], "我对这里很不熟") != 0) {
        printf("    actual first candidate: %s\n", cands[0]);
    }
    free_cands(cands, count);
}

static void test_select_and_continue(void) {
    printf("[test_select_and_continue]\n");

    // After selecting a candidate, the remaining pinyin should produce valid candidates.
    // "nihao" → select "你" → feed "hao" → should get "好"
    char *c[9] = {0};
    int n = get_candidates_bulk("nihao", c, 9);
    check("'nihao' gives candidates", n > 0);
    free_cands(c, n);

    // Feed remaining "hao" → should get candidates
    char *c2[9] = {0};
    int n2 = get_candidates_bulk("hao", c2, 9);
    check("remaining 'hao' gives candidates", n2 > 0);
    check("remaining 'hao' → '好'", n2 > 0 && c2[0] && strcmp(c2[0], "好") == 0);
    free_cands(c2, n2);

    // "zhongwenshuru" → after consuming "zhongwen" (8 chars), "shuru" remains
    char *c3[9] = {0};
    int n3 = get_candidates_bulk("shuru", c3, 9);
    check("remaining 'shuru' gives candidates", n3 > 0);
    free_cands(c3, n3);
}

// ── Candidate range tests ────────────────────────────────────────────────────

static void test_candidate_range(void) {
    printf("[test_candidate_range]\n");

    // "nihao" → candidate[0] = "你好" should consume 5 vertices (all chars)
    char *c[9] = {0};
    int n = get_candidates_bulk("nihao", c, 9);
    check("range: 'nihao' has candidates", n > 0);
    if (n > 0) {
        int consumed = hmm_engine_get_candidate_consumed(0);
        printf("    range[0] '%s' consumed=%d (total=5)\n", c[0], consumed);
        check("range: 'nihao' cand[0] consumed == 5", consumed == 5);
    }
    free_cands(c, n);

    // "zhongwenshurufa" → candidate[0] = "中文输入法" should consume 15
    char *c2[9] = {0};
    int n2 = get_candidates_bulk("zhongwenshurufa", c2, 9);
    check("range: 'zhongwenshurufa' has candidates", n2 > 0);
    if (n2 > 0) {
        int consumed = hmm_engine_get_candidate_consumed(0);
        printf("    range[0] '%s' consumed=%d (total=15)\n", c2[0], consumed);
        check("range: 'zhongwenshurufa' cand[0] consumed == 15", consumed == 15);
    }
    free_cands(c2, n2);

    // "woqunijiaya" → if cand[0] is partial, consumed < total
    char *c3[9] = {0};
    int n3 = get_candidates_bulk("woqunijiaya", c3, 9);
    check("range: 'woqunijiaya' has candidates", n3 > 0);
    if (n3 > 0) {
        int consumed = hmm_engine_get_candidate_consumed(0);
        printf("    range[0] '%s' consumed=%d (total=11)\n", c3[0], consumed);
        check("range: consumed > 0", consumed > 0);
        check("range: consumed <= 11", consumed <= 11);
    }
    free_cands(c3, n3);
}

// ── Full IME simulation: type → select → continue with remaining ─────────────

// Simulate the full IME flow: type pinyin, get candidates, select cand[0],
// compute remaining from range, re-feed remaining, repeat until done.
// Returns the concatenated committed Chinese text.
static int simulate_ime_flow(const char *pinyin, char *out, int out_size) {
    char composition[256];
    strncpy(composition, pinyin, sizeof(composition) - 1);
    composition[sizeof(composition) - 1] = '\0';
    out[0] = '\0';
    int rounds = 0;

    while (composition[0] != '\0' && rounds < 20) {
        rounds++;
        hmm_engine_reset();
        if (!hmm_engine_append(composition)) {
            printf("    round %d: append '%s' failed\n", rounds, composition);
            break;
        }
        char *cands[9] = {0};
        int n = hmm_engine_get_candidates(cands, 9);
        if (n <= 0) {
            printf("    round %d: no candidates for '%s'\n", rounds, composition);
            break;
        }

        int consumed = hmm_engine_get_candidate_consumed(0);
        printf("    round %d: '%s' → cand[0]='%s' consumed=%d/%zu\n",
               rounds, composition, cands[0], consumed, strlen(composition));

        // Commit candidate text
        strncat(out, cands[0], out_size - strlen(out) - 1);
        hmm_engine_select(0);

        // Advance composition
        if (consumed > 0 && consumed < (int)strlen(composition)) {
            memmove(composition, composition + consumed, strlen(composition) - consumed + 1);
        } else {
            composition[0] = '\0';
        }

        free_cands(cands, n);
    }
    return rounds;
}

static void test_full_ime_simulation(void) {
    printf("[test_full_ime_simulation]\n");

    // Test 1: "nihao" → should produce "你好"
    {
        char out[256];
        simulate_ime_flow("nihao", out, sizeof(out));
        printf("    result: '%s'\n", out);
        check("sim 'nihao' → contains 你好", strstr(out, "你好") != NULL);
    }

    // Test 2: "zhongwenshurufa" → should produce "中文输入法"
    {
        char out[256];
        simulate_ime_flow("zhongwenshurufa", out, sizeof(out));
        printf("    result: '%s'\n", out);
        check("sim 'zhongwenshurufa' → contains 中文", strstr(out, "中文") != NULL);
        check("sim 'zhongwenshurufa' → contains 输入法", strstr(out, "输入法") != NULL);
    }

    // Test 3: "woshizhongguoren" → should produce something with 中国人
    {
        char out[256];
        simulate_ime_flow("woshizhongguoren", out, sizeof(out));
        printf("    result: '%s'\n", out);
        check("sim 'woshizhongguoren' → non-empty", out[0] != '\0');
    }

    // Test 4: "jintiandtianqihenhaowomenyiqichuquwan"
    {
        char out[256];
        simulate_ime_flow("jintiantianqihenhaowomenyiqichuquwan", out, sizeof(out));
        printf("    result: '%s'\n", out);
        check("sim long sentence → non-empty", out[0] != '\0');
    }
}

// ── Brute force: every valid initial → must get candidates + valid range ─────

static void test_brute_force_initials(void) {
    printf("[test_brute_force_initials]\n");
    // Every possible pinyin initial + common vowels
    const char *inputs[] = {
        "a","o","e","ai","an","ba","bo","bi","bu","ca","ce","ci","cu",
        "da","de","di","du","fa","fo","fu","ga","ge","gu","ha","he","hu",
        "ji","ju","ka","ke","ku","la","le","li","lu","lv","ma","me","mi","mu",
        "na","ne","ni","nu","nv","pa","po","pi","pu","qi","qu","re","ri","ru",
        "sa","se","si","su","sha","she","shi","shu","ta","te","ti","tu",
        "wa","wo","wu","xi","xu","ya","ye","yi","yu","za","ze","zi","zu",
        "zha","zhe","zhi","zhu","cha","che","chi","chu",
        NULL
    };

    int total = 0, ok = 0, range_ok = 0;
    for (int i = 0; inputs[i]; i++) {
        total++;
        char *cands[9] = {0};
        int n = get_candidates_bulk(inputs[i], cands, 9);
        if (n > 0) {
            ok++;
            int consumed = hmm_engine_get_candidate_consumed(0);
            if (consumed > 0 && consumed <= (int)strlen(inputs[i])) {
                range_ok++;
            } else {
                printf("    WARN: '%s' cand[0]='%s' consumed=%d (len=%zu)\n",
                       inputs[i], cands[0], consumed, strlen(inputs[i]));
            }
        } else {
            printf("    WARN: '%s' returned 0 candidates\n", inputs[i]);
        }
        free_cands(cands, n);
    }
    printf("    %d/%d inputs returned candidates, %d/%d had valid ranges\n",
           ok, total, range_ok, total);
    check("brute force: all initials return candidates", ok == total);
    check("brute force: all ranges valid", range_ok == total);
}

// ── Brute force: common phrases → simulate full select flow ──────────────────

static void test_brute_force_phrases(void) {
    printf("[test_brute_force_phrases]\n");

    const char *phrases[] = {
        "nihao", "xiexie", "zaijian", "duibuqi", "meiguanxi",
        "zhongguo", "meiguo", "beijing", "shanghai", "guangzhou",
        "dianhua", "diannao", "shouji", "yinyue", "dianying",
        "xuesheng", "laoshi", "tongxue", "pengyou", "jiaren",
        "chifan", "shuijiao", "shangban", "xiaban", "huijia",
        "zuotian", "jintian", "mingtian", "xianzai", "yihou",
        "feichang", "feichanghao", "taihaole", "meiwenti",
        "qingwen", "xingming", "dizhi", "dianhuahaoma",
        "womenshipengyou", "zhongwenshurufa", "rengongzhineng",
        "jiqixuexi", "shenduxuexi", "ziranyuyan",
        "woxihuanzhongguo", "jintiantianqihenhao",
        NULL
    };

    int total = 0, completed = 0;
    for (int i = 0; phrases[i]; i++) {
        total++;
        char out[512];
        int rounds = simulate_ime_flow(phrases[i], out, sizeof(out));
        if (out[0] != '\0') {
            completed++;
            printf("    OK: '%s' → '%s' (%d rounds)\n", phrases[i], out, rounds);
        } else {
            printf("    FAIL: '%s' → empty output\n", phrases[i]);
        }
    }
    printf("    %d/%d phrases completed\n", completed, total);
    check("brute force: all phrases produce output", completed == total);
}

// ── Brute force: random a-z strings → must not crash ─────────────────────────

static void test_brute_force_random(void) {
    printf("[test_brute_force_random]\n");

    // Deterministic pseudo-random: test engine doesn't crash on garbage input
    unsigned seed = 12345;
    int total = 200, crashed = 0;
    for (int i = 0; i < total; i++) {
        unsigned len = 1 + (seed % 15);
        char buf[16];
        for (unsigned j = 0; j < len; j++) {
            seed = seed * 1103515245 + 12345;
            buf[j] = 'a' + (seed >> 16) % 26;
        }
        buf[len] = '\0';

        char *cands[9] = {0};
        int n = get_candidates_bulk(buf, cands, 9);
        if (n > 0) {
            int consumed = hmm_engine_get_candidate_consumed(0);
            (void)consumed; // just checking it doesn't crash
        }
        if (n >= 0) free_cands(cands, n);
        else crashed++;
    }
    printf("    tested %d random strings, %d failures\n", total, crashed);
    check("brute force: no crashes on random input", crashed == 0);
}

// ── Stress: incremental keystroke simulation ─────────────────────────────────

static void test_brute_force_incremental(void) {
    printf("[test_brute_force_incremental]\n");

    // Simulate typing each phrase one key at a time (like real user)
    const char *phrases[] = {
        "nihao", "zhongwen", "shurufa", "woshizhongguoren",
        "jintiantianqihenhao", "feichang", "pengyou", NULL
    };

    int ok = 0, total = 0;
    for (int p = 0; phrases[p]; p++) {
        total++;
        bool phrase_ok = true;
        int len = (int)strlen(phrases[p]);

        // Simulate each keystroke: reset + re-append full composition (like the IME)
        for (int k = 1; k <= len; k++) {
            hmm_engine_reset();
            char partial[64];
            strncpy(partial, phrases[p], k);
            partial[k] = '\0';

            if (!hmm_engine_append(partial)) {
                printf("    FAIL: '%s' append failed at len=%d\n", phrases[p], k);
                phrase_ok = false;
                break;
            }
            char *cands[9] = {0};
            int n = hmm_engine_get_candidates(cands, 9);
            if (n <= 0) {
                printf("    WARN: '%s' no candidates at len=%d\n", phrases[p], k);
            }
            free_cands(cands, n > 0 ? n : 0);
        }

        // After full input, should have candidates
        char *cands[9] = {0};
        int n = get_candidates_bulk(phrases[p], cands, 9);
        if (n <= 0) {
            printf("    FAIL: '%s' no final candidates\n", phrases[p]);
            phrase_ok = false;
        }
        free_cands(cands, n > 0 ? n : 0);

        if (phrase_ok) ok++;
    }
    printf("    %d/%d phrases OK through incremental keystroke\n", ok, total);
    check("brute force incremental: all phrases work", ok == total);
}

// ── Visual pinyin segmentation ──────────────────────────────────────────────

static void test_segmented_pinyin(void) {
    printf("[test_segmented_pinyin]\n");

    struct { const char *input; const char *expected; } cases[] = {
        { "nihao",              "ni'hao" },
        { "zhongwenshurufa",    "zhong'wen'shu'ru'fa" },
        { "woquni",             "wo'qu'ni" },
        { "beijing",            "bei'jing" },
        { "woaini",             "wo'ai'ni" },
        { "xian",               "xian" },
        { "fangan",             "fang'an" },
        { "jintiantianqihenhao","jin'tian'tian'qi'hen'hao" },
        { "woshizhongguoren",   "wo'shi'zhong'guo'ren" },
        { "github",             "github" },
        { "woyaogithub",        "wo'yao'github" },
        { "yongpythonxie",      "yong'python'xie" },
    };
    int total = sizeof(cases) / sizeof(cases[0]);
    int ok = 0;

    for (int c = 0; c < total; c++) {
        const char *pinyin = cases[c].input;
        char *cands[9] = {0};
        get_candidates_for(pinyin, cands, 9);

        char buf[256] = {0};
        hmm_engine_get_segmented_pinyin(buf, sizeof(buf));

        bool match = strcmp(buf, cases[c].expected) == 0;
        if (match) ok++;
        else printf("    FAIL: '%s' → '%s' (expected '%s')\n", pinyin, buf, cases[c].expected);

        free_cands(cands, 9);
    }
    printf("    %d/%d segmented readings correct\n", ok, total);
    check("segmented pinyin matches engine tokens", ok == total);
}

static void test_english_mixed_input(void) {
    printf("[test_english_mixed_input]\n");
    static const struct { const char *input; const char *expected; } cases[] = {
        { "github",        "GitHub" },
        { "woyaogithub",   "我要GitHub" },
        { "yongpythonxie", "用Python写" },
        { "zhongwen",      "中文" },
        { "women",         "我们" },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        char *cands[9] = {0};
        int n = get_candidates_bulk(cases[c].input, cands, 9);
        char label[128];
        snprintf(label, sizeof(label), "'%s' first candidate is %s",
                 cases[c].input, cases[c].expected);
        check(label, n > 0 && strcmp(cands[0], cases[c].expected) == 0);
        if (n > 0 && strcmp(cands[0], cases[c].expected) != 0)
            printf("    got '%s'\n", cands[0]);
        free_cands(cands, n);
    }

    char *cands[9] = {0};
    int n = get_candidates_bulk("github", cands, 9);
    char tokens[16][16];
    int types[16];
    int tc = n > 0 ? hmm_user_dict_extract_tokens(0, tokens, types, 16) : 0;
    bool all_english = tc == 6;
    for (int i = 0; i < tc; i++) all_english = all_english && types[i] == 0;
    check("'GitHub' tokens are six English letters", all_english);
    if (n > 0) {
        check("token extraction refuses truncated token lists",
              hmm_user_dict_extract_tokens(0, tokens, types, 3) == 0);
    }
    free_cands(cands, n);

    // Context-bearing input switches to the context engine, which must keep
    // the same English setting.
    set_editor_context("我觉得");
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        char *ctx_cands[9] = {0};
        int ctx_n = get_candidates_bulk(cases[c].input, ctx_cands, 9);
        char label[160];
        snprintf(label, sizeof(label),
                 "with context, '%s' first candidate is %s",
                 cases[c].input, cases[c].expected);
        check(label, ctx_n > 0 &&
                     strcmp(ctx_cands[0], cases[c].expected) == 0);
        if (ctx_n > 0 && strcmp(ctx_cands[0], cases[c].expected) != 0)
            printf("    got '%s'\n", ctx_cands[0]);
        free_cands(ctx_cands, ctx_n);
    }
    set_editor_context("");
}

// ── User dictionary tests ────────────────────────────────────────────────────

static void test_user_dict_init(void) {
    printf("[test_user_dict_init]\n");
    check("user dict is ready after init", hmm_user_dict_is_ready());
    int size = hmm_user_dict_get_size();
    printf("    initial user dict size: %d\n", size);
    check("user dict initial size >= 0", size >= 0);
}

static void test_user_dict_token_extraction(void) {
    printf("[test_user_dict_token_extraction]\n");

    char *cands[9] = {0};
    int n = get_candidates_bulk("nihao", cands, 9);
    check("'nihao' has candidates for token extraction", n > 0);

    if (n > 0) {
        char tokens[16][16];
        int types[16];
        int tc = hmm_user_dict_extract_tokens(0, tokens, types, 16);
        printf("    candidate[0] '%s' has %d tokens:\n", cands[0], tc);
        for (int i = 0; i < tc; i++) {
            printf("      token[%d] = '%s' type=%d\n", i, tokens[i], types[i]);
        }
        check("extracted at least 1 token", tc >= 1);
        // For 你好, tokens should be "ni" and "hao" (or similar)
        if (tc >= 2 && strcmp(cands[0], "你好") == 0) {
            check("token[0] is 'ni'", strcmp(tokens[0], "ni") == 0);
            check("token[1] is 'hao'", strcmp(tokens[1], "hao") == 0);
        }
        // Learning routes language-0 selections to the English dictionary.
        bool pinyin_language = tc > 0;
        for (int i = 0; i < tc; i++)
            pinyin_language = pinyin_language && types[i] != 0;
        check("Pinyin tokens are not English language", pinyin_language);
    }
    free_cands(cands, n);
}

static void test_context_overrides_user_frequency(void) {
    printf("[test_context_overrides_user_frequency]\n");
    char *cands[20] = {0};

    set_editor_context("");
    int count = get_candidates_bulk("bushu", cands, 20);
    check("frequency test starts with 部署",
          count > 0 && strcmp(cands[0], "部署") == 0);

    char tokens[8][16] = {{0}};
    int types[8] = {0};
    int token_count = count > 0
        ? hmm_user_dict_extract_tokens(0, tokens, types, 8)
        : 0;
    free_cands(cands, count);
    check("部署 exposes learning tokens", token_count > 0);

    const char *token_ptrs[8] = {0};
    for (int i = 0; i < token_count; i++) token_ptrs[i] = tokens[i];
    for (int i = 0; i < 50 && token_count > 0; i++) {
        hmm_user_dict_learn(token_ptrs, types, token_count,
                            "部署", true);
    }

    set_editor_context("");
    count = get_candidates_bulk("bushu", cands, 20);
    check("standalone bushu respects learned frequency",
          count > 0 && strcmp(cands[0], "部署") == 0);
    free_cands(cands, count);

    set_editor_context("我对这里很");
    count = get_candidates_bulk("bushu", cands, 20);
    check("context overrides learned deployment frequency",
          count > 0 && strcmp(cands[0], "不熟") == 0);
    free_cands(cands, count);

    set_editor_context("");
    count = get_candidates_bulk("woduizhelihenbushu", cands, 20);
    check("continuous context overrides learned deployment frequency",
          count > 0 && strcmp(cands[0], "我对这里很不熟") == 0);
    free_cands(cands, count);

    bool unlearned = true;
    for (int i = 0; i < 50 && token_count > 0; i++) {
        unlearned =
            hmm_user_dict_unlearn(token_ptrs, types, token_count,
                                  "部署") &&
            unlearned;
    }
    check("frequency test removes learned deployment counts", unlearned);
}

static void test_user_dict_learn_and_boost(void) {
    printf("[test_user_dict_learn_and_boost]\n");

    const char *input = "ceshi";
    char *cands_before[20] = {0};
    int n_before = get_candidates_bulk(input, cands_before, 20);
    check("'ceshi' has candidates", n_before > 1);

    int pos_before = -1;
    for (int i = n_before - 1; i >= 2; i--) {
        if (cands_before[i] &&
            hmm_engine_get_candidate_consumed(i) == (int)strlen(input) &&
            strlen(cands_before[i]) >= 6) {
            pos_before = i;
            break;
        }
    }
    check("found a lower-ranked full phrase", pos_before > 0);
    if (pos_before <= 0) {
        free_cands(cands_before, n_before);
        return;
    }

    char target[128];
    snprintf(target, sizeof(target), "%s", cands_before[pos_before]);
    char token_storage[16][16];
    int types[16];
    int token_count = hmm_user_dict_extract_tokens(
        pos_before, token_storage, types, 16);
    const char *tokens[16];
    for (int i = 0; i < token_count; ++i) tokens[i] = token_storage[i];
    check("selected phrase exposes learning tokens", token_count > 0);

    printf("    'ceshi' candidates before learning (top 5): ");
    for (int i = 0; i < 5 && i < n_before; i++) printf("'%s' ", cands_before[i]);
    printf("\n    target '%s' position before: %d\n", target, pos_before);
    free_cands(cands_before, n_before);

    check("decoder selects the target candidate",
          hmm_engine_select(pos_before));
    bool any_ok = false;
    for (int rep = 0; rep < 20; rep++) {
        bool ok = hmm_user_dict_learn(tokens, types, token_count, target, true);
        if (ok) any_ok = true;
    }
    check("learn succeeds (at least once)", any_ok);

    char *cands_after[20] = {0};
    int n_after = get_candidates_bulk(input, cands_after, 20);
    int pos_after = -1;
    for (int i = 0; i < n_after; i++) {
        if (cands_after[i] && strcmp(cands_after[i], target) == 0) {
            pos_after = i;
            break;
        }
    }
    printf("    target '%s' position after: %d\n", target, pos_after);
    check("repeated selection raises candidate ranking",
          pos_after >= 0 && pos_after < pos_before);
    free_cands(cands_after, n_after);
}

static void test_user_dict_learn_phrase(void) {
    printf("[test_user_dict_learn_phrase]\n");

    // Learn a multi-character phrase
    const char *tokens[] = {"ni", "hao"};
    int types[] = {16, 16};
    for (int i = 0; i < 3; i++) {
        hmm_user_dict_learn(tokens, types, 2, "你好", true);
    }

    // Verify the phrase appears in candidates
    char *cands[9] = {0};
    int n = get_candidates_bulk("nihao", cands, 9);
    check("'nihao' still returns candidates after learning phrase", n > 0);
    if (n > 0) {
        check("first candidate is still '你好'", strcmp(cands[0], "你好") == 0);
    }
    free_cands(cands, n);
}

static void test_user_dict_persist_and_reload(const char *executable,
                                              const char *so,
                                              const char *pack) {
    printf("[test_user_dict_persist_reload]\n");
    (void)so; (void)pack;

    // Learn something distinctive before persist
    const char *tokens[] = {"ce", "shi"};
    int types[] = {16, 16};
    for (int i = 0; i < 20; i++) {
        hmm_user_dict_learn(tokens, types, 2, "车时", true);
    }
    const char *english_tokens[] = {"g", "i", "t", "h", "u", "b"};
    int english_types[] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 20; i++) {
        hmm_user_dict_learn(english_tokens, english_types, 6, "GITHUB", true);
    }
    int size_before = hmm_user_dict_get_size();
    printf("    size before persist: %d\n", size_before);

    // Persist
    bool persist_ok = hmm_user_dict_persist();
    check("persist succeeds", persist_ok);

    // Check file exists
    char dict_path[4096];
    snprintf(dict_path, sizeof(dict_path), "tests/test_user_data/user_dict_3_3");
    struct stat st;
    bool file_exists = (stat(dict_path, &st) == 0 && st.st_size > 0);
    check("persisted file exists with data", file_exists);
    if (file_exists) {
        printf("    persisted file size: %lld bytes\n", (long long)st.st_size);
    }

    pid_t pid = fork();
    if (pid == 0) {
        execl(executable, executable, "--verify-user-dict", so, pack,
              "tests/test_user_data", "ceshi", "车时", "10", NULL);
        _exit(127);
    }
    int status = 0;
    bool child_ok = pid > 0 && waitpid(pid, &status, 0) == pid &&
                    WIFEXITED(status) && WEXITSTATUS(status) == 0;
    check("fresh process reloads learned ranking", child_ok);

    char english_path[4096];
    snprintf(english_path, sizeof(english_path),
             "tests/test_user_data/user_dict_3_3_english");
    check("English selection persisted to user_dict_3_3_english",
          stat(english_path, &st) == 0 && st.st_size > 0);

    pid = fork();
    if (pid == 0) {
        execl(executable, executable, "--verify-user-dict", so, pack,
              "tests/test_user_data", "github", "GITHUB", "1", NULL);
        _exit(127);
    }
    status = 0;
    child_ok = pid > 0 && waitpid(pid, &status, 0) == pid &&
               WIFEXITED(status) && WEXITSTATUS(status) == 0;
    check("fresh process reloads learned English ranking", child_ok);
    // Clearing removes both dictionaries' data and stale copies on disk.
    for (int i = 0; i < 5; i++) {
        hmm_user_dict_learn(tokens, types, 2, "车时", true);
        hmm_user_dict_learn(english_tokens, english_types, 6, "GITHUB", true);
    }
    FILE *stale = fopen("tests/test_user_data/user_dict_3_3_bak", "wb");
    if (stale) { fputs("stale", stale); fclose(stale); }
    stale = fopen("tests/test_user_data/user_dict_3_3_english_tmp", "wb");
    if (stale) { fputs("stale", stale); fclose(stale); }
    check("clear after reload succeeds", hmm_user_dict_clear());
    check("clear empties Chinese and English dictionaries",
          hmm_user_dict_get_size() == 0);
    check("clear removes Chinese backup",
          access("tests/test_user_data/user_dict_3_3_bak", F_OK) != 0);
    check("clear removes Chinese temp",
          access("tests/test_user_data/user_dict_3_3_tmp", F_OK) != 0);
    check("clear removes English backup",
          access("tests/test_user_data/user_dict_3_3_english_bak", F_OK) != 0);
    check("clear removes English temp",
          access("tests/test_user_data/user_dict_3_3_english_tmp", F_OK) != 0);

    pid = fork();
    if (pid == 0) {
        execl(executable, executable, "--verify-user-dict-empty", so, pack,
              "tests/test_user_data", NULL);
        _exit(127);
    }
    status = 0;
    child_ok = pid > 0 && waitpid(pid, &status, 0) == pid &&
               WIFEXITED(status) && WEXITSTATUS(status) == 0;
    check("fresh process reloads empty dictionaries after clear", child_ok);

    // Saving after clear must keep the dictionaries empty across reloads.
    check("persist after clear succeeds", hmm_user_dict_persist());
    pid = fork();
    if (pid == 0) {
        execl(executable, executable, "--verify-user-dict-empty", so, pack,
              "tests/test_user_data", NULL);
        _exit(127);
    }
    status = 0;
    child_ok = pid > 0 && waitpid(pid, &status, 0) == pid &&
               WIFEXITED(status) && WEXITSTATUS(status) == 0;
    check("fresh process stays empty after clear and persist", child_ok);

    unlink(english_path);
    unlink("tests/test_user_data/user_dict_3_3_english_bak");
    unlink("tests/test_user_data/user_dict_3_3_english_tmp");

    // Cleanup test data
    unlink(dict_path);
    char bak_path[4096], tmp_path[4096];
    snprintf(bak_path, sizeof(bak_path), "tests/test_user_data/user_dict_3_3_bak");
    snprintf(tmp_path, sizeof(tmp_path), "tests/test_user_data/user_dict_3_3_tmp");
    unlink(bak_path);
    unlink(tmp_path);
    rmdir("tests/test_user_data");
}

static int find_candidate(char **cands, int count, const char *text) {
    for (int i = 0; i < count; ++i) {
        if (cands[i] && strcmp(cands[i], text) == 0) return i;
    }
    return -1;
}

static int verify_user_dict_child(const char *executable, const char *so,
                                  const char *pack, const char *pinyin,
                                  const char *text, const char *max_position) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(executable, executable, "--verify-user-dict", so, pack,
              "tests/test_user_data", pinyin, text, max_position, NULL);
        _exit(127);
    }
    int status = 0;
    if (pid <= 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

// Exit 0 when a fresh process still offers `text` from the user dictionary
// alone (removable), 3 when it does not.
static int verify_user_dict_removable_child(const char *executable,
                                            const char *so, const char *pack,
                                            const char *pinyin,
                                            const char *text) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(executable, executable, "--verify-user-dict-removable", so, pack,
              "tests/test_user_data", pinyin, text, NULL);
        _exit(127);
    }
    int status = 0;
    if (pid <= 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static void test_user_dict_remove_candidate(const char *executable,
                                            const char *so,
                                            const char *pack) {
    printf("[test_user_dict_remove_candidate]\n");

    const char *tokens[] = {"ce", "shi"};
    int types[] = {16, 16};
    for (int i = 0; i < 20; ++i)
        hmm_user_dict_learn(tokens, types, 2, "侧试", true);
    check("remove: persist learned phrase", hmm_user_dict_persist());
    check("remove: fresh process ranks learned phrase first",
          verify_user_dict_child(executable, so, pack,
                                 "ceshi", "侧试", "0") == 0);

    char *cands[50] = {0};
    set_editor_context("");
    int count = get_candidates_bulk("ceshi", cands, 50);
    int learned = find_candidate(cands, count, "侧试");
    int system = find_candidate(cands, count, "测试");
    printf("    侧试 at %d, 测试 at %d\n", learned, system);
    check("remove: learned phrase is a candidate", learned >= 0);
    check("remove: learned-only candidate is removable",
          learned >= 0 && hmm_user_dict_is_candidate_removable(learned));
    check("remove: system candidate is not removable",
          system >= 0 && !hmm_user_dict_is_candidate_removable(system));
    check("remove: system candidate cannot be removed",
          system >= 0 && !hmm_user_dict_remove_candidate(system));
    check("remove: out-of-range candidate is not removable",
          !hmm_user_dict_is_candidate_removable(-1));
    check("remove: learned candidate is removed",
          learned >= 0 && hmm_user_dict_remove_candidate(learned));
    free_cands(cands, count);

    // Refill without resetting, as the IME does after removal.
    count = hmm_engine_get_candidates(cands, 50);
    int refilled = find_candidate(cands, count, "侧试");
    printf("    after removal, 侧试 at %d of %d\n", refilled, count);
    check("remove: refilled list still has candidates", count > 0);
    check("remove: refilled list no longer offers it from the user dict",
          refilled < 0 || !hmm_user_dict_is_candidate_removable(refilled));
    free_cands(cands, count);

    // Check the saved removal before anything else is learned.
    check("remove: persist first removal", hmm_user_dict_persist());
    check("remove: fresh process no longer has it in the user dict",
          verify_user_dict_removable_child(executable, so, pack,
                                           "ceshi", "侧试") == 3);

    // Learning after a persist must still reach the decoder.
    const char *other_tokens[] = {"ce", "shi"};
    int other_types[] = {16, 16};
    for (int i = 0; i < 20; ++i)
        hmm_user_dict_learn(other_tokens, other_types, 2, "厕视", true);
    count = get_candidates_bulk("ceshi", cands, 50);
    check("remove: learning after persist updates ranking live",
          find_candidate(cands, count, "厕视") == 0);
    check("remove: removal survives a fresh decode",
          find_candidate(cands, count, "侧试") < 0 ||
          !hmm_user_dict_is_candidate_removable(
              find_candidate(cands, count, "侧试")));
    free_cands(cands, count);

    check("remove: persist after relearning", hmm_user_dict_persist());
    check("remove: fresh process offers the relearned phrase",
          verify_user_dict_removable_child(executable, so, pack,
                                           "ceshi", "厕视") == 0);

    count = get_candidates_bulk("ceshi", cands, 50);
    int fresh = find_candidate(cands, count, "侧试");
    check("remove: fresh decode no longer offers it from the user dict",
          fresh < 0 || !hmm_user_dict_is_candidate_removable(fresh));
    free_cands(cands, count);

    // A save can land while candidates are shown; a removal made before
    // the next reset must not be lost when the decoder refreshes.
    count = get_candidates_bulk("ceshi", cands, 50);
    int pending = find_candidate(cands, count, "厕视");
    check("remove: second learned phrase is removable before save",
          pending >= 0 && hmm_user_dict_is_candidate_removable(pending));
    check("remove: persist with candidates shown", hmm_user_dict_persist());
    check("remove: removal right after persist succeeds",
          pending >= 0 && hmm_user_dict_remove_candidate(pending));
    free_cands(cands, count);
    // The IME refills without a reset right after removal.
    count = hmm_engine_get_candidates(cands, 50);
    int refill_after_save = find_candidate(cands, count, "厕视");
    printf("    post-save refill: 厕视 at %d removable=%d refresh=%d\n",
           refill_after_save,
           refill_after_save >= 0 &&
               hmm_user_dict_is_candidate_removable(refill_after_save),
           hmm_user_dict_decoder_refresh_pending());
    // The stale decoder can refill the phrase, so the IME must re-decode
    // (reset refreshes the decoder) while a refresh is pending.
    check("remove: post-save removal reports a pending decoder refresh",
          hmm_user_dict_decoder_refresh_pending());
    free_cands(cands, count);
    count = get_candidates_bulk("ceshi", cands, 50);
    int after_reset = find_candidate(cands, count, "厕视");
    check("remove: removal between persist and reset survives refresh",
          after_reset < 0 ||
          !hmm_user_dict_is_candidate_removable(after_reset));
    free_cands(cands, count);
    check("remove: persist post-save removal", hmm_user_dict_persist());
    check("remove: fresh process drops post-save removal",
          verify_user_dict_removable_child(executable, so, pack,
                                           "ceshi", "厕视") == 3);
}

static const HmmUserDictEntry *find_entry(const HmmUserDictEntry *entries,
                                          int count, const char *value) {
    for (int i = 0; i < count; ++i)
        if (strcmp(entries[i].value, value) == 0) return &entries[i];
    return NULL;
}

static int exported_count(const char *value) {
    HmmUserDictEntry *entries = NULL;
    int count = hmm_user_dict_export_entries(&entries);
    const HmmUserDictEntry *entry = find_entry(entries, count, value);
    int result = entry ? entry->count : (count < 0 ? -2 : -1);
    hmm_user_dict_free_entries(entries, count);
    return result;
}

static int removable_position(const char *pinyin, const char *text) {
    char *cands[50] = {0};
    int count = get_candidates_bulk(pinyin, cands, 50);
    int position = find_candidate(cands, count, text);
    bool removable = position >= 0 &&
        hmm_user_dict_is_candidate_removable(position);
    free_cands(cands, count);
    return removable ? position : -1;
}

static void test_user_dict_manage_entries(const char *executable,
                                          const char *so, const char *pack) {
    printf("[test_user_dict_manage_entries]\n");
    set_editor_context("");
    const char *tokens[] = {"ce", "shi"};
    int types[] = {16, 16};
    for (int i = 0; i < 3; ++i)
        hmm_user_dict_learn(tokens, types, 2, "测视", true);
    const char *letters[] = {"q", "x", "z"};
    int letter_types[] = {0, 0, 0};
    for (int i = 0; i < 2; ++i)
        hmm_user_dict_learn(letters, letter_types, 3, "qxz", true);

    HmmUserDictEntry *entries = NULL;
    int count = hmm_user_dict_export_entries(&entries);
    const HmmUserDictEntry *pinyin = find_entry(entries, count, "测视");
    const HmmUserDictEntry *english = find_entry(entries, count, "qxz");
    printf("    exported %d entries\n", count);
    check("manage: export lists the learned phrase",
          pinyin && pinyin->dictionary == HMM_USER_DICT_PINYIN &&
          pinyin->count == 3 && pinyin->token_count == 2 &&
          strcmp(pinyin->tokens[0], "ce") == 0 &&
          strcmp(pinyin->tokens[1], "shi") == 0 &&
          pinyin->languages[0] == HMM_LANGUAGE_CHINESE &&
          pinyin->languages[1] == HMM_LANGUAGE_CHINESE && pinyin->full_match);
    check("manage: export lists the English word",
          english && english->dictionary == HMM_USER_DICT_ENGLISH &&
          english->count == 2 && english->token_count == 3);
    bool only_positive = count >= 0;
    for (int i = 0; i < count; ++i)
        only_positive = only_positive && entries[i].count > 0;
    check("manage: export skips zero-count entries", only_positive);
    bool repeated = true;
    for (int i = 0; i < 200 && repeated; ++i)
        repeated = exported_count("测视") == 3;
    check("manage: repeated exports keep working", repeated);

    HmmUserDictEntry phrase = pinyin ? *pinyin : (HmmUserDictEntry){0};
    check("manage: zero count is rejected",
          !hmm_user_dict_set_entry_count(&phrase, 0, phrase.full_match));
    check("manage: update sets the count",
          hmm_user_dict_set_entry_count(&phrase, 50, phrase.full_match) &&
          exported_count("测视") == 50);
    check("manage: raised count ranks the phrase first",
          removable_position("ceshi", "测视") == 0);
    phrase.count = 50;
    check("manage: set count can lower a count",
          hmm_user_dict_set_entry_count(&phrase, 2, phrase.full_match) &&
          exported_count("测视") == 2);
    phrase.count = 2;
    check("manage: set count raises it again",
          hmm_user_dict_set_entry_count(&phrase, 50, phrase.full_match) &&
          exported_count("测视") == 50);
    phrase.count = 50;

    char long_token[] = "abcdefghijklmnop";
    char *long_tokens[] = {long_token, phrase.tokens ? phrase.tokens[1] : NULL};
    HmmUserDictEntry oversized = phrase;
    oversized.tokens = long_tokens;
    check("manage: oversized tokens are rejected",
          !hmm_user_dict_set_entry_count(&oversized, 9, oversized.full_match) &&
          exported_count("测视") == 50);
    oversized.tokens = phrase.tokens;
    oversized.token_count = HMM_USER_DICT_MAX_TOKENS + 1;
    check("manage: too many tokens are rejected",
          !hmm_user_dict_set_entry_count(&oversized, 9, oversized.full_match));

    int wrong_languages[] = {0, 0};
    HmmUserDictEntry wrong = phrase;
    wrong.languages = wrong_languages;
    check("manage: English languages are rejected in the pinyin dictionary",
          !hmm_user_dict_set_entry_count(&wrong, 9, wrong.full_match) &&
          exported_count("测视") == 50);
    wrong.dictionary = HMM_USER_DICT_ENGLISH;
    check("manage: removal needs the exported token languages",
          !hmm_user_dict_remove_entry(&wrong) && exported_count("测视") == 50);
    int other_languages[] = {69, 69};
    wrong = phrase;
    wrong.languages = other_languages;
    check("manage: unsupported languages are rejected",
          !hmm_user_dict_remove_entry(&wrong) &&
          exported_count("测视") == 50);
    check("manage: remove deletes the entry",
          hmm_user_dict_remove_entry(&phrase) &&
          exported_count("测视") == -1);
    check("manage: removed phrase is no longer a learned candidate",
          removable_position("ceshi", "测视") < 0);
    check("manage: update restores a removed entry",
          hmm_user_dict_set_entry_count(&phrase, 7, phrase.full_match) &&
          exported_count("测视") == 7 &&
          removable_position("ceshi", "测视") >= 0);
    check("manage: remove after restore",
          hmm_user_dict_remove_entry(&phrase) &&
          exported_count("测视") == -1);
    check("manage: persist removal", hmm_user_dict_persist());
    check("manage: fresh process does not offer the removed entry",
          verify_user_dict_removable_child(executable, so, pack,
                                           "ceshi", "测视") == 3);
    const char *partial_tokens[] = {"zi", "fu"};
    for (int i = 0; i < 2; ++i)
        hmm_user_dict_learn(partial_tokens, types, 2, "字幅", false);
    HmmUserDictEntry *partial_entries = NULL;
    int partial_count = hmm_user_dict_export_entries(&partial_entries);
    const HmmUserDictEntry *found = find_entry(partial_entries, partial_count,
                                               "字幅");
    HmmUserDictEntry partial = found ? *found : (HmmUserDictEntry){0};
    check("manage: partial-match learning exports full_match false",
          found && !found->full_match && found->count == 2);
    check("manage: partial-match count can be lowered and raised",
          hmm_user_dict_set_entry_count(&partial, 1, partial.full_match) &&
          exported_count("字幅") == 1 &&
          hmm_user_dict_set_entry_count(&partial, 30, partial.full_match) &&
          exported_count("字幅") == 30);
    hmm_user_dict_free_entries(partial_entries, partial_count);
    partial_entries = NULL;
    partial_count = hmm_user_dict_export_entries(&partial_entries);
    found = find_entry(partial_entries, partial_count, "字幅");
    check("manage: count changes keep the match type",
          found && !found->full_match);
    partial = found ? *found : (HmmUserDictEntry){0};
    check("manage: a partial match upgrades at the same count",
          found && hmm_user_dict_set_entry_count(&partial, 30, true) &&
          exported_count("字幅") == 30);
    hmm_user_dict_free_entries(partial_entries, partial_count);
    partial_entries = NULL;
    partial_count = hmm_user_dict_export_entries(&partial_entries);
    found = find_entry(partial_entries, partial_count, "字幅");
    check("manage: the upgraded entry is a full match", found && found->full_match);
    hmm_user_dict_free_entries(partial_entries, partial_count);

    phrase.count = 0;
    phrase.full_match = false;
    bool restored_partial = hmm_user_dict_set_entry_count(&phrase, 5, phrase.full_match);
    int restored_count = exported_count("测视");
    printf("    full-match tombstone restored as partial: %d (count %d)\n",
           restored_partial, restored_count);
    check("manage: a removed full-match entry comes back as partial",
          restored_partial && restored_count == 5);
    phrase.full_match = true;
    phrase.count = restored_count > 0 ? restored_count : 0;
    hmm_user_dict_remove_entry(&phrase);

    // Learning joins consecutive selections and has no token limit.
    enum { LONG_TOKENS = 40 };
    const char *learned_tokens[LONG_TOKENS];
    int long_types[LONG_TOKENS];
    char long_value[LONG_TOKENS * 3 + 1] = {0};
    for (int i = 0; i < LONG_TOKENS; ++i) {
        learned_tokens[i] = "ce";
        long_types[i] = HMM_LANGUAGE_CHINESE;
        strcat(long_value, "测");
    }
    hmm_user_dict_learn(learned_tokens, long_types, LONG_TOKENS, long_value, true);
    HmmUserDictEntry *long_entries = NULL;
    int long_count = hmm_user_dict_export_entries(&long_entries);
    found = find_entry(long_entries, long_count, long_value);
    HmmUserDictEntry long_entry = found ? *found : (HmmUserDictEntry){0};
    check("manage: long learned phrases are exported",
          found && found->token_count == LONG_TOKENS);
    check("manage: long learned phrases can be edited and removed",
          found && hmm_user_dict_set_entry_count(&long_entry, 4, long_entry.full_match) &&
          exported_count(long_value) == 4 &&
          hmm_user_dict_remove_entry(&long_entry) &&
          exported_count(long_value) == -1);
    hmm_user_dict_free_entries(long_entries, long_count);

    const char *relearn_tokens[] = {"ci", "fu"};
    hmm_user_dict_learn(relearn_tokens, types, 2, "词夫", true);
    HmmUserDictEntry *relearn_entries = NULL;
    int relearn_count = hmm_user_dict_export_entries(&relearn_entries);
    found = find_entry(relearn_entries, relearn_count, "词夫");
    bool relearn_removed = found && hmm_user_dict_remove_entry(found);
    hmm_user_dict_free_entries(relearn_entries, relearn_count);
    hmm_user_dict_learn(relearn_tokens, types, 2, "词夫", false);
    int relearned = exported_count("词夫");
    printf("    partial relearn after full-match removal: count %d\n", relearned);
    check("manage: a removed word can be relearned by partial match",
          relearn_removed && relearned == 1);

    HmmUserDictEntry letters_entry = english ? *english : (HmmUserDictEntry){0};
    check("manage: English count can be lowered",
          english && hmm_user_dict_set_entry_count(&letters_entry, 1, letters_entry.full_match) &&
          exported_count("qxz") == 1);
    letters_entry.count = 1;
    char *word_token[] = {"qxz"};
    int english_language[] = {HMM_LANGUAGE_ENGLISH};
    HmmUserDictEntry whole_word = letters_entry;
    whole_word.tokens = word_token;
    whole_word.languages = english_language;
    whole_word.token_count = 1;
    check("manage: multi-letter English tokens are not inserted",
          !hmm_user_dict_set_entry_count(&whole_word, 9, whole_word.full_match));
    check("manage: remove the English word",
          english && hmm_user_dict_remove_entry(english) &&
          exported_count("qxz") == -1);
    hmm_user_dict_free_entries(entries, count);
}

static void test_user_dict_clear(void) {
    printf("[test_user_dict_clear]\n");

    const char *tokens[] = {"ce", "shi"};
    int types[] = {16, 16};
    for (int i = 0; i < 20; ++i)
        hmm_user_dict_learn(tokens, types, 2, "车时", true);

    int size_before = hmm_user_dict_get_size();
    printf("    size before clear: %d\n", size_before);

    bool ok = hmm_user_dict_clear();
    check("clear succeeds", ok);

    int size_after = hmm_user_dict_get_size();
    printf("    size after clear: %d\n", size_after);
    check("dict size is 0 after clear", size_after == 0);

    char *cands[20] = {0};
    int count = get_candidates_bulk("ceshi", cands, 20);
    int position = -1;
    for (int i = 0; i < count; ++i) {
        if (strcmp(cands[i], "车时") == 0) {
            position = i;
            break;
        }
    }
    check("clear immediately removes learned ranking", position != 1);
    free_cands(cands, count);
}

static void test_next_word_prediction(void) {
    printf("[test_next_word_prediction]\n");
    char *cands[20] = {0};

    set_editor_context("");
    int count = hmm_engine_get_predictions(cands, 20);
    check("prediction: empty context yields none", count == 0);
    free_cands(cands, count);

    set_editor_context("你好");
    char *many[100] = {0};
    count = hmm_engine_get_predictions(many, 100);
    check("prediction: capped at 50", count > 0 && count <= 50);
    free_cands(many, count);
    count = hmm_engine_get_predictions(cands, 20);
    printf("    你好 →");
    for (int i = 0; i < count && i < 9; i++) printf(" %s", cands[i]);
    printf("\n");
    check("prediction: '你好' yields predictions", count > 0);
    bool unique = true;
    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++)
            if (strcmp(cands[i], cands[j]) == 0) unique = false;
    check("prediction: results are unique", unique);
    free_cands(cands, count);

    set_editor_context("生日快");
    count = hmm_engine_get_predictions(cands, 20);
    printf("    生日快 →");
    for (int i = 0; i < count && i < 9; i++) printf(" %s", cands[i]);
    printf("\n");
    check("prediction: '生日快' predicts '乐'",
          has_candidate(cands, count, "乐"));
    free_cands(cands, count);

    set_editor_context("你好。");
    count = hmm_engine_get_predictions(cands, 20);
    check("prediction: trailing punctuation yields none", count == 0);
    free_cands(cands, count);

    // Empty context still discards an active composition.
    set_editor_context("");
    count = get_candidates_bulk("nihao", cands, 9);
    free_cands(cands, count);
    count = hmm_engine_get_predictions(cands, 20);
    free_cands(cands, count);
    count = hmm_engine_get_candidates(cands, 9);
    check("prediction: empty context resets composition", count == 0);
    free_cands(cands, count);

    // Decoding still works after prediction.
    set_editor_context("");
    count = get_candidates_bulk("nihao", cands, 9);
    check("prediction: decoding works afterwards",
          count > 0 && strcmp(cands[0], "你好") == 0);
    free_cands(cands, count);
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    const char *so = "../source/resources/lib/arm64-v8a/libintegrated_shared_object.so";
    const char *pack = "../hmmoemdata/current";
    if (argc == 4 && strcmp(argv[1], "--shuangpin-only") == 0) {
        char user_dir[] = "/tmp/gboard-shuangpin-XXXXXX";
        if (!mkdtemp(user_dir)) {
            perror("mkdtemp(shuangpin)");
            return 2;
        }
        if (!hmm_engine_init_with_user_data(argv[2], argv[3], user_dir)) return 2;
        test_shuangpin();
        check("persist shuangpin learning", hmm_user_dict_persist());
        pid_t pid = fork();
        if (pid == 0) {
            execl(argv[0], argv[0], "--verify-user-dict", argv[2], argv[3],
                  user_dir, "ceshi", "侧试", "500", NULL);
            _exit(127);
        }
        int status = 0;
        check("fresh process reloads shuangpin learning as full pinyin",
              pid > 0 && waitpid(pid, &status, 0) == pid &&
              WIFEXITED(status) && WEXITSTATUS(status) == 0);
        check("clear entries learned by shuangpin test", hmm_user_dict_clear());
        hmm_engine_destroy();
        const char *files[] = {
            "user_dict_3_3", "user_dict_3_3_bak", "user_dict_3_3_tmp",
            "user_dict_3_3_english", "user_dict_3_3_english_bak",
            "user_dict_3_3_english_tmp"
        };
        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", user_dir, files[i]);
            unlink(path);
        }
        rmdir(user_dir);
        printf("Results: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail > 0 ? 1 : 0;
    }
    if (argc == 4 && strcmp(argv[1], "--fuzzy-only") == 0) {
        test_jni_native_lookup();
        if (!hmm_engine_init(argv[2], argv[3])) return 2;
        test_fuzzy_pinyin();
        hmm_engine_destroy();
        printf("Results: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail > 0 ? 1 : 0;
    }
    if (argc == 5 && strcmp(argv[1], "--verify-ranking-policy") == 0) {
        if (!hmm_engine_init_with_user_data(argv[2], argv[3], argv[4]))
            return 2;

        char *cands[20] = {0};
        set_editor_context("");
        int count = get_candidates_bulk("bushu", cands, 20);
        bool standalone_ok =
            count > 0 && strcmp(cands[0], "部署") == 0;
        printf("    standalone bushu: %s\n",
               count > 0 ? cands[0] : "(none)");
        free_cands(cands, count);

        set_editor_context("我对这里很");
        count = get_candidates_bulk("bushu", cands, 20);
        bool context_ok =
            count > 0 && strcmp(cands[0], "不熟") == 0;
        printf("    contextual bushu: %s\n",
               count > 0 ? cands[0] : "(none)");
        free_cands(cands, count);

        set_editor_context("");
        count = get_candidates_bulk("woduizhelihenbushu", cands, 20);
        bool continuous_ok =
            count > 0 && strcmp(cands[0], "我对这里很不熟") == 0;
        printf("    continuous phrase: %s\n",
               count > 0 ? cands[0] : "(none)");
        free_cands(cands, count);

        hmm_engine_destroy();
        return standalone_ok && context_ok && continuous_ok ? 0 : 3;
    }
    if (argc == 9 &&
        strcmp(argv[1], "--verify-user-dict-context") == 0) {
        if (!hmm_engine_init_with_user_data(argv[2], argv[3], argv[4]))
            return 2;
        set_editor_context(argv[5]);
        char *cands[500] = {0};
        int count = get_candidates_bulk(argv[6], cands, 500);
        int position = -1;
        for (int i = 0; i < count; ++i) {
            if (strcmp(cands[i], argv[7]) == 0) {
                position = i;
            }
        }
        free_cands(cands, count);
        int max_position = atoi(argv[8]);
        hmm_engine_destroy();
        return position >= 0 && position <= max_position ? 0 : 3;
    }
    if (argc == 5 && strcmp(argv[1], "--verify-user-dict-empty") == 0) {
        if (!hmm_engine_init_with_user_data(argv[2], argv[3], argv[4]))
            return 2;
        int size = hmm_user_dict_get_size();
        hmm_engine_destroy();
        return size == 0 ? 0 : 3;
    }
    if (argc == 7 && strcmp(argv[1], "--verify-user-dict-removable") == 0) {
        if (!hmm_engine_init_with_user_data(argv[2], argv[3], argv[4]))
            return 2;
        char *cands[500] = {0};
        int count = get_candidates_bulk(argv[5], cands, 500);
        int position = find_candidate(cands, count, argv[6]);
        bool removable = position >= 0 &&
            hmm_user_dict_is_candidate_removable(position);
        free_cands(cands, count);
        hmm_engine_destroy();
        return removable ? 0 : 3;
    }
    if (argc == 8 && strcmp(argv[1], "--verify-user-dict") == 0) {
        if (!hmm_engine_init_with_user_data(argv[2], argv[3], argv[4]))
            return 2;
        char *cands[500] = {0};
        int count = get_candidates_bulk(argv[5], cands, 500);
        int position = -1;
        for (int i = 0; i < count; ++i) {
            if (strcmp(cands[i], argv[6]) == 0) {
                position = i;
            }
        }
        free_cands(cands, count);
        int max_position = atoi(argv[7]);
        hmm_engine_destroy();
        return position >= 0 && position <= max_position ? 0 : 3;
    }
    if (argc > 2) { so = argv[1]; pack = argv[2]; }

    // Use a test-specific user data directory
    const char *test_user_dir = "tests/test_user_data";
    mkdir(test_user_dir, 0755);
    unlink("tests/test_user_data/user_dict_3_3");
    unlink("tests/test_user_data/user_dict_3_3_tmp");
    unlink("tests/test_user_data/user_dict_3_3_bak");
    unlink("tests/test_user_data/user_dict_3_3_english");
    unlink("tests/test_user_data/user_dict_3_3_english_tmp");
    unlink("tests/test_user_data/user_dict_3_3_english_bak");

    test_jni_native_lookup();
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
    test_long_sentence_candidate_regression();
    test_neural_activation_boundary();
    test_neural_bulk_incremental_parity();
    test_neural_reranker_reset_stress();
    test_context_language_scoring();
    test_continuous_phrase_language_scoring();
    test_select_and_continue();
    test_candidate_range();
    test_full_ime_simulation();
    test_brute_force_initials();
    test_brute_force_phrases();
    test_brute_force_random();
    test_brute_force_incremental();
    test_segmented_pinyin();
    test_english_mixed_input();
    test_next_word_prediction();
    test_fuzzy_pinyin();
    pid_t shuangpin_pid = fork();
    if (shuangpin_pid == 0) {
        execl(argv[0], argv[0], "--shuangpin-only", so, pack, NULL);
        _exit(127);
    }
    int shuangpin_status = 0;
    check("native shuangpin integration passes in an isolated process",
          shuangpin_pid > 0 &&
          waitpid(shuangpin_pid, &shuangpin_status, 0) == shuangpin_pid &&
          WIFEXITED(shuangpin_status) && WEXITSTATUS(shuangpin_status) == 0);

    // User dictionary tests
    test_user_dict_init();
    test_user_dict_token_extraction();
    test_context_overrides_user_frequency();
    test_user_dict_learn_and_boost();
    test_user_dict_learn_phrase();
    test_user_dict_remove_candidate(argv[0], so, pack);
    test_user_dict_manage_entries(argv[0], so, pack);
    test_user_dict_clear();

    // Persist/reload test — destroys and reinits engine
    test_user_dict_persist_and_reload(argv[0], so, pack);

    printf("\n══════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    printf("══════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
