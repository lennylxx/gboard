// Input append, candidate retrieval, selection, and reset.
#include "hmm_internal.h"

bool hmm_engine_append(const char *pinyin_input) {
    LOG("append: input='%s' g_append=%p g_engine=%lld", pinyin_input, (void*)g_append, (long long)g_engine);
    if (!g_append || !g_engine) { LOG("append: SKIP (null)"); return false; }

    size_t len = strlen(pinyin_input);
    for (size_t i = 0; i < len; i++) {
        char ch[2] = { pinyin_input[i], '\0' };
        jobject arr = jni_NewObjectArray(g_env, 1, NULL, NULL);
        jobject si = jni_create_scored_input(g_env, ch, 1.0f);
        jni_scored_input_set_vertices(si, (int)g_end_vertex, (int)(g_end_vertex + 1));
        jni_SetObjectArrayElement(g_env, arr, 0, si);

        jint result = 0;
        CRASH_PROTECT_BEGIN()
        result = g_append(g_env, NULL, g_engine, arr, 0);
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
        jobject range = jni_create_range(g_env, 0, g_end_vertex);
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
    return end_v;
}

int hmm_engine_get_separator(int vertex_index) {
    if (!g_getSeparator || !g_engine) return -1;
    jint result = 0;
    CRASH_PROTECT_BEGIN()
    result = g_getSeparator(g_env, NULL, g_engine, (jint)vertex_index);
    CRASH_PROTECT_END("nativeGetSeparator")
    return (int)result;
}

bool hmm_engine_set_separator(int vertex_index, int separator_type) {
    if (!g_setSeparator || !g_engine) return false;
    jboolean result = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    result = g_setSeparator(g_env, NULL, g_engine, (jint)vertex_index, (jint)separator_type);
    CRASH_PROTECT_END("nativeSetSeparator")
    return result == JNI_TRUE;
}

int hmm_engine_get_syllable_breaks(int *breaks, int max_breaks) {
    if (!breaks || max_breaks < 1) return 0;
    if (!g_getSegmentCount || !g_getSegment || !g_getSegmentTokenCount || !g_getSegmentToken)
        return 0;

    jint segCount = 0;
    CRASH_PROTECT_BEGIN()
    segCount = g_getSegmentCount(g_env, NULL, g_engine);
    CRASH_PROTECT_END("getSegmentCount")

    int n = 0;
    for (jint s = 0; s < segCount && n < max_breaks; s++) {
        jlong seg = 0;
        CRASH_PROTECT_BEGIN()
        seg = g_getSegment(g_env, NULL, g_engine, s);
        CRASH_PROTECT_END("getSegment")
        if (!seg) continue;

        jint tokenCount = 0;
        CRASH_PROTECT_BEGIN()
        tokenCount = g_getSegmentTokenCount(g_env, NULL, g_engine, seg);
        CRASH_PROTECT_END("getSegmentTokenCount")

        for (jint t = 0; t < tokenCount && n < max_breaks; t++) {
            jlong token = 0;
            CRASH_PROTECT_BEGIN()
            token = g_getSegmentToken(g_env, NULL, g_engine, seg, t);
            CRASH_PROTECT_END("getSegmentToken")
            if (!token) continue;

            // Token layout: offset 0x18 = start_vertex (int32), 0x1C = end_vertex (int32)
            int start_v = *(int *)((uint8_t *)token + 0x18);
            int end_v   = *(int *)((uint8_t *)token + 0x1C);

            // Record the end vertex as a syllable break (skip the last one = end of input)
            if (end_v > start_v && end_v < g_end_vertex && n < max_breaks) {
                breaks[n++] = end_v;
            }
        }
    }
    return n;
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
    if (g_reset && g_engine) {
        CRASH_PROTECT_BEGIN()
        g_reset(g_env, NULL, g_engine);
        CRASH_PROTECT_END("nativeReset")
    }
}
