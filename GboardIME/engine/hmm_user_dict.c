// User dictionary (user_dict_3_3 / user_dict_3_3_english) learning implementation.
// Wraps MutableDictionaryAccessorImpl native JNI calls.
#include "hmm_internal.h"
#include "hmm_user_dict.h"

#include <sys/stat.h>
#include <errno.h>
#include <stdatomic.h>

// ── Static state ────────────────────────────────────────────────────────────

typedef struct {
    const char *filename;       // data id and persisted file name
    const char *accessor_name;
    const char *scheme_file;
    jlong accessor;
} UserDict;

// Gboard routes all-English selections to en_user_dictionary_accessor
// (user_dict_3_3_english) and Chinese or mixed selections to the Pinyin one.
static UserDict s_dicts[] = {
    {"user_dict_3_3", "user_dictionary_accessor_for_ime",
     "pinyin_mutable_dictionary_accessor_setting_scheme", 0},
    {"user_dict_3_3_english", "en_user_dictionary_accessor",
     "en_mutable_dictionary_accessor_setting_scheme", 0},
};
#define PINYIN_DICT  (&s_dicts[0])
#define ENGLISH_DICT (&s_dicts[1])
#define DICT_COUNT   ((int)(sizeof(s_dicts) / sizeof(s_dicts[0])))

#define ENGLISH_TOKEN_LANGUAGE 0
// Routes to the Pinyin dictionary when the token language is unavailable.
#define UNKNOWN_TOKEN_LANGUAGE (-1)

static const HmmUserDictNatives *s_natives = NULL;
static bool  s_ready = false;
static atomic_bool s_decoder_refresh_pending = false;
static char  s_user_data_dir[4096] = {0};
static char  s_pack_dir[4096] = {0};

#define COMPACT_TARGET 450000

// ── Helpers ─────────────────────────────────────────────────────────────────

static bool ensure_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static void build_path(char *out, size_t out_sz, const UserDict *dict,
                       const char *suffix) {
    snprintf(out, out_sz, "%s/%s%s", s_user_data_dir, dict->filename, suffix);
}

// Enroll the setting scheme for the accessor name (required for data model)
static void enroll_accessor_setting_scheme(const UserDict *dict) {
    if (!g_enrollSettingScheme || !g_sm || !s_pack_dir[0]) return;
    char mpath[4096];
    snprintf(mpath, sizeof(mpath), "%s/%s", s_pack_dir, dict->scheme_file);
    FILE *mfp = fopen(mpath, "rb");
    if (!mfp) return;
    fseek(mfp, 0, SEEK_END);
    long msz = ftell(mfp);
    fseek(mfp, 0, SEEK_SET);
    uint8_t *mbuf = malloc((size_t)msz);
    fread(mbuf, 1, (size_t)msz, mfp);
    fclose(mfp);
    jbyteArray mba = jni_NewByteArray(g_env, (jsize)msz);
    jni_SetByteArrayRegion(g_env, mba, 0, (jsize)msz, (jbyte*)mbuf);
    free(mbuf);
    jstring macc = jni_NewStringUTF(g_env, dict->accessor_name);
    jstring mloc = jni_NewStringUTF(g_env, "");
    CRASH_PROTECT_BEGIN()
    g_enrollSettingScheme(g_env, NULL, g_sm, macc, mloc, mba);
    CRASH_PROTECT_END("enrollSettingScheme(accessor)")
}

static bool enroll_dictionary_file(const UserDict *dict, const char *path) {
    if (!s_natives->enrollMutableDictFd || !g_dm) return false;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) return false;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    jobject jfd = jni_create_file_descriptor(g_env, fd);
    jboolean ok = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    ok = s_natives->enrollMutableDictFd(
        g_env, NULL, g_dm, jni_NewStringUTF(g_env, dict->filename),
        (jint)23, jfd, 0, (jint)st.st_size, (jint)0);
    CRASH_PROTECT_END("nativeEnrollMutableDictFd")
    close(fd);
    return ok;
}

static bool create_accessor(UserDict *dict) {
    jstring jname = jni_NewStringUTF(g_env, dict->accessor_name);
    jstring jlocale = jni_NewStringUTF(g_env, "");
    jstring jtype = jni_NewStringUTF(g_env, dict->filename);
    CRASH_PROTECT_BEGIN()
    dict->accessor = s_natives->createAccessor(
        g_env, NULL, g_factory, jname, jlocale, jtype);
    CRASH_PROTECT_END("nativeCreateMutableDictionaryAccessor")
    return dict->accessor != 0;
}

static void init_dictionary(UserDict *dict) {
    // The accessor binds to the enrolled data, so load saved entries first.
    bool loaded = false;
    if (s_user_data_dir[0]) {
        char dict_path[4096];
        build_path(dict_path, sizeof(dict_path), dict, "");
        loaded = enroll_dictionary_file(dict, dict_path);
    }
    enroll_accessor_setting_scheme(dict);
    if (create_accessor(dict)) return;

    LOGERR("user dictionary accessor '%s' unavailable", dict->accessor_name);
    // Without an accessor the entries cannot be persisted or cleared, so do
    // not let them keep influencing ranking.
    if (loaded && g_enrollEmptyMutableDict && g_dm) {
        CRASH_PROTECT_BEGIN()
        g_enrollEmptyMutableDict(g_env, NULL, g_dm,
                                 jni_NewStringUTF(g_env, dict->filename),
                                 (jint)23, (jint)0);
        CRASH_PROTECT_END("nativeEnrollEmptyMutableDict(unload)")
    }
}

static UserDict *dictionary_for_types(const int *token_types, int token_count) {
    for (int i = 0; i < token_count; ++i) {
        if (token_types[i] != ENGLISH_TOKEN_LANGUAGE) return PINYIN_DICT;
    }
    return ENGLISH_DICT;
}

// ── Init ────────────────────────────────────────────────────────────────────

bool hmm_user_dict_init(const char *user_data_dir, const char *pack_dir) {
    if (s_ready) return true;

    s_natives = hmm_get_user_dict_natives();
    if (!s_natives->createAccessor || !g_factory) return false;

    if (pack_dir && pack_dir[0])
        snprintf(s_pack_dir, sizeof(s_pack_dir), "%s", pack_dir);
    if (user_data_dir && user_data_dir[0]) {
        snprintf(s_user_data_dir, sizeof(s_user_data_dir), "%s", user_data_dir);
        ensure_directory(s_user_data_dir);
    }

    for (int i = 0; i < DICT_COUNT; ++i) init_dictionary(&s_dicts[i]);

    // The Pinyin dictionary is required; the English one is optional.
    if (!PINYIN_DICT->accessor) {
        for (int i = 0; i < DICT_COUNT; ++i) {
            if (s_natives->closeAccessor && s_dicts[i].accessor) {
                CRASH_PROTECT_BEGIN()
                s_natives->closeAccessor(g_env, NULL, s_dicts[i].accessor);
                CRASH_PROTECT_END("nativeClose(init failure)")
            }
            s_dicts[i].accessor = 0;
        }
        return false;
    }
    s_ready = true;
    return true;
}

// ── Learn / unlearn ─────────────────────────────────────────────────────────

static bool build_token_arrays(const char **tokens, const int *token_types,
                               int token_count, jobject *token_array,
                               jintArray *type_array) {
    *token_array = jni_NewObjectArray(g_env, token_count, NULL, NULL);
    *type_array = jni_NewIntArray(g_env, token_count);
    if (!*token_array || !*type_array) return false;
    jint *types = malloc((size_t)token_count * sizeof(*types));
    if (!types) return false;
    for (int i = 0; i < token_count; ++i) {
        if (!tokens[i] || !tokens[i][0]) {
            free(types);
            return false;
        }
        jni_SetObjectArrayElement(g_env, *token_array, i,
                                  jni_NewStringUTF(g_env, tokens[i]));
        types[i] = token_types[i];
    }
    jni_SetIntArrayRegion(g_env, *type_array, 0, token_count, types);
    free(types);
    return true;
}

bool hmm_user_dict_learn(const char **tokens, const int *token_types,
                         int token_count, const char *value,
                         bool is_full_match) {
    if (!s_ready || !s_natives->addCount ||
        !tokens || !token_types ||
        token_count <= 0 || !value || !value[0]) {
        return false;
    }
    UserDict *dict = dictionary_for_types(token_types, token_count);
    if (!dict->accessor) return false;

    jobject token_array;
    jintArray type_array;
    if (!build_token_arrays(tokens, token_types, token_count,
                            &token_array, &type_array)) {
        return false;
    }
    jstring jvalue = jni_NewStringUTF(g_env, value);
    jboolean result = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    result = s_natives->addCount(
        g_env, NULL, dict->accessor, token_array, type_array, jvalue, (jint)1,
        is_full_match ? (jboolean)JNI_TRUE : (jboolean)JNI_FALSE);
    CRASH_PROTECT_END("nativeAddCount")
    return (bool)result;
}

bool hmm_user_dict_unlearn(const char **tokens, const int *token_types,
                           int token_count, const char *value) {
    if (!s_ready || !s_natives->decreaseCount ||
        !tokens || !token_types ||
        token_count <= 0 || !value || !value[0]) {
        return false;
    }
    UserDict *dict = dictionary_for_types(token_types, token_count);
    if (!dict->accessor) return false;

    jobject token_array;
    jintArray type_array;
    if (!build_token_arrays(tokens, token_types, token_count,
                            &token_array, &type_array)) {
        return false;
    }
    jboolean result = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    result = s_natives->decreaseCount(
        g_env, NULL, dict->accessor, token_array, type_array,
        jni_NewStringUTF(g_env, value), (jint)1);
    CRASH_PROTECT_END("nativeDecreaseCount")
    return (bool)result;
}

// ── Persistence ─────────────────────────────────────────────────────────────

static bool persist_dictionary(const UserDict *dict) {
    char primary[4096], tmp_path[4096], bak_path[4096];
    build_path(primary, sizeof(primary), dict, "");
    build_path(tmp_path, sizeof(tmp_path), dict, "_tmp");
    build_path(bak_path, sizeof(bak_path), dict, "_bak");

    // 1. Remove stale tmp
    if (access(tmp_path, F_OK) == 0 && unlink(tmp_path) != 0) return false;

    // 2. Duplicate dictionary
    if (s_natives->duplicateDictionary) {
        jboolean duplicate_ok = JNI_FALSE;
        CRASH_PROTECT_BEGIN()
        duplicate_ok = s_natives->duplicateDictionary(
            g_env, NULL, dict->accessor);
        CRASH_PROTECT_END("nativeDuplicateDictionary")
        if (!duplicate_ok) return false;
    }

    // 3. Compact (ignore return per Android behavior)
    if (s_natives->compact) {
        CRASH_PROTECT_BEGIN()
        s_natives->compact(g_env, NULL, dict->accessor, (jint)COMPACT_TARGET);
        CRASH_PROTECT_END("nativeCompact")
    }

    // 4. Persist to tmp
    jstring jtmp = jni_NewStringUTF(g_env, tmp_path);
    jboolean persist_ok = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    persist_ok = s_natives->persist(g_env, NULL, dict->accessor, jtmp);
    CRASH_PROTECT_END("nativePersist")
    if (!persist_ok) { unlink(tmp_path); return false; }

    // 5. Delete old bak
    if (access(bak_path, F_OK) == 0 && unlink(bak_path) != 0) {
        unlink(tmp_path); return false;
    }

    // 6. Rename primary → bak
    bool had_primary = (access(primary, F_OK) == 0);
    if (had_primary && rename(primary, bak_path) != 0) {
        unlink(tmp_path); return false;
    }

    // 7. Rename tmp → primary
    if (rename(tmp_path, primary) != 0) {
        if (had_primary && access(primary, F_OK) != 0)
            rename(bak_path, primary);
        unlink(tmp_path);
        return false;
    }

    // 8. Delete bak (non-fatal)
    if (access(bak_path, F_OK) == 0) unlink(bak_path);

    // 9. Re-enroll the saved snapshot; the decoder refreshes afterwards.
    return enroll_dictionary_file(dict, primary);
}

bool hmm_user_dict_persist(void) {
    if (!s_ready || !s_user_data_dir[0] || !s_natives->persist) {
        return false;
    }

    bool ok = true;
    bool any_enrolled = false;
    for (int i = 0; i < DICT_COUNT; ++i) {
        if (!s_dicts[i].accessor) continue;
        if (persist_dictionary(&s_dicts[i])) {
            any_enrolled = true;
        } else {
            ok = false;
        }
    }
    if (any_enrolled) atomic_store(&s_decoder_refresh_pending, true);
    return ok;
}

void hmm_user_dict_refresh_decoder_if_needed(void) {
    if (!atomic_load(&s_decoder_refresh_pending) || !PINYIN_DICT->accessor) {
        return;
    }
    if (!hmm_engine_refresh_user_dictionary()) return;
    if (s_natives->refreshData) {
        for (int i = 0; i < DICT_COUNT; ++i) {
            if (!s_dicts[i].accessor) continue;
            CRASH_PROTECT_BEGIN()
            s_natives->refreshData(g_env, NULL, s_dicts[i].accessor);
            CRASH_PROTECT_END("MutableDictionaryAccessor nativeRefreshData")
        }
    }
    atomic_store(&s_decoder_refresh_pending, false);
}

// ── Query ───────────────────────────────────────────────────────────────────

int hmm_user_dict_get_size(void) {
    if (!s_ready || !s_natives->getDictionarySize) return 0;
    int total = 0;
    for (int i = 0; i < DICT_COUNT; ++i) {
        if (!s_dicts[i].accessor) continue;
        jint size = 0;
        CRASH_PROTECT_BEGIN()
        size = s_natives->getDictionarySize(g_env, NULL, s_dicts[i].accessor);
        CRASH_PROTECT_END("nativeGetDictionarySize")
        if (size > 0) total += (int)size;
    }
    return total;
}

// ── Clear ───────────────────────────────────────────────────────────────────

bool hmm_user_dict_clear(void) {
    if (!s_ready || !s_natives->newEmptyDictionary) return false;

    // Clear every dictionary before persisting so a single failure does not
    // leave an already-cleared dictionary unsaved.
    bool all_cleared = true;
    for (int i = 0; i < DICT_COUNT; ++i) {
        if (!s_dicts[i].accessor) continue;
        jboolean cleared = JNI_FALSE;
        CRASH_PROTECT_BEGIN()
        cleared = s_natives->newEmptyDictionary(g_env, NULL,
                                                s_dicts[i].accessor);
        CRASH_PROTECT_END("nativeNewEmptyDictionary(clear)")
        if (!cleared) all_cleared = false;
    }

    if (s_user_data_dir[0]) {
        bool persisted = hmm_user_dict_persist();
        return all_cleared && persisted;
    }
    atomic_store(&s_decoder_refresh_pending, true);
    return all_cleared;
}

// ── Token extraction ────────────────────────────────────────────────────────

int hmm_user_dict_extract_tokens(int candidate_index,
                                 char tokens[][16], int *types, int max_tokens) {
    if (!s_natives ||
        !s_natives->getCandidateTokenCount ||
        !s_natives->getCandidateToken || !g_engine) {
        return 0;
    }
    if (!tokens || !types || max_tokens <= 0) return 0;

    jint count = 0;
    CRASH_PROTECT_BEGIN()
    count = s_natives->getCandidateTokenCount(
        g_env, NULL, g_engine, (jint)candidate_index);
    CRASH_PROTECT_END("nativeGetCandidateTokenCount")
    // A truncated token list would pair the full value with partial
    // readings, so refuse to learn instead.
    if (count <= 0 || count > max_tokens) return 0;

    int filled = 0;
    for (jint i = 0; i < count; i++) {
        jlong token = 0;
        CRASH_PROTECT_BEGIN()
        token = s_natives->getCandidateToken(
            g_env, NULL, g_engine, (jint)candidate_index, i);
        CRASH_PROTECT_END("nativeGetCandidateToken")
        if (!token) return 0;

        if (g_getTokenString) {
            jstring js = NULL;
            CRASH_PROTECT_BEGIN()
            js = g_getTokenString(g_env, NULL, g_engine, token);
            CRASH_PROTECT_END("nativeGetTokenString")
            const char *s = js ? jni_get_string(js) : "";
            strncpy(tokens[filled], s && s[0] ? s : "", 15);
            tokens[filled][15] = '\0';
        } else {
            tokens[filled][0] = '\0';
        }

        if (s_natives->getTokenLanguage) {
            jint t = 0;
            CRASH_PROTECT_BEGIN()
            t = s_natives->getTokenLanguage(g_env, NULL, g_engine, token);
            CRASH_PROTECT_END("nativeGetTokenLanguage")
            types[filled] = (int)t;
        } else {
            types[filled] = UNKNOWN_TOKEN_LANGUAGE;
        }
        filled++;
    }
    return filled;
}

// ── Teardown ────────────────────────────────────────────────────────────────

void hmm_user_dict_destroy(void) {
    if (!s_ready) return;
    if (s_user_data_dir[0]) hmm_user_dict_persist();
    for (int i = 0; i < DICT_COUNT; ++i) {
        if (s_natives->closeAccessor && s_dicts[i].accessor) {
            CRASH_PROTECT_BEGIN()
            s_natives->closeAccessor(g_env, NULL, s_dicts[i].accessor);
            CRASH_PROTECT_END("nativeClose(destroy)")
        }
        s_dicts[i].accessor = 0;
    }
    s_natives = NULL;
    s_ready = false;
    atomic_store(&s_decoder_refresh_pending, false);
    s_user_data_dir[0] = '\0';
    s_pack_dir[0] = '\0';
}

bool hmm_user_dict_is_ready(void) { return s_ready; }
