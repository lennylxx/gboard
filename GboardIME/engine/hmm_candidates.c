// Input append, candidate retrieval, selection, and reset.
#include "hmm_internal.h"
#include "hmm_user_dict.h"

enum {
    INPUT_TYPE_SOURCE_INPUT_UNIT = 0,
    INPUT_TYPE_TARGET_TOKEN = 2,
    SEPARATOR_TOKEN = 1,
    SEPARATOR_SEGMENT = 2,
    END_VERTEX_SENTINEL = 32767,
    CONTEXT_MAX_UTF8_BYTES = 256
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

bool hmm_engine_append(const char *pinyin_input) {
    LOG("append: input='%s' g_append=%p g_engine=%lld", pinyin_input, (void*)g_append, (long long)g_engine);
    if (!g_append || !g_engine) { LOG("append: SKIP (null)"); return false; }
    if (!append_context()) return false;

    size_t len = strlen(pinyin_input);
    for (size_t i = 0; i < len; i++) {
        char ch[2] = { pinyin_input[i], '\0' };
        jobject arr = jni_NewObjectArray(g_env, 1, NULL, NULL);
        jobject si = jni_create_scored_input(g_env, ch, 1.0f);
        jni_scored_input_set_vertices(si, (int)g_end_vertex, (int)(g_end_vertex + 1));
        jni_SetObjectArrayElement(g_env, arr, 0, si);

        jint result = 0;
        CRASH_PROTECT_BEGIN()
        result = g_append(g_env, NULL, g_engine, arr,
                          INPUT_TYPE_SOURCE_INPUT_UNIT);
        LOGERR("nativeAppend('%c') → %d", pinyin_input[i], result);
        CRASH_PROTECT_END("nativeAppend")
        if (result > 0) {
            g_end_vertex = result;
        } else {
            return i > 0;
        }
    }
    return true;
}

int hmm_engine_get_candidates_page(char **candidates, int offset, int max_count) {
    if (!g_engine || !candidates || offset < 0 || max_count < 1) return 0;

    int count = 0;

    if (g_fillCandList) {
        jobject range = jni_create_range(g_env, s_context_end_vertex,
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
    return hmm_engine_get_candidates_page(candidates, 0, max_count);
}

int hmm_engine_get_candidate_consumed(int index) {
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
    if (!g_getSeparator || !g_engine) return -1;
    jint result = 0;
    CRASH_PROTECT_BEGIN()
    result = g_getSeparator(g_env, NULL, g_engine,
                            (jint)(s_context_end_vertex + vertex_index));
    CRASH_PROTECT_END("nativeGetSeparator")
    return (int)result;
}

bool hmm_engine_set_separator(int vertex_index, int separator_type) {
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
            if (!token_text || !token_text[0]) continue;

            if (has_token && written < max_bytes - 1) {
                text[written++] = '\'';
                text[written] = '\0';
            }
            int available = max_bytes - 1 - written;
            int length = (int)strlen(token_text);
            if (length > available) length = available;
            memcpy(text + written, token_text, (size_t)length);
            written += length;
            text[written] = '\0';
            has_token = true;
        }
    }
    return written;
}

bool hmm_engine_select(int index) {
    if (!g_selectCand || !g_engine) return false;
    jboolean result = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    result = g_selectCand(g_env, NULL, g_engine, (jint)index);
    CRASH_PROTECT_END("nativeSelectCandidate")
    return (bool)result;
}

void hmm_engine_reset(void) {
    g_end_vertex = 0;
    s_context_injected = false;
    s_context_end_vertex = 0;
    if (g_reset && g_engine) {
        CRASH_PROTECT_BEGIN()
        g_reset(g_env, NULL, g_engine);
        CRASH_PROTECT_END("nativeReset")
    }
    hmm_user_dict_refresh_decoder_if_needed();
}
