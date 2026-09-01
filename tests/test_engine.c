#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "engine/hmm_engine.h"
#include "engine/hmm_user_dict.h"

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

    hmm_engine_set_context("");
    int baseline_count = get_candidates_bulk("bushu", cands, 20);
    check("context baseline prefers 部署",
          baseline_count > 0 && strcmp(cands[0], "部署") == 0);
    free_cands(cands, baseline_count);

    hmm_engine_set_context("我对这里很");
    int contextual_count = get_candidates_bulk("bushu", cands, 20);
    check("Chinese context reranks bushu to 不熟",
          contextual_count > 0 && strcmp(cands[0], "不熟") == 0);
    check("context candidate range excludes committed prefix",
          contextual_count > 0 &&
          hmm_engine_get_candidate_consumed(0) == 5);
    free_cands(cands, contextual_count);

    hmm_engine_set_context("无关前缀我对这里很");
    int truncated_count = get_candidates_bulk("bushu", cands, 20);
    check("Chinese context keeps the trailing five characters",
          truncated_count > 0 && strcmp(cands[0], "不熟") == 0);
    free_cands(cands, truncated_count);

    hmm_engine_set_context("我对这里很，");
    int boundary_count = get_candidates_bulk("bushu", cands, 20);
    check("punctuation ends the Java-compatible context window",
          boundary_count > 0 && strcmp(cands[0], "部署") == 0);
    free_cands(cands, boundary_count);

    hmm_engine_set_context("");
    hmm_engine_reset();
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

// ── Syllable breaks ─────────────────────────────────────────────────────────

static void test_syllable_breaks(void) {
    printf("[test_syllable_breaks]\n");

    struct { const char *input; const char *expected; } cases[] = {
        { "nihao",              "ni'hao" },
        { "zhongwenshurufa",    "zhong'wen'shu'ru'fa" },
        { "woquni",             "wo'qu'ni" },
        { "beijing",            "bei'jing" },
        { "woaini",             "wo'ai'ni" },
        { "xian",               "xian" },
        { "jintiantianqihenhao","jin'tian'tian'qi'hen'hao" },
        { "woshizhongguoren",   "wo'shi'zhong'guo'ren" },
    };
    int total = sizeof(cases) / sizeof(cases[0]);
    int ok = 0;

    for (int c = 0; c < total; c++) {
        const char *pinyin = cases[c].input;
        char *cands[9] = {0};
        get_candidates_for(pinyin, cands, 9);

        int breaks[16];
        int n = hmm_engine_get_syllable_breaks(breaks, 16);

        // Build segmented string
        char buf[256] = {0};
        int pos = 0, prev = 0;
        int len = (int)strlen(pinyin);
        for (int i = 0; i < n; i++) {
            if (prev > 0) buf[pos++] = '\'';
            int bp = breaks[i];
            memcpy(buf + pos, pinyin + prev, bp - prev);
            pos += bp - prev;
            prev = bp;
        }
        if (prev > 0) buf[pos++] = '\'';
        memcpy(buf + pos, pinyin + prev, len - prev);
        pos += len - prev;
        buf[pos] = '\0';

        bool match = strcmp(buf, cases[c].expected) == 0;
        if (match) ok++;
        else printf("    FAIL: '%s' → '%s' (expected '%s')\n", pinyin, buf, cases[c].expected);

        free_cands(cands, 9);
    }
    printf("    %d/%d syllable breaks correct\n", ok, total);
    check("syllable breaks match engine segmentation", ok == total);
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
    }
    free_cands(cands, n);
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

    // Cleanup test data
    unlink(dict_path);
    char bak_path[4096], tmp_path[4096];
    snprintf(bak_path, sizeof(bak_path), "tests/test_user_data/user_dict_3_3_bak");
    snprintf(tmp_path, sizeof(tmp_path), "tests/test_user_data/user_dict_3_3_tmp");
    unlink(bak_path);
    unlink(tmp_path);
    rmdir("tests/test_user_data");
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

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    const char *so = "../source/resources/lib/arm64-v8a/libintegrated_shared_object.so";
    const char *pack = "../hmmoemdata/current";
    if (argc == 8 && strcmp(argv[1], "--verify-user-dict") == 0) {
        if (!hmm_engine_init_with_user_data(argv[2], argv[3], argv[4]))
            return 2;
        char *cands[20] = {0};
        int count = get_candidates_bulk(argv[5], cands, 20);
        int position = -1;
        for (int i = 0; i < count; ++i) {
            if (strcmp(cands[i], argv[6]) == 0) {
                position = i;
                break;
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
    test_select_and_continue();
    test_candidate_range();
    test_full_ime_simulation();
    test_brute_force_initials();
    test_brute_force_phrases();
    test_brute_force_random();
    test_brute_force_incremental();
    test_syllable_breaks();

    // User dictionary tests
    test_user_dict_init();
    test_user_dict_token_extraction();
    test_user_dict_learn_and_boost();
    test_user_dict_learn_phrase();
    test_user_dict_clear();

    // Persist/reload test — destroys and reinits engine
    test_user_dict_persist_and_reload(argv[0], so, pack);

    printf("\n══════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    printf("══════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
