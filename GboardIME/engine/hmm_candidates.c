// Input append, candidate retrieval, selection, and reset.
#include "hmm_internal.h"
#include "hmm_user_dict.h"

enum {
    INPUT_TYPE_SOURCE_INPUT_UNIT = 0,
    INPUT_TYPE_TARGET_TOKEN = 2,
    SEPARATOR_TOKEN = 1,
    SEPARATOR_SEGMENT = 2,
    END_VERTEX_SENTINEL = 32767,
    CONTEXT_MAX_UTF8_BYTES = 256,
    PREDICTION_MAX_CANDIDATES = 50,
    MAX_CONVERTED_RANGES = 64
};

typedef struct {
    size_t start;
    int utf16_units;
    int language;
} ContextCodepoint;

static char s_context[CONTEXT_MAX_UTF8_BYTES];
static bool s_context_ends_latin;
static bool s_context_injected;
static int s_context_end_vertex;
static bool s_settings_ready = true;

// Ranges of candidates selected in the active composition, like the
// selected-range stack of Gboard's AbstractHmmEngineWrapper.
typedef struct {
    int start;
    int end;
} VertexRange;
static VertexRange s_converted[MAX_CONVERTED_RANGES];
static int s_converted_count;
static uint64_t s_input_generation;

// Shuangpin key correction. Scores are log likelihoods like those of
// Gboard's touch spatial model, where a single key scores 0. The penalties
// were tuned on all six schemes against typos and dictionary words.
// Shorter input stays literal: two keys are nearly always a valid syllable,
// and initial-letter abbreviations would lose to corrected syllables.
enum { KEY_HISTORY_MAX = 512, KEY_CORRECTION_MIN_KEYS = 4 };
static const float TYPED_KEY_SCORE = 0.0f;
static const float NEIGHBOR_KEY_SCORE = -6.0f;
static const float SWAPPED_KEYS_SCORE = -6.0f;
static bool s_key_correction_enabled;
static bool s_key_correction_active;
static bool s_semicolon_is_key;
// Key typed between vertices v and v + 1.
static char s_typed_keys[KEY_HISTORY_MAX];
// A swapped-key edge ends at vertex v + 1.
static bool s_swap_added[KEY_HISTORY_MAX];

// Candidates cover the input after the last selection, or after the
// injected context when nothing is selected yet.
static int candidate_start_vertex(void) {
    return s_converted_count > 0
        ? s_converted[s_converted_count - 1].end
        : s_context_end_vertex;
}

static size_t utf8_codepoint_length(const unsigned char *s,
                                    size_t remaining) {
    if (remaining == 0) return 0;
    if (s[0] < 0x80) return 1;
    if (remaining >= 2 && (s[0] & 0xe0) == 0xc0 &&
        (s[1] & 0xc0) == 0x80) return 2;
    if (remaining >= 3 && (s[0] & 0xf0) == 0xe0 &&
        (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) return 3;
    if (remaining >= 4 && (s[0] & 0xf8) == 0xf0 &&
        (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 &&
        (s[3] & 0xc0) == 0x80) return 4;
    return 1;
}

static uint32_t utf8_decode(const unsigned char *s, size_t len) {
    if (len == 1) return s[0];
    if (len == 2) return ((uint32_t)(s[0] & 0x1f) << 6) |
                         (uint32_t)(s[1] & 0x3f);
    if (len == 3) return ((uint32_t)(s[0] & 0x0f) << 12) |
                         ((uint32_t)(s[1] & 0x3f) << 6) |
                         (uint32_t)(s[2] & 0x3f);
    return ((uint32_t)(s[0] & 0x07) << 18) |
           ((uint32_t)(s[1] & 0x3f) << 12) |
           ((uint32_t)(s[2] & 0x3f) << 6) |
           (uint32_t)(s[3] & 0x3f);
}

static int context_language(uint32_t cp) {
    if ((cp >= 0x3400 && cp <= 0x4dbf) ||
        (cp >= 0x4e00 && cp <= 0x9fff) ||
        (cp >= 0x20000 && cp <= 0x2a6df)) {
        return 2;
    }
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
        return 3;
    }
    return 1;
}

bool hmm_engine_set_context(const char *text_before_cursor) {
    JNI_LOCAL_FRAME();
    const unsigned char *text =
        (const unsigned char *)(text_before_cursor ? text_before_cursor : "");
    size_t byte_length = strlen((const char *)text);
    ContextCodepoint *points = byte_length > 0
        ? calloc(byte_length, sizeof(*points))
        : NULL;
    if (byte_length > 0 && !points) return false;
    size_t pos = 0;
    size_t count = 0;

    while (pos < byte_length) {
        size_t length =
            utf8_codepoint_length(text + pos, byte_length - pos);
        uint32_t cp = utf8_decode(text + pos, length);
        points[count++] = (ContextCodepoint){
            .start = pos,
            .utf16_units = cp > 0xffff ? 2 : 1,
            .language = context_language(cp)
        };
        pos += length;
    }

    size_t start = byte_length;
    s_context_ends_latin = false;
    if (count > 0) {
        int language = points[count - 1].language;
        if (language != 1) {
            int limit = language == 2 ? 5 : 20;
            int retained_units = 0;
            s_context_ends_latin = language == 3;
            for (size_t i = count; i > 0; i--) {
                ContextCodepoint *point = &points[i - 1];
                if (point->language != language || retained_units >= limit) {
                    break;
                }
                start = point->start;
                retained_units += point->utf16_units;
            }
        }
    }

    size_t kept = byte_length - start;
    if (kept >= sizeof(s_context)) kept = sizeof(s_context) - 1;
    if (kept > 0) memcpy(s_context, text + start, kept);
    s_context[kept] = '\0';
    hmm_engine_set_external_context(s_context[0] != '\0');
    free(points);
    s_context_injected = false;
    s_context_end_vertex = 0;
    LOG("set context: raw='%s' normalized='%s' latin=%d",
        text_before_cursor ? text_before_cursor : "", s_context,
        s_context_ends_latin);
    return true;
}

static bool append_context(void) {
    if (s_context_injected) return true;
    s_context_injected = true;
    s_context_end_vertex = 0;
    if (!s_context[0]) return true;

    const unsigned char *text = (const unsigned char *)s_context;
    size_t length = strlen(s_context);
    size_t pos = 0;
    while (pos < length) {
        size_t char_length =
            utf8_codepoint_length(text + pos, length - pos);
        char token[5] = {0};
        memcpy(token, text + pos, char_length);

        jobject arr = jni_NewObjectArray(g_env, 1, NULL, NULL);
        jobject si = jni_create_scored_input(g_env, token, 0.0f);
        jni_SetObjectArrayElement(g_env, arr, 0, si);

        jint result = 0;
        CRASH_PROTECT_BEGIN()
        result = g_append(g_env, NULL, g_engine, arr, INPUT_TYPE_TARGET_TOKEN);
        CRASH_PROTECT_END("nativeAppend(context)")
        if (result <= 0) {
            LOGERR("context append failed for '%s'; continuing without context",
                   token);
            if (g_reset) {
                CRASH_PROTECT_BEGIN()
                g_reset(g_env, NULL, g_engine);
                CRASH_PROTECT_END("nativeReset(context append failure)")
            }
            g_end_vertex = 0;
            return true;
        }
        g_end_vertex = result;
        pos += char_length;
    }

    if (g_setSeparator) {
        CRASH_PROTECT_BEGIN()
        g_setSeparator(g_env, NULL, g_engine, END_VERTEX_SENTINEL,
                       SEPARATOR_TOKEN);
        CRASH_PROTECT_END("nativeSetSeparator(context token)")
        if (s_context_ends_latin) {
            CRASH_PROTECT_BEGIN()
            g_setSeparator(g_env, NULL, g_engine, END_VERTEX_SENTINEL,
                           SEPARATOR_SEGMENT);
            CRASH_PROTECT_END("nativeSetSeparator(context segment)")
        }
    }

    if (!g_getDecodingRange || !g_selectRange) {
        LOGERR("context range methods unavailable; continuing without context");
        if (g_reset) {
            CRASH_PROTECT_BEGIN()
            g_reset(g_env, NULL, g_engine);
            CRASH_PROTECT_END("nativeReset(context methods unavailable)")
        }
        g_end_vertex = 0;
        return true;
    }

    jobject range = NULL;
    CRASH_PROTECT_BEGIN()
    range = g_getDecodingRange(g_env, NULL, g_engine);
    CRASH_PROTECT_END("nativeGetDecodingRange(context)")
    if (!range) {
        LOGERR("context decoding range unavailable; continuing without context");
        if (g_reset) {
            CRASH_PROTECT_BEGIN()
            g_reset(g_env, NULL, g_engine);
            CRASH_PROTECT_END("nativeReset(context range unavailable)")
        }
        g_end_vertex = 0;
        return true;
    }
    int start = 0, end = 0;
    jni_get_range(range, &start, &end);
    if (start < end || end < 0) {
        jboolean selected = JNI_FALSE;
        CRASH_PROTECT_BEGIN()
        selected = g_selectRange(g_env, NULL, g_engine, range);
        CRASH_PROTECT_END("nativeSelectRange(context)")
        if (!selected) {
            LOGERR("context range selection failed; continuing without context");
            if (g_reset) {
                CRASH_PROTECT_BEGIN()
                g_reset(g_env, NULL, g_engine);
                CRASH_PROTECT_END("nativeReset(context selection failure)")
            }
            g_end_vertex = 0;
            return true;
        }
    }
    s_context_end_vertex = end > 0 ? end : 0;
    g_end_vertex = s_context_end_vertex;
    LOG("context selected: range=(%d,%d)", start, end);
    return true;
}

void hmm_engine_set_shuangpin_key_correction(bool enabled) {
    s_key_correction_enabled = enabled;
}

bool hmm_engine_get_shuangpin_key_correction(void) {
    return s_key_correction_enabled;
}

static bool key_correction_applies(size_t input_length) {
    return s_key_correction_enabled && s_settings_ready &&
           hmm_engine_get_pinyin_scheme() != HMM_PINYIN_FULL &&
           input_length >= KEY_CORRECTION_MIN_KEYS;
}

static bool is_shuangpin_key(char key) {
    return (key >= 'a' && key <= 'z') || (key == ';' && s_semicolon_is_key);
}

// The keys left and right of a key on its QWERTY row. Like librime and
// libime, rows are not crossed: physical keyboards rarely miss vertically.
static int qwerty_neighbors(char key, char neighbors[2]) {
    static const char *const rows[] = {
        "qwertyuiop", "asdfghjkl;", "zxcvbnm"
    };
    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        const char *position = strchr(rows[r], key);
        if (!position || !key) continue;
        int count = 0;
        if (position > rows[r]) neighbors[count++] = position[-1];
        if (position[1]) neighbors[count++] = position[1];
        return count;
    }
    return 0;
}

static jobject key_alternatives(char key) {
    char neighbors[2], keys[3] = { key };
    int count = 1;
    int neighbor_count =
        is_shuangpin_key(key) ? qwerty_neighbors(key, neighbors) : 0;
    for (int i = 0; i < neighbor_count; i++) {
        if (is_shuangpin_key(neighbors[i])) keys[count++] = neighbors[i];
    }
    jobject arr = jni_NewObjectArray(g_env, count, NULL, NULL);
    for (int i = 0; i < count; i++) {
        char text[2] = { keys[i], '\0' };
        jobject si = jni_create_scored_input(
            g_env, text, i == 0 ? TYPED_KEY_SCORE : NEIGHBOR_KEY_SCORE);
        jni_scored_input_set_vertices(si, (int)g_end_vertex,
                                      (int)(g_end_vertex + 1));
        jni_SetObjectArrayElement(g_env, arr, i, si);
    }
    return arr;
}

// True when the keys typed between start - 1 and start + 1 form one
// Shuangpin syllable that may have been typed in the opposite order. Edges
// across syllables never help: decoded tokens cannot split them.
static bool swapped_keys_edge_wanted(int start) {
    int first = candidate_start_vertex();
    if (start < 1 || start >= KEY_HISTORY_MAX || start - 1 < first ||
        (start - first) % 2 == 0) {
        return false;
    }
    char previous = s_typed_keys[start - 1], key = s_typed_keys[start];
    return previous != key && is_shuangpin_key(previous) &&
           is_shuangpin_key(key) &&
           hmm_engine_get_separator(start - s_context_end_vertex) == 0;
}

// Lets the decoder read the keys typed between start - 1 and start + 1 in
// the opposite order. Edges must end at the last vertex.
static void add_swapped_keys_edge(int start) {
    if (!g_addInputEdge || !swapped_keys_edge_wanted(start)) return;
    char previous = s_typed_keys[start - 1], key = s_typed_keys[start];
    char swapped[3] = { key, previous, '\0' };
    jobject si = jni_create_scored_input(g_env, swapped, SWAPPED_KEYS_SCORE);
    jni_scored_input_set_vertices(si, start - 1, start + 1);
    jint result = 0;
    CRASH_PROTECT_BEGIN()
    result = g_addInputEdge(g_env, NULL, g_engine, start - 1, start + 1, si,
                            INPUT_TYPE_SOURCE_INPUT_UNIT);
    CRASH_PROTECT_END("nativeAddInputEdge")
    if (result > 0) {
        s_swap_added[start] = true;
    } else {
        LOGERR("swapped key edge '%s' rejected", swapped);
    }
}

bool hmm_engine_append(const char *pinyin_input) {
    JNI_LOCAL_FRAME();
    LOG("append: input='%s' g_append=%p g_engine=%lld", pinyin_input, (void*)g_append, (long long)g_engine);
    if (!g_append || !g_engine) { LOG("append: SKIP (null)"); return false; }
    if (!s_settings_ready) {
        LOGERR("Cannot append: decoder settings failed to refresh");
        return false;
    }
    if (!append_context()) return false;

    size_t len = strlen(pinyin_input);
    for (size_t i = 0; i < len; i++) {
        char ch[2] = { pinyin_input[i], '\0' };
        int start = (int)g_end_vertex;
        jobject arr;
        if (s_key_correction_active) {
            arr = key_alternatives(ch[0]);
        } else {
            arr = jni_NewObjectArray(g_env, 1, NULL, NULL);
            jobject si = jni_create_scored_input(g_env, ch, 1.0f);
            jni_scored_input_set_vertices(si, start, start + 1);
            jni_SetObjectArrayElement(g_env, arr, 0, si);
        }

        jint result = 0;
        CRASH_PROTECT_BEGIN()
        result = g_append(g_env, NULL, g_engine, arr,
                          INPUT_TYPE_SOURCE_INPUT_UNIT);
        LOGERR("nativeAppend('%c') → %d", pinyin_input[i], result);
        CRASH_PROTECT_END("nativeAppend")
        if (result > 0) {
            g_end_vertex = result;
            if (start < KEY_HISTORY_MAX) {
                s_typed_keys[start] = ch[0];
                s_swap_added[start] = false;
            }
            if (s_key_correction_active && result == start + 1) {
                add_swapped_keys_edge(start);
            }
        } else {
            return i > 0;
        }
    }
    return true;
}

int hmm_engine_get_candidates_page(char **candidates, int offset, int max_count) {
    JNI_LOCAL_FRAME();
    if (!g_engine || !candidates || offset < 0 || max_count < 1) return 0;

    int count = 0;

    if (g_fillCandList) {
        jobject range = jni_create_range(g_env, candidate_start_vertex(),
                                         g_end_vertex);
        CRASH_PROTECT_BEGIN()
        jboolean ok = g_fillCandList(g_env, NULL, g_engine, range);
        LOGERR("fillCandidateList → %d", ok);
        CRASH_PROTECT_END("nativeFillCandidateList")
    }

    if (g_getCandCount) {
        CRASH_PROTECT_BEGIN()
        count = g_getCandCount(g_env, NULL, g_engine);
        LOGERR("getCandidateCount → %d", count);
        CRASH_PROTECT_END("nativeGetCandidateCount")
    }

    if (offset >= count) return 0;
    int end = offset + max_count;
    if (end > count) end = count;

    int filled = 0;
    if (g_getCandString) {
        for (int i = offset; i < end; i++) {
            jstring js = NULL;
            CRASH_PROTECT_BEGIN()
            js = g_getCandString(g_env, NULL, g_engine, (jint)i);
            CRASH_PROTECT_END("nativeGetCandidateString")
            if (js) {
                const char *s = jni_get_string(js);
                if (s && *s) {
                    candidates[filled++] = strdup(s);
                }
            }
        }
    }
    return filled;
}

int hmm_engine_get_candidates(char **candidates, int max_count) {
    JNI_LOCAL_FRAME();
    return hmm_engine_get_candidates_page(candidates, 0, max_count);
}

int hmm_engine_get_predictions(char **candidates, int max_count) {
    JNI_LOCAL_FRAME();
    if (!candidates || max_count < 1 || !g_fillPredictionCandList ||
        !g_getPredictionCandCount || !g_getPredictionCandString) {
        hmm_engine_reset();
        return 0;
    }
    // Mirrors Gboard hbe.g(): inject context as TARGET_TOKEN input, fill
    // predictions, then reset so the next composition starts clean.
    hmm_engine_prepare_input("");
    hmm_engine_reset();
    if (!s_context[0] || !g_engine || !s_settings_ready) return 0;
    append_context();

    int filled = 0;
    if (s_context_end_vertex > 0) {
        jboolean ok = JNI_FALSE;
        CRASH_PROTECT_BEGIN()
        ok = g_fillPredictionCandList(g_env, NULL, g_engine);
        CRASH_PROTECT_END("nativeFillPredictionCandidateList")

        jint count = 0;
        if (ok) {
            CRASH_PROTECT_BEGIN()
            count = g_getPredictionCandCount(g_env, NULL, g_engine);
            CRASH_PROTECT_END("nativeGetPredictionCandidateCount")
        }
        if (count > PREDICTION_MAX_CANDIDATES) {
            count = PREDICTION_MAX_CANDIDATES;
        }
        for (jint i = 0; i < count && filled < max_count; i++) {
            jstring js = NULL;
            CRASH_PROTECT_BEGIN()
            js = g_getPredictionCandString(g_env, NULL, g_engine, i);
            CRASH_PROTECT_END("nativeGetPredictionCandidateString")
            const char *s = js ? jni_get_string(js) : NULL;
            if (!s || !*s) continue;
            bool duplicate = false;
            for (int j = 0; j < filled && !duplicate; j++) {
                duplicate = strcmp(candidates[j], s) == 0;
            }
            if (!duplicate) candidates[filled++] = strdup(s);
        }
        LOG("predictions for '%s' → %d", s_context, filled);
    }

    hmm_engine_reset();
    return filled;
}

int hmm_engine_get_candidate_consumed(int index) {
    JNI_LOCAL_FRAME();
    if (!g_getCandRange || !g_engine) return -1;
    jobject range = NULL;
    CRASH_PROTECT_BEGIN()
    range = g_getCandRange(g_env, NULL, g_engine, (jint)index);
    CRASH_PROTECT_END("nativeGetCandidateRange")
    if (!range) return -1;
    int start_v = 0, end_v = 0;
    jni_get_range(range, &start_v, &end_v);
    LOG("getCandidateRange(%d) → start=%d end=%d (g_end_vertex=%d)", index, start_v, end_v, g_end_vertex);
    int consumed = end_v - s_context_end_vertex;
    return consumed >= 0 ? consumed : -1;
}

int hmm_engine_get_separator(int vertex_index) {
    JNI_LOCAL_FRAME();
    if (!g_getSeparator || !g_engine) return -1;
    jint result = 0;
    CRASH_PROTECT_BEGIN()
    result = g_getSeparator(g_env, NULL, g_engine,
                            (jint)(s_context_end_vertex + vertex_index));
    CRASH_PROTECT_END("nativeGetSeparator")
    return (int)result;
}

bool hmm_engine_set_separator(int vertex_index, int separator_type) {
    JNI_LOCAL_FRAME();
    if (!g_setSeparator || !g_engine) return false;
    jboolean result = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    result = g_setSeparator(g_env, NULL, g_engine,
                            (jint)(s_context_end_vertex + vertex_index),
                            (jint)separator_type);
    CRASH_PROTECT_END("nativeSetSeparator")
    return result == JNI_TRUE;
}

int hmm_engine_get_segmented_pinyin(char *text, int max_bytes) {
    JNI_LOCAL_FRAME();
    if (!text || max_bytes < 1) return 0;
    text[0] = '\0';
    if (!g_getSegmentCount || !g_getSegment || !g_getSegmentRange ||
        !g_getSegmentTokenCount || !g_getSegmentToken || !g_getTokenString) {
        return 0;
    }

    jint segCount = 0;
    CRASH_PROTECT_BEGIN()
    segCount = g_getSegmentCount(g_env, NULL, g_engine);
    CRASH_PROTECT_END("getSegmentCount")

    int written = 0;
    bool has_token = false;
    int prev_language = -1;
    const HmmUserDictNatives *lang_natives = hmm_get_user_dict_natives();
    for (jint s = 0; s < segCount && written < max_bytes - 1; s++) {
        jlong seg = 0;
        CRASH_PROTECT_BEGIN()
        seg = g_getSegment(g_env, NULL, g_engine, s);
        CRASH_PROTECT_END("getSegment")
        if (!seg) continue;

        jobject range = NULL;
        CRASH_PROTECT_BEGIN()
        range = g_getSegmentRange(g_env, NULL, g_engine, seg);
        CRASH_PROTECT_END("getSegmentRange")
        if (!range) continue;

        int start_v = 0, end_v = 0;
        jni_get_range(range, &start_v, &end_v);
        if (start_v < s_context_end_vertex) continue;
        // Without the native check, converted segments end at the
        // last selection.
        if (!g_isSegmentConverted && start_v < candidate_start_vertex()) {
            continue;
        }
        if (g_isSegmentConverted) {
            jboolean converted = JNI_FALSE;
            CRASH_PROTECT_BEGIN()
            converted = g_isSegmentConverted(g_env, NULL, g_engine, seg);
            CRASH_PROTECT_END("nativeIsSegmentConverted")
            if (converted) continue;
        }

        jint tokenCount = 0;
        CRASH_PROTECT_BEGIN()
        tokenCount = g_getSegmentTokenCount(g_env, NULL, g_engine, seg);
        CRASH_PROTECT_END("getSegmentTokenCount")

        for (jint t = 0; t < tokenCount && written < max_bytes - 1; t++) {
            jlong token = 0;
            CRASH_PROTECT_BEGIN()
            token = g_getSegmentToken(g_env, NULL, g_engine, seg, t);
            CRASH_PROTECT_END("getSegmentToken")
            if (!token) continue;

            jstring js = NULL;
            CRASH_PROTECT_BEGIN()
            js = g_getTokenString(g_env, NULL, g_engine, token);
            CRASH_PROTECT_END("nativeGetTokenString")
            const char *token_text = js ? jni_get_string(js) : NULL;
            if (!token_text || !token_text[0]) {
                jni_release_string(js);
                continue;
            }

            /* English tokens are single letters; keep a word contiguous. */
            int language = -1;
            if (lang_natives && lang_natives->getTokenLanguage) {
                CRASH_PROTECT_BEGIN()
                language = lang_natives->getTokenLanguage(g_env, NULL, g_engine, token);
                CRASH_PROTECT_END("nativeGetTokenLanguage")
            }
            bool join = has_token && language == 0 && prev_language == 0;
            prev_language = language;

            if (has_token && !join && written < max_bytes - 1) {
                text[written++] = '\'';
                text[written] = '\0';
            }
            int available = max_bytes - 1 - written;
            int length = (int)strlen(token_text);
            if (length > available) length = available;
            memcpy(text + written, token_text, (size_t)length);
            jni_release_string(js);
            written += length;
            text[written] = '\0';
            has_token = true;
        }
    }
    return written;
}

static void copy_token_text(char *dst, size_t max, jstring js) {
    const char *src = js ? jni_get_string(js) : "";
    size_t length = strlen(src);
    if (length >= max) length = max - 1;
    memcpy(dst, src, length);
    dst[length] = '\0';
    jni_release_string(js);
}

// Copies the keys typed over a token's vertex range; false when unknown.
static bool typed_keys_for_token(jlong token, char *dst, size_t max) {
    if (!g_getTokenRange) return false;
    jobject range = g_getTokenRange(g_env, NULL, g_engine, token);
    if (!range) return false;
    int start = 0, end = 0;
    jni_get_range(range, &start, &end);
    if (start < candidate_start_vertex() || end <= start ||
        end > g_end_vertex || end > KEY_HISTORY_MAX ||
        (size_t)(end - start) >= max) {
        return false;
    }
    memcpy(dst, s_typed_keys + start, (size_t)(end - start));
    dst[end - start] = '\0';
    return true;
}

int hmm_engine_get_corrected_candidate_reading(int index,
                                               HmmTokenReading *tokens,
                                               int max_tokens) {
    JNI_LOCAL_FRAME();
    const HmmUserDictNatives *natives = hmm_get_user_dict_natives();
    if (!tokens || max_tokens < 1 || !g_engine || !g_isCandidateCorrected ||
        !g_getTokenString || !natives || !natives->getCandidateTokenCount ||
        !natives->getCandidateToken) {
        return 0;
    }

    volatile int filled = 0;
    CRASH_PROTECT_BEGIN()
    jint count = 0;
    bool spelling_corrected =
        g_isCandidateCorrected(g_env, NULL, g_engine, (jint)index);
    if (spelling_corrected || s_key_correction_active) {
        count = natives->getCandidateTokenCount(g_env, NULL, g_engine,
                                                (jint)index);
    }
    bool keys_corrected = false;
    if (count > 0 && count <= max_tokens) {
        int i = 0;
        for (; i < count; i++) {
            jlong token = natives->getCandidateToken(g_env, NULL, g_engine,
                                                     (jint)index, (jint)i);
            if (!token) break;
            HmmTokenReading *reading = &tokens[i];
            copy_token_text(reading->raw, sizeof(reading->raw),
                            g_getTokenString(g_env, NULL, g_engine, token));
            if (!reading->raw[0]) break;
            if (g_getTokenNormalizedString) {
                copy_token_text(reading->normalized,
                                sizeof(reading->normalized),
                                g_getTokenNormalizedString(g_env, NULL,
                                                           g_engine, token));
            } else {
                reading->normalized[0] = '\0';
            }
            if (!reading->normalized[0]) {
                memcpy(reading->normalized, reading->raw,
                       sizeof(reading->normalized));
            }
            // A token decoded from neighbour or swapped keys reports the
            // keys it used; show them as a correction of the typed keys.
            char typed[HMM_TOKEN_TEXT_MAX];
            if (s_key_correction_active &&
                typed_keys_for_token(token, typed, sizeof(typed)) &&
                strcmp(typed, reading->raw) != 0) {
                keys_corrected = true;
                if (!spelling_corrected) {
                    memcpy(reading->normalized, reading->raw,
                           sizeof(reading->normalized));
                }
                memcpy(reading->raw, typed, sizeof(reading->raw));
            }
            reading->language = natives->getTokenLanguage
                ? natives->getTokenLanguage(g_env, NULL, g_engine, token)
                : -1;
        }
        if (i == count && (spelling_corrected || keys_corrected)) {
            filled = (int)count;
        }
    }
    CRASH_PROTECT_END("corrected candidate reading")
    return filled;
}

bool hmm_engine_select(int index) {
    JNI_LOCAL_FRAME();
    if (!g_selectCand || !g_engine) return false;
    if (s_converted_count >= MAX_CONVERTED_RANGES) return false;
    int start = 0, end = 0;
    bool has_range = false;
    if (g_getCandRange) {
        jobject range = NULL;
        CRASH_PROTECT_BEGIN()
        range = g_getCandRange(g_env, NULL, g_engine, (jint)index);
        CRASH_PROTECT_END("nativeGetCandidateRange(select)")
        if (range) {
            jni_get_range(range, &start, &end);
            has_range = end > start;
        }
    }
    jboolean result = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    result = g_selectCand(g_env, NULL, g_engine, (jint)index);
    CRASH_PROTECT_END("nativeSelectCandidate")
    if (result && has_range) {
        s_converted[s_converted_count++] = (VertexRange){ start, end };
    }
    return (bool)result;
}

bool hmm_engine_delete_last_input(void) {
    JNI_LOCAL_FRAME();
    if (!g_deleteInput || !g_engine) return false;
    if (g_end_vertex <= candidate_start_vertex()) return false;
    jobject range = jni_create_range(g_env, (int)g_end_vertex - 1,
                                     (int)g_end_vertex);
    jboolean result = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    result = g_deleteInput(g_env, NULL, g_engine, range);
    CRASH_PROTECT_END("nativeDelete")
    if (!result) return false;
    g_end_vertex--;
    return true;
}

bool hmm_engine_input_edges_stale(void) {
    if (!s_key_correction_active) return false;
    int end = g_end_vertex < KEY_HISTORY_MAX ? g_end_vertex : KEY_HISTORY_MAX;
    for (int v = candidate_start_vertex() + 1; v < end; v++) {
        if (swapped_keys_edge_wanted(v) && !s_swap_added[v]) return true;
    }
    return false;
}

uint64_t hmm_engine_input_generation(void) {
    return s_input_generation;
}

bool hmm_engine_can_edit_in_place(uint64_t generation, int letters,
                                  int next_letters) {
    return g_engine && generation == s_input_generation &&
           key_correction_applies((size_t)next_letters) ==
               s_key_correction_active &&
           s_settings_ready && s_converted_count == 0 &&
           s_context_injected && letters >= 0 && next_letters >= 0 &&
           g_end_vertex - s_context_end_vertex == letters &&
           hmm_engine_decoder_for_input_length((size_t)next_letters) ==
               g_engine;
}

int hmm_engine_get_converted_count(void) {
    return s_converted_count;
}

bool hmm_engine_unconvert_last(void) {
    JNI_LOCAL_FRAME();
    if (s_converted_count == 0 || !g_unconvertSegments || !g_engine) {
        return false;
    }
    VertexRange last = s_converted[s_converted_count - 1];
    jobject range = jni_create_range(g_env, last.start, last.end);
    jboolean result = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    result = g_unconvertSegments(g_env, NULL, g_engine, range);
    CRASH_PROTECT_END("nativeUnconvertSegments")
    if (!result) return false;
    s_converted_count--;
    return true;
}

void hmm_engine_reset(void) {
    JNI_LOCAL_FRAME();
    s_input_generation++;
    g_end_vertex = 0;
    s_converted_count = 0;
    s_context_injected = false;
    s_context_end_vertex = 0;
    volatile bool reset_ok = false;
    if (g_reset && g_engine) {
        CRASH_PROTECT_BEGIN()
        g_reset(g_env, NULL, g_engine);
        reset_ok = true;
        CRASH_PROTECT_END("nativeReset")
    }
    s_settings_ready = reset_ok && hmm_engine_refresh_settings_after_reset();
    HmmPinyinScheme scheme = hmm_engine_get_pinyin_scheme();
    s_key_correction_active =
        key_correction_applies(hmm_engine_prepared_input_length());
    s_semicolon_is_key = scheme == HMM_PINYIN_SHUANGPIN_MS ||
                         scheme == HMM_PINYIN_SHUANGPIN_ZIGUANG;
    hmm_user_dict_refresh_decoder_if_needed();
}
