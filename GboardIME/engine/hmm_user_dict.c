// User dictionary (user_dict_3_3) automatic learning implementation.
// Wraps MutableDictionaryAccessorImpl native JNI calls.
#include "hmm_internal.h"
#include "hmm_user_dict.h"

#include <sys/stat.h>
#include <errno.h>
#include <stdatomic.h>

// ── JNI function pointer types ──────────────────────────────────────────────

typedef jlong    (*fn_CreateAccessor)(JNIEnv *, jclass, jlong, jstring, jstring, jstring);
typedef jboolean (*fn_AddCount)(JNIEnv *, jclass, jlong, jobject, jintArray,
                                jstring, jint, jboolean);
typedef jboolean (*fn_DecreaseCount)(JNIEnv *, jclass, jlong, jobject, jintArray,
                                     jstring, jint);
typedef jboolean (*fn_DuplicateDict)(JNIEnv *, jclass, jlong);
typedef jboolean (*fn_Compact)(JNIEnv *, jclass, jlong, jint);
typedef jint     (*fn_GetDictSize)(JNIEnv *, jclass, jlong);
typedef jboolean (*fn_Persist)(JNIEnv *, jclass, jlong, jstring);
typedef void     (*fn_RefreshAccessor)(JNIEnv *, jclass, jlong);
typedef void     (*fn_CloseAccessor)(JNIEnv *, jclass, jlong);
typedef jboolean (*fn_NewEmptyDict)(JNIEnv *, jclass, jlong);
typedef jboolean (*fn_EnrollMutableDictFd)(JNIEnv *, jclass, jlong, jstring, jint, jobject, jint, jint, jint);
typedef jint     (*fn_GetCandTokenCount)(JNIEnv *, jobject, jlong, jint);
typedef jlong    (*fn_GetCandToken)(JNIEnv *, jobject, jlong, jint, jint);
typedef jstring  (*fn_GetTokenString)(JNIEnv *, jobject, jlong, jlong);
typedef jint     (*fn_GetTokenLanguage)(JNIEnv *, jobject, jlong, jlong);

// ── Static state ────────────────────────────────────────────────────────────

static fn_CreateAccessor       s_createAccessor = NULL;
static fn_AddCount             s_addCount = NULL;
static fn_DecreaseCount        s_decreaseCount = NULL;
static fn_DuplicateDict        s_duplicateDict  = NULL;
static fn_Compact              s_compact        = NULL;
static fn_GetDictSize          s_getDictSize    = NULL;
static fn_Persist              s_persist        = NULL;
static fn_RefreshAccessor      s_refreshAccessor = NULL;
static fn_CloseAccessor        s_closeAccessor  = NULL;
static fn_NewEmptyDict         s_newEmptyDict   = NULL;
static fn_EnrollMutableDictFd  s_enrollMutableDictFd = NULL;
static fn_GetCandTokenCount    s_getCandTokenCount = NULL;
static fn_GetCandToken         s_getCandToken      = NULL;
static fn_GetTokenString       s_getTokenString    = NULL;
static fn_GetTokenLanguage     s_getTokenLanguage  = NULL;

static jlong s_accessor = 0;
static bool  s_ready = false;
static atomic_bool s_decoder_refresh_pending = false;
static char  s_user_data_dir[4096] = {0};
static char  s_pack_dir[4096] = {0};
static const char *DICT_FILENAME = "user_dict_3_3";

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

static void build_path(char *out, size_t out_sz, const char *suffix) {
    snprintf(out, out_sz, "%s/%s%s", s_user_data_dir, DICT_FILENAME, suffix);
}

// Enroll the setting scheme for the accessor name (required for data model)
static void enroll_accessor_setting_scheme(void) {
    if (!g_enrollSettingScheme || !g_sm || !s_pack_dir[0]) return;
    char mpath[4096];
    snprintf(mpath, sizeof(mpath), "%s/pinyin_mutable_dictionary_accessor_setting_scheme", s_pack_dir);
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
    jstring macc = jni_NewStringUTF(g_env, "user_dictionary_accessor_for_ime");
    jstring mloc = jni_NewStringUTF(g_env, "");
    CRASH_PROTECT_BEGIN()
    g_enrollSettingScheme(g_env, NULL, g_sm, macc, mloc, mba);
    CRASH_PROTECT_END("enrollSettingScheme(accessor)")
}

// Create (or re-create) the accessor with proper initialization
static bool create_accessor(bool loaded_from_file) {
    (void)loaded_from_file;
    jstring jname = jni_NewStringUTF(g_env, "user_dictionary_accessor_for_ime");
    jstring jlocale = jni_NewStringUTF(g_env, "");
    jstring jtype = jni_NewStringUTF(g_env, "user_dict_3_3");
    CRASH_PROTECT_BEGIN()
    s_accessor = s_createAccessor(g_env, NULL, g_factory, jname, jlocale, jtype);
    CRASH_PROTECT_END("nativeCreateMutableDictionaryAccessor")
    if (!s_accessor) return false;
    return true;
}

// ── Init ────────────────────────────────────────────────────────────────────

bool hmm_user_dict_init(const char *user_data_dir, const char *pack_dir) {
    if (s_ready) return true;

    // Resolve native functions
    s_createAccessor = (fn_CreateAccessor)
        jni_find_registered_native_by_sig("nativeCreateMutableDictionaryAccessor",
            "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)J");
    s_addCount = (fn_AddCount)
        jni_find_registered_native_by_sig("nativeAddCount",
            "(J[Ljava/lang/String;[ILjava/lang/String;IZ)Z");
    s_decreaseCount = (fn_DecreaseCount)
        jni_find_registered_native_by_sig("nativeDecreaseCount",
            "(J[Ljava/lang/String;[ILjava/lang/String;I)Z");
    s_duplicateDict = (fn_DuplicateDict)
        jni_find_registered_native_by_sig("nativeDuplicateDictionary", "(J)Z");
    s_compact = (fn_Compact)
        jni_find_registered_native_by_sig("nativeCompact", "(JI)Z");
    s_getDictSize = (fn_GetDictSize)
        jni_find_registered_native_by_sig("nativeGetDictionarySize", "(J)I");
    s_persist = (fn_Persist)
        jni_find_registered_native_by_sig("nativePersist", "(JLjava/lang/String;)Z");
    s_refreshAccessor = (fn_RefreshAccessor)
        jni_find_registered_native_by_sig("nativeRefreshData", "(J)V");
    s_closeAccessor = (fn_CloseAccessor)
        jni_find_registered_native_by_sig("nativeClose", "(J)V");
    s_newEmptyDict = (fn_NewEmptyDict)
        jni_find_registered_native_by_sig("nativeNewEmptyDictionary", "(J)Z");
    s_enrollMutableDictFd = (fn_EnrollMutableDictFd)
        jni_find_registered_native_by_sig("nativeEnrollMutableDictFd",
            "(JLjava/lang/String;ILjava/io/FileDescriptor;III)Z");
    s_getCandTokenCount = (fn_GetCandTokenCount)
        jni_find_registered_native_by_sig("nativeGetCandidateTokenCount", "(JI)I");
    s_getCandToken = (fn_GetCandToken)
        jni_find_registered_native_by_sig("nativeGetCandidateToken", "(JII)J");
    s_getTokenString = (fn_GetTokenString)
        jni_find_registered_native_by_sig("nativeGetTokenString", "(JJ)");
    s_getTokenLanguage = (fn_GetTokenLanguage)
        jni_find_registered_native_by_sig("nativeGetTokenLanguage", "(JJ)I");

    if (!s_createAccessor || !g_factory) return false;

    // Store paths
    if (pack_dir && pack_dir[0])
        snprintf(s_pack_dir, sizeof(s_pack_dir), "%s", pack_dir);
    if (user_data_dir && user_data_dir[0]) {
        snprintf(s_user_data_dir, sizeof(s_user_data_dir), "%s", user_data_dir);
        ensure_directory(s_user_data_dir);
    }

    // Load persisted dictionary if available
    bool loaded_from_file = false;
    if (s_user_data_dir[0] && s_enrollMutableDictFd && g_dm) {
        char dict_path[4096];
        build_path(dict_path, sizeof(dict_path), "");
        struct stat st;
        if (stat(dict_path, &st) == 0 && st.st_size > 0) {
            int fd = open(dict_path, O_RDONLY);
            if (fd >= 0) {
                jstring jid = jni_NewStringUTF(g_env, DICT_FILENAME);
                jobject jfd = jni_create_file_descriptor(g_env, fd);
                CRASH_PROTECT_BEGIN()
                jboolean ok = s_enrollMutableDictFd(g_env, NULL, g_dm, jid,
                    (jint)23, jfd, 0, (jint)st.st_size, (jint)0);
                if (ok) loaded_from_file = true;
                CRASH_PROTECT_END("nativeEnrollMutableDictFd")
                close(fd);
            }
        }
    }

    // Refresh data manager first
    if (g_refreshData && g_dm) {
        CRASH_PROTECT_BEGIN()
        g_refreshData(g_env, NULL, g_dm);
        CRASH_PROTECT_END("nativeRefreshData(pre-userdict)")
    }

    // Enroll setting scheme for the accessor name (critical for data model)
    enroll_accessor_setting_scheme();

    if (g_refreshData && g_dm) {
        CRASH_PROTECT_BEGIN()
        g_refreshData(g_env, NULL, g_dm);
        CRASH_PROTECT_END("nativeRefreshData(userdict)")
    }

    // Create the accessor
    if (!create_accessor(loaded_from_file)) return false;

    s_ready = true;
    return true;
}

// ── Learn / unlearn ─────────────────────────────────────────────────────────

bool hmm_user_dict_learn(const char **tokens, const int *token_types,
                         int token_count, const char *value,
                         bool is_full_match) {
    if (!s_ready || !s_addCount || !s_accessor || !tokens || !token_types ||
        token_count <= 0 || !value || !value[0]) {
        return false;
    }

    jobject token_array = jni_NewObjectArray(g_env, token_count, NULL, NULL);
    jintArray type_array = jni_NewIntArray(g_env, token_count);
    if (!token_array || !type_array) return false;
    jint *types = malloc((size_t)token_count * sizeof(*types));
    if (!types) return false;
    for (int i = 0; i < token_count; ++i) {
        if (!tokens[i] || !tokens[i][0]) {
            free(types);
            return false;
        }
        jni_SetObjectArrayElement(g_env, token_array, i,
                                  jni_NewStringUTF(g_env, tokens[i]));
        types[i] = token_types[i];
    }
    jni_SetIntArrayRegion(g_env, type_array, 0, token_count, types);
    free(types);
    jstring jvalue = jni_NewStringUTF(g_env, value);
    jboolean result = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    result = s_addCount(g_env, NULL, s_accessor, token_array, type_array,
                        jvalue, (jint)1,
                        is_full_match ? (jboolean)JNI_TRUE
                                      : (jboolean)JNI_FALSE);
    CRASH_PROTECT_END("nativeAddCount")
    return (bool)result;
}

bool hmm_user_dict_unlearn(const char **tokens, const int *token_types,
                           int token_count, const char *value) {
    if (!s_ready || !s_decreaseCount || !s_accessor || !tokens || !token_types ||
        token_count <= 0 || !value || !value[0]) {
        return false;
    }

    jobject token_array = jni_NewObjectArray(g_env, token_count, NULL, NULL);
    jintArray type_array = jni_NewIntArray(g_env, token_count);
    if (!token_array || !type_array) return false;
    jint *types = malloc((size_t)token_count * sizeof(*types));
    if (!types) return false;
    for (int i = 0; i < token_count; ++i) {
        if (!tokens[i] || !tokens[i][0]) {
            free(types);
            return false;
        }
        jni_SetObjectArrayElement(g_env, token_array, i,
                                  jni_NewStringUTF(g_env, tokens[i]));
        types[i] = token_types[i];
    }
    jni_SetIntArrayRegion(g_env, type_array, 0, token_count, types);
    free(types);
    jboolean result = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    result = s_decreaseCount(g_env, NULL, s_accessor, token_array, type_array,
                             jni_NewStringUTF(g_env, value), (jint)1);
    CRASH_PROTECT_END("nativeDecreaseCount")
    return (bool)result;
}

// ── Persistence ─────────────────────────────────────────────────────────────

bool hmm_user_dict_persist(void) {
    if (!s_ready || !s_accessor || !s_user_data_dir[0] || !s_persist) return false;

    char primary[4096], tmp_path[4096], bak_path[4096];
    build_path(primary, sizeof(primary), "");
    build_path(tmp_path, sizeof(tmp_path), "_tmp");
    build_path(bak_path, sizeof(bak_path), "_bak");

    // 1. Remove stale tmp
    if (access(tmp_path, F_OK) == 0 && unlink(tmp_path) != 0) return false;

    // 2. Duplicate dictionary
    if (s_duplicateDict) {
        jboolean duplicate_ok = JNI_FALSE;
        CRASH_PROTECT_BEGIN()
        duplicate_ok = s_duplicateDict(g_env, NULL, s_accessor);
        CRASH_PROTECT_END("nativeDuplicateDictionary")
        if (!duplicate_ok) return false;
    }

    // 3. Compact (ignore return per Android behavior)
    if (s_compact) {
        CRASH_PROTECT_BEGIN()
        s_compact(g_env, NULL, s_accessor, (jint)COMPACT_TARGET);
        CRASH_PROTECT_END("nativeCompact")
    }

    // 4. Persist to tmp
    jstring jtmp = jni_NewStringUTF(g_env, tmp_path);
    jboolean persist_ok = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    persist_ok = s_persist(g_env, NULL, s_accessor, jtmp);
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

    // 9. Re-enroll the saved snapshot, then refresh decoder and accessor.
    struct stat saved_stat;
    if (!s_enrollMutableDictFd ||
        stat(primary, &saved_stat) != 0 || saved_stat.st_size <= 0) {
        return false;
    }
    int fd = open(primary, O_RDONLY);
    if (fd < 0) return false;
    jobject jfd = jni_create_file_descriptor(g_env, fd);
    jboolean enrolled = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    enrolled = s_enrollMutableDictFd(
        g_env, NULL, g_dm, jni_NewStringUTF(g_env, DICT_FILENAME),
        (jint)23, jfd, 0, (jint)saved_stat.st_size, (jint)0);
    CRASH_PROTECT_END("nativeEnrollMutableDictFd(persist)")
    close(fd);
    if (!enrolled) return false;

    if (g_refreshData && g_dm) {
        CRASH_PROTECT_BEGIN()
        g_refreshData(g_env, NULL, g_dm);
        CRASH_PROTECT_END("DataManager nativeRefreshData(persist)")
    }
    atomic_store(&s_decoder_refresh_pending, true);
    return true;
}

void hmm_user_dict_refresh_decoder_if_needed(void) {
    if (!atomic_load(&s_decoder_refresh_pending) || !s_accessor) return;
    if (!hmm_engine_refresh_user_dictionary()) return;
    if (s_refreshAccessor) {
        CRASH_PROTECT_BEGIN()
        s_refreshAccessor(g_env, NULL, s_accessor);
        CRASH_PROTECT_END("MutableDictionaryAccessor nativeRefreshData")
    }
    atomic_store(&s_decoder_refresh_pending, false);
}

// ── Query ───────────────────────────────────────────────────────────────────

int hmm_user_dict_get_size(void) {
    if (!s_ready || !s_getDictSize || !s_accessor) return 0;
    jint size = 0;
    CRASH_PROTECT_BEGIN()
    size = s_getDictSize(g_env, NULL, s_accessor);
    CRASH_PROTECT_END("nativeGetDictionarySize")
    return (int)size;
}

// ── Clear ───────────────────────────────────────────────────────────────────

bool hmm_user_dict_clear(void) {
    if (!s_ready || !s_accessor || !s_newEmptyDict) return false;

    jboolean cleared = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    cleared = s_newEmptyDict(g_env, NULL, s_accessor);
    CRASH_PROTECT_END("nativeNewEmptyDictionary(clear)")
    if (!cleared) return false;

    if (s_user_data_dir[0]) {
        return hmm_user_dict_persist();
    }
    atomic_store(&s_decoder_refresh_pending, true);
    return true;
}

// ── Token extraction ────────────────────────────────────────────────────────

int hmm_user_dict_extract_tokens(int candidate_index,
                                 char tokens[][16], int *types, int max_tokens) {
    if (!s_getCandTokenCount || !s_getCandToken || !g_engine) return 0;
    if (!tokens || !types || max_tokens <= 0) return 0;

    jint count = 0;
    CRASH_PROTECT_BEGIN()
    count = s_getCandTokenCount(g_env, NULL, g_engine, (jint)candidate_index);
    CRASH_PROTECT_END("nativeGetCandidateTokenCount")
    if (count <= 0) return 0;
    if (count > max_tokens) count = max_tokens;

    int filled = 0;
    for (jint i = 0; i < count; i++) {
        jlong token = 0;
        CRASH_PROTECT_BEGIN()
        token = s_getCandToken(g_env, NULL, g_engine, (jint)candidate_index, i);
        CRASH_PROTECT_END("nativeGetCandidateToken")
        if (!token) continue;

        if (s_getTokenString) {
            jstring js = NULL;
            CRASH_PROTECT_BEGIN()
            js = s_getTokenString(g_env, NULL, g_engine, token);
            CRASH_PROTECT_END("nativeGetTokenString")
            const char *s = js ? jni_get_string(js) : "";
            strncpy(tokens[filled], s && s[0] ? s : "", 15);
            tokens[filled][15] = '\0';
        } else {
            tokens[filled][0] = '\0';
        }

        if (s_getTokenLanguage) {
            jint t = 0;
            CRASH_PROTECT_BEGIN()
            t = s_getTokenLanguage(g_env, NULL, g_engine, token);
            CRASH_PROTECT_END("nativeGetTokenLanguage")
            types[filled] = (int)t;
        } else {
            types[filled] = 0;
        }
        filled++;
    }
    return filled;
}

// ── Teardown ────────────────────────────────────────────────────────────────

void hmm_user_dict_destroy(void) {
    if (!s_ready) return;
    if (s_user_data_dir[0]) hmm_user_dict_persist();
    if (s_closeAccessor && s_accessor) {
        CRASH_PROTECT_BEGIN()
        s_closeAccessor(g_env, NULL, s_accessor);
        CRASH_PROTECT_END("nativeClose(destroy)")
    }
    s_accessor = 0;
    s_ready = false;
    atomic_store(&s_decoder_refresh_pending, false);
    s_user_data_dir[0] = '\0';
    s_pack_dir[0] = '\0';
}

bool hmm_user_dict_is_ready(void) { return s_ready; }
