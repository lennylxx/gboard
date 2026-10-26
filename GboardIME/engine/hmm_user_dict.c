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
static bool  s_pinyin_settings_valid = true;
static atomic_bool s_decoder_refresh_pending = false;
static char  s_user_data_dir[4096] = {0};
static char  s_pack_dir[4096] = {0};

#define COMPACT_TARGET 450000

// AbstractHmmEngineFactory type 4: the shortcuts mutable dictionaries.
// Like Gboard's sync, ASCII-only phrases go to the English dictionary.
typedef struct {
    const char *filename;
    const char *accessor;
} ShortcutDict;
static const ShortcutDict s_shortcut_dicts[] = {
    {"shortcuts_dict_3_3", "zh_t_i0_pinyin_shortcuts_dictionary_accessor"},
    {"shortcuts_dict_3_3_english", "en_shortcut_dictionary_accessor"},
};
#define SHORTCUT_DICT_COUNT \
    (int)(sizeof(s_shortcut_dicts) / sizeof(s_shortcut_dicts[0]))
#define SHORTCUTS_DICT_TYPE     24
#define SHORTCUTS_DICT_CAPACITY 4
// Each shortcut letter is one token of this type (shortcuts_tokens).
#define SHORTCUT_TOKEN_TYPE     26
// Candidates read shortcut tokens back as type 25 (verified in tests).
#define SHORTCUT_DECODED_TYPE   25
// Gboard's personal dictionary reports a fixed frequency for every word.
#define SHORTCUT_FREQUENCY      255

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

static void shortcuts_path(char *out, size_t out_sz, const ShortcutDict *dict,
                           const char *suffix) {
    snprintf(out, out_sz, "%s/%s%s", s_user_data_dir, dict->filename, suffix);
}

// Like AbstractHmmEngineFactory.G(), enroll the saved shortcuts file over
// the empty dictionary registered by hmm_enroll_all().
static bool enroll_shortcuts_file(const ShortcutDict *dict) {
    if (!s_user_data_dir[0] || !s_natives->enrollMutableDictFd || !g_dm) {
        return false;
    }
    char path[4096];
    shortcuts_path(path, sizeof(path), dict, "");
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) return false;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    jstring name = jni_NewStringUTF(g_env, dict->filename);
    volatile jboolean ok = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    ok = s_natives->enrollMutableDictFd(
        g_env, NULL, g_dm, name, (jint)SHORTCUTS_DICT_TYPE,
        jni_create_file_descriptor(g_env, fd), 0, (jint)st.st_size,
        (jint)SHORTCUTS_DICT_CAPACITY);
    CRASH_PROTECT_END("nativeEnrollMutableDictFd(shortcuts)")
    close(fd);
    jni_release_string(name);
    if (!ok) LOGERR("Failed to enroll shortcuts dictionary '%s'", path);
    return ok;
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
    for (int i = 0; i < SHORTCUT_DICT_COUNT; ++i) {
        enroll_shortcuts_file(&s_shortcut_dicts[i]);
    }

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

bool hmm_user_dict_pinyin_settings_valid(void) {
    return s_pinyin_settings_valid;
}

static bool refresh_pinyin_accessor(void) {
    volatile bool refreshed = false;
    // Both decoder and accessor resolve to the same nativeRefreshData(J)V.
    CRASH_PROTECT_BEGIN()
    g_refreshData(g_env, NULL, PINYIN_DICT->accessor);
    refreshed = true;
    CRASH_PROTECT_END("MutableDictionaryAccessor nativeRefreshData(pinyin scheme)")
    return refreshed;
}

bool hmm_user_dict_configure_pinyin_accessor(HmmPinyinScheme scheme,
                                             bool shortcuts) {
    if (!s_ready) return true;
    if (!g_refreshData) {
        LOGERR("Cannot refresh user dictionary accessor settings");
        return false;
    }
    // AbstractHmmChineseDecodeProcessor.C() refreshes the existing
    // accessor; replacing it can discard unpersisted learning.
    bool refreshed = hmm_enroll_pinyin_accessor_settings(
                         s_pack_dir, scheme, shortcuts) &&
                     refresh_pinyin_accessor();
    if (!refreshed) {
        LOGERR("Cannot configure user dictionary accessor (scheme %d, shortcuts %d)",
               scheme, shortcuts);
        s_pinyin_settings_valid = hmm_enroll_pinyin_accessor_settings(
            s_pack_dir, hmm_engine_get_pinyin_scheme(),
            hmm_engine_get_shortcuts_enabled()) && refresh_pinyin_accessor();
        if (!s_pinyin_settings_valid) {
            LOGERR("Cannot restore accessor settings; input and learning blocked until retry");
        }
        return false;
    }
    s_pinyin_settings_valid = true;
    return true;
}

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
    if (!s_pinyin_settings_valid) {
        LOGERR("Cannot learn: pinyin accessor settings require a successful retry");
        return false;
    }
    if (!s_ready || !s_natives->addCount ||
        !tokens || !token_types ||
        token_count <= 0 || !value || !value[0]) {
        return false;
    }
    // Gboard would learn shortcut expansions into the pinyin dictionary,
    // but those entries cannot be managed here and would outlive the
    // shortcut. The shortcut dictionary already ranks them first.
    for (int i = 0; i < token_count; ++i) {
        if (token_types[i] == SHORTCUT_DECODED_TYPE ||
            token_types[i] == SHORTCUT_TOKEN_TYPE) {
            return false;
        }
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
    if (!s_pinyin_settings_valid) {
        LOGERR("Cannot unlearn: pinyin accessor settings require a successful retry");
        return false;
    }
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

// ── Learned word management ─────────────────────────────────────────────────

static bool usable_entry(const HmmUserDictEntry *entry) {
    if (!s_ready || !entry || !entry->value || !entry->value[0] ||
        !entry->tokens || !entry->languages || entry->token_count <= 0 ||
        entry->token_count > HMM_USER_DICT_MAX_TOKENS ||
        (entry->dictionary != HMM_USER_DICT_PINYIN &&
         entry->dictionary != HMM_USER_DICT_ENGLISH) ||
        !s_dicts[entry->dictionary].accessor) {
        return false;
    }
    for (int i = 0; i < entry->token_count; ++i) {
        size_t length = entry->tokens[i] ? strlen(entry->tokens[i]) : 0;
        if (length == 0 || length > HMM_USER_DICT_MAX_TOKEN_BYTES) return false;
        if (entry->languages[i] != HMM_LANGUAGE_CHINESE &&
            entry->languages[i] != HMM_LANGUAGE_ENGLISH) {
            return false;
        }
    }
    if (entry->dictionary == HMM_USER_DICT_PINYIN && !s_pinyin_settings_valid) {
        LOGERR("Cannot edit entries: pinyin accessor settings require a successful retry");
        return false;
    }
    return true;
}

// Inserted keys must look like learned ones: the native splits English
// into one token per letter, and dictionary_for_types routes only
// all-English keys to the English dictionary.
static bool learnable_entry(const HmmUserDictEntry *entry) {
    if (!usable_entry(entry)) return false;
    bool chinese = false;
    for (int i = 0; i < entry->token_count; ++i) {
        if (entry->languages[i] == HMM_LANGUAGE_CHINESE) {
            chinese = true;
        } else if (strlen(entry->tokens[i]) != 1) {
            return false;
        }
    }
    return chinese == (entry->dictionary == HMM_USER_DICT_PINYIN);
}

static char *copy_string(const char *s) {
    return strdup(s ? s : "");
}

static void free_entry(HmmUserDictEntry *entry) {
    for (int i = 0; i < entry->token_count; ++i) free(entry->tokens[i]);
    free(entry->tokens);
    free(entry->languages);
    free(entry->value);
}

void hmm_user_dict_free_entries(HmmUserDictEntry *entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; ++i) free_entry(&entries[i]);
    free(entries);
}

static bool copy_exported_entry(HmmUserDictKind kind, const JniDictEntry *src,
                                HmmUserDictEntry *dst) {
    *dst = (HmmUserDictEntry){
        .dictionary = kind,
        .count = src->count,
        .full_match = src->match_type == 0,
    };
    dst->value = copy_string(src->value);
    if (!dst->value) return false;
    if (src->token_count > 0) {
        dst->tokens = calloc((size_t)src->token_count, sizeof(*dst->tokens));
        dst->languages = calloc((size_t)src->token_count,
                                sizeof(*dst->languages));
        if (!dst->tokens || !dst->languages) {
            free_entry(dst);
            return false;
        }
        for (int i = 0; i < src->token_count; ++i) {
            dst->tokens[i] = copy_string(jni_get_string(src->tokens[i]));
            if (!dst->tokens[i]) {
                free_entry(dst);
                return false;
            }
            dst->token_count = i + 1;
            dst->languages[i] = src->languages ? src->languages[i] : 0;
        }
    }
    return true;
}

int hmm_user_dict_export_entries(HmmUserDictEntry **out) {
    if (!out) return -1;
    *out = NULL;
    if (!s_ready || !s_natives->exportAllEntries) return -1;

    HmmUserDictEntry *entries = NULL;
    int count = 0, capacity = 0;
    for (int i = 0; i < DICT_COUNT; ++i) {
        if (!s_dicts[i].accessor) continue;
        jobject volatile exported = NULL;
        CRASH_PROTECT_BEGIN()
        exported = s_natives->exportAllEntries(g_env, NULL, s_dicts[i].accessor);
        CRASH_PROTECT_END("nativeExportAllEntries")
        if (!exported) {
            hmm_user_dict_free_entries(entries, count);
            return -1;
        }
        jsize length = jni_GetArrayLength(g_env, exported);
        bool copied = true;
        for (jsize j = 0; j < length && copied; ++j) {
            JniDictEntry src;
            if (!jni_get_dict_entry(jni_GetObjectArrayElement(g_env, exported, j),
                                    &src) || src.count <= 0 ||
                src.match_type > 1 || src.token_count <= 0 ||
                !src.value || !src.value[0]) {
                continue;
            }
            if (count == capacity) {
                int grown = capacity ? capacity * 2 : 256;
                HmmUserDictEntry *larger =
                    realloc(entries, (size_t)grown * sizeof(*entries));
                if (!larger) {
                    copied = false;
                    break;
                }
                entries = larger;
                capacity = grown;
            }
            copied = copy_exported_entry((HmmUserDictKind)i, &src,
                                         &entries[count]);
            if (copied) count++;
        }
        jni_release_dict_entries(exported);
        if (!copied) {
            hmm_user_dict_free_entries(entries, count);
            return -1;
        }
    }
    *out = entries;
    return count;
}

static bool entry_arrays(const HmmUserDictEntry *entry, jobject *token_array,
                         jintArray *type_array) {
    if (build_token_arrays((const char **)entry->tokens, entry->languages,
                           entry->token_count, token_array, type_array)) {
        return true;
    }
    jni_release_string_array(*token_array);
    jni_release_array(*type_array);
    return false;
}

bool hmm_user_dict_remove_entry(const HmmUserDictEntry *entry) {
    if (!usable_entry(entry) || !s_natives->removeByTokenAndValue) return false;
    jobject tokens;
    jintArray types;
    if (!entry_arrays(entry, &tokens, &types)) return false;
    jstring value = jni_NewStringUTF(g_env, entry->value);
    jboolean volatile removed = JNI_FALSE;
    CRASH_PROTECT_BEGIN()
    removed = s_natives->removeByTokenAndValue(
        g_env, NULL, s_dicts[entry->dictionary].accessor, tokens, types, value);
    CRASH_PROTECT_END("nativeRemoveByTokenAndValue")
    jni_release_string_array(tokens);
    jni_release_array(types);
    jni_release_string(value);
    return (bool)removed;
}

static bool insert_entry(const HmmUserDictEntry *entry, int count,
                         bool modified) {
    jobject tokens;
    jintArray languages;
    if (!entry_arrays(entry, &tokens, &languages)) return false;
    jstring value = jni_NewStringUTF(g_env, entry->value);
    jboolean volatile inserted = JNI_FALSE;
    // The flags are Entry.e (modified) and is_full_match (Entry.f).
    CRASH_PROTECT_BEGIN()
    inserted = s_natives->insertOrUpdate(
        g_env, NULL, s_dicts[entry->dictionary].accessor, tokens, languages,
        value, (jint)count, modified ? JNI_TRUE : JNI_FALSE,
        entry->full_match ? JNI_TRUE : JNI_FALSE);
    CRASH_PROTECT_END("nativeInsertOrUpdate")
    jni_release_string_array(tokens);
    jni_release_array(languages);
    jni_release_string(value);
    return (bool)inserted;
}

bool hmm_user_dict_set_entry_count(const HmmUserDictEntry *entry, int count,
                                   bool full_match) {
    if (count <= 0 || !learnable_entry(entry) || !s_natives->insertOrUpdate) {
        return false;
    }
    // InsertOrUpdate keeps an existing record, including a removal
    // tombstone, unless the new one has a better match type or a higher
    // count, and still reports success. A zero count with modified=false
    // deletes the record outright, so the new count always applies.
    HmmUserDictEntry target = *entry;
    target.full_match = !full_match;
    bool purged = insert_entry(&target, 0, false);
    target.full_match = full_match;
    purged = insert_entry(&target, 0, false) || purged;
    // Learning always marks entries modified (Entry.e).
    if (insert_entry(&target, count, true)) return true;
    if (purged && entry->count > 0 && !insert_entry(entry, entry->count, true)) {
        LOGERR("Cannot restore learned entry after a failed count change");
    }
    return false;
}

// ── Remove a learned candidate ──────────────────────────────────────────────

// Mirrors Gboard's candidate iterator (hbm): a candidate offers "remove
// suggestion" only when its primary data source is a user dictionary and no
// other source (e.g. the system dictionary) contributed to it.
// Emulated JNI strings are heap objects that DeleteLocalRef never frees.
// This check runs on every selection change, so reuse one name per file.
static jstring dictionary_filename_string(int index) {
    static jstring names[DICT_COUNT];
    if (!names[index]) {
        names[index] = jni_NewStringUTF(g_env, s_dicts[index].filename);
    }
    return names[index];
}

bool hmm_user_dict_is_candidate_removable(int candidate_index) {
    if (!s_ready || !g_engine || candidate_index < 0 ||
        !s_natives->getDataSourceIndex ||
        !s_natives->getCandidatePrimaryDataSource ||
        !s_natives->isCandidateFromSingleDataSource) {
        return false;
    }
    volatile bool removable = false;
    CRASH_PROTECT_BEGIN()
    volatile jint primary = s_natives->getCandidatePrimaryDataSource(
        g_env, NULL, g_engine, (jint)candidate_index);
    if (primary >= 0 &&
        s_natives->isCandidateFromSingleDataSource(
            g_env, NULL, g_engine, (jint)candidate_index)) {
        // Indices can change after nativeRefreshData, so look them up each time.
        for (int i = 0; i < DICT_COUNT && !removable; ++i) {
            if (!s_dicts[i].accessor) continue;
            jint source = s_natives->getDataSourceIndex(
                g_env, NULL, g_engine, dictionary_filename_string(i));
            removable = source >= 0 && source == primary;
        }
    }
    CRASH_PROTECT_END("isCandidateRemovable")
    return removable;
}

// Mirrors AbstractHmmChineseDecodeProcessor.X(): remove the candidate text
// by value from both user dictionaries, then drop it from the decoder's
// current list. Like Gboard, this does not persist immediately.
bool hmm_user_dict_remove_candidate(int candidate_index) {
    if (!hmm_user_dict_is_candidate_removable(candidate_index) ||
        !s_natives->removeByValue || !s_natives->deleteCandidate ||
        !g_getCandString) {
        return false;
    }
    jstring volatile value = NULL;
    CRASH_PROTECT_BEGIN()
    value = g_getCandString(g_env, NULL, g_engine, (jint)candidate_index);
    CRASH_PROTECT_END("nativeGetCandidateString(remove)")
    const char *text = value ? jni_get_string(value) : NULL;
    if (!text || !text[0]) {
        jni_release_string(value);
        return false;
    }

    volatile bool removed = false;
    for (int i = 0; i < DICT_COUNT; ++i) {
        if (!s_dicts[i].accessor) continue;
        jstring volatile key = jni_NewStringUTF(g_env, text);
        CRASH_PROTECT_BEGIN()
        if (s_natives->removeByValue(g_env, NULL, s_dicts[i].accessor, key)) {
            removed = true;
        }
        CRASH_PROTECT_END("nativeRemoveByValue")
        jni_release_string(key);
    }
    jni_release_string(value);
    if (!removed) return false;

    CRASH_PROTECT_BEGIN()
    if (!s_natives->deleteCandidate(g_env, NULL, g_engine,
                                    (jint)candidate_index)) {
        LOGERR("nativeDeleteCandidate failed for index %d", candidate_index);
    }
    CRASH_PROTECT_END("nativeDeleteCandidate")
    return true;
}

// ── Persistence ─────────────────────────────────────────────────────────────

static bool persist_dictionary(const UserDict *dict) {
    char primary[4096], tmp_path[4096], bak_path[4096];
    build_path(primary, sizeof(primary), dict, "");
    build_path(tmp_path, sizeof(tmp_path), dict, "_tmp");
    build_path(bak_path, sizeof(bak_path), dict, "_bak");

    // 1. Remove stale tmp
    if (access(tmp_path, F_OK) == 0 && unlink(tmp_path) != 0) return false;

    // 2-4. Like Gboard's SaveDictionaryTask, snapshot through a temporary
    // accessor. Duplicating or compacting the live accessor would detach it
    // from the decoder, so later learning and removal would stop showing.
    volatile jlong snapshot = 0;
    CRASH_PROTECT_BEGIN()
    snapshot = s_natives->createAccessor(
        g_env, NULL, g_factory, jni_NewStringUTF(g_env, dict->accessor_name),
        jni_NewStringUTF(g_env, ""), jni_NewStringUTF(g_env, dict->filename));
    CRASH_PROTECT_END("nativeCreateMutableDictionaryAccessor(persist)")
    if (!snapshot) return false;

    volatile jboolean persist_ok = JNI_FALSE;
    volatile jboolean duplicate_ok =
        s_natives->duplicateDictionary ? JNI_FALSE : JNI_TRUE;
    if (s_natives->duplicateDictionary) {
        CRASH_PROTECT_BEGIN()
        duplicate_ok = s_natives->duplicateDictionary(g_env, NULL, snapshot);
        CRASH_PROTECT_END("nativeDuplicateDictionary")
    }
    if (duplicate_ok) {
        // Ignore the compact result, as Android does.
        if (s_natives->compact) {
            CRASH_PROTECT_BEGIN()
            s_natives->compact(g_env, NULL, snapshot, (jint)COMPACT_TARGET);
            CRASH_PROTECT_END("nativeCompact")
        }
        jstring jtmp = jni_NewStringUTF(g_env, tmp_path);
        CRASH_PROTECT_BEGIN()
        persist_ok = s_natives->persist(g_env, NULL, snapshot, jtmp);
        CRASH_PROTECT_END("nativePersist")
    }
    if (s_natives->closeAccessor) {
        CRASH_PROTECT_BEGIN()
        s_natives->closeAccessor(g_env, NULL, snapshot);
        CRASH_PROTECT_END("nativeClose(persist)")
    }
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

    // 9. Re-enroll the saved snapshot. Rebind the accessor now so edits
    // made before the next reset land in the enrolled data; only the
    // decoder refresh is deferred.
    if (!enroll_dictionary_file(dict, primary)) return false;
    if (s_natives->refreshData) {
        CRASH_PROTECT_BEGIN()
        s_natives->refreshData(g_env, NULL, dict->accessor);
        CRASH_PROTECT_END("MutableDictionaryAccessor nativeRefreshData(persist)")
    }
    return true;
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

bool hmm_user_dict_decoder_refresh_pending(void) {
    return atomic_load(&s_decoder_refresh_pending);
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

// Deletes one user dictionary file; a missing file already counts as removed.
static bool remove_dictionary_file(const UserDict *dict, const char *suffix) {
    char path[4096];
    build_path(path, sizeof(path), dict, suffix);
    if (unlink(path) == 0 || errno == ENOENT) return true;
    LOGERR("failed to remove user dictionary file '%s' (errno=%d)",
           path, errno);
    return false;
}

// Removes stale copies of cleared data. Dictionaries with an accessor keep
// their freshly persisted empty primary file; dictionaries without one cannot
// persist, so their primary file is removed too.
static bool remove_cleared_files(void) {
    bool ok = true;
    for (int i = 0; i < DICT_COUNT; ++i) {
        const UserDict *dict = &s_dicts[i];
        if (!dict->accessor && !remove_dictionary_file(dict, "")) ok = false;
        if (!remove_dictionary_file(dict, "_bak")) ok = false;
        if (!remove_dictionary_file(dict, "_tmp")) ok = false;
    }
    return ok;
}

bool hmm_user_dict_clear(void) {
    if (!s_ready) return false;

    // Clear every dictionary before persisting so a single failure does not
    // leave an already-cleared dictionary unsaved. Like Gboard, replace the
    // enrolled data instead of detaching the live accessor from the decoder;
    // the accessor adopts it now and the decoder on its next reset.
    bool all_cleared = true;
    for (int i = 0; i < DICT_COUNT; ++i) {
        if (!s_dicts[i].accessor) continue;
        volatile jboolean cleared = JNI_FALSE;
        CRASH_PROTECT_BEGIN()
        if (g_enrollEmptyMutableDict && g_dm && s_natives->refreshData) {
            cleared = g_enrollEmptyMutableDict(
                g_env, NULL, g_dm, jni_NewStringUTF(g_env, s_dicts[i].filename),
                (jint)23, (jint)0);
            if (cleared) {
                s_natives->refreshData(g_env, NULL, s_dicts[i].accessor);
            }
        }
        CRASH_PROTECT_END("nativeEnrollEmptyMutableDict(clear)")
        if (!cleared) all_cleared = false;
    }

    if (s_user_data_dir[0]) {
        bool persisted = hmm_user_dict_persist();
        bool removed = remove_cleared_files();
        return all_cleared && persisted && removed;
    }
    atomic_store(&s_decoder_refresh_pending, true);
    return all_cleared;
}

// ── Shortcuts ───────────────────────────────────────────────────────────────

// Returns the length of one UTF-8 sequence at s, or 0 if malformed.
static size_t utf8_sequence_length(const unsigned char *s) {
    size_t length;
    uint32_t cp;
    if (s[0] < 0x80) return 1;
    if ((s[0] & 0xE0) == 0xC0) { length = 2; cp = s[0] & 0x1F; }
    else if ((s[0] & 0xF0) == 0xE0) { length = 3; cp = s[0] & 0x0F; }
    else if ((s[0] & 0xF8) == 0xF0) { length = 4; cp = s[0] & 0x07; }
    else return 0;
    for (size_t i = 1; i < length; ++i) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    static const uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < minimum[length] || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return length;
}

bool hmm_shortcut_is_valid(const char *shortcut, const char *phrase) {
    if (!shortcut || !phrase || !phrase[0]) return false;
    size_t letters = strlen(shortcut);
    if (letters == 0 || letters > HMM_SHORTCUT_MAX_LETTERS) return false;
    for (size_t i = 0; i < letters; ++i) {
        if (shortcut[i] < 'a' || shortcut[i] > 'z') return false;
    }
    for (const unsigned char *p = (const unsigned char *)phrase; *p;) {
        size_t length = utf8_sequence_length(p);
        if (length == 0) return false;
        p += length;
    }
    return true;
}

static bool phrase_is_ascii(const char *phrase) {
    for (const unsigned char *p = (const unsigned char *)phrase; *p; ++p) {
        if (*p >= 0x7F) return false;
    }
    return true;
}

static bool insert_shortcut(jlong accessor, const HmmShortcut *entry) {
    int letters = (int)strlen(entry->shortcut);
    jobject tokens = jni_NewObjectArray(g_env, letters, NULL, NULL);
    jintArray types = jni_NewIntArray(g_env, letters);
    if (!tokens || !types) {
        jni_release_string_array(tokens);
        jni_release_array(types);
        return false;
    }
    jint type_values[HMM_SHORTCUT_MAX_LETTERS];
    for (int i = 0; i < letters; ++i) {
        char letter[2] = {entry->shortcut[i], '\0'};
        jni_SetObjectArrayElement(g_env, tokens, i, jni_NewStringUTF(g_env, letter));
        type_values[i] = SHORTCUT_TOKEN_TYPE;
    }
    jni_SetIntArrayRegion(g_env, types, 0, letters, type_values);
    jstring value = jni_NewStringUTF(g_env, entry->phrase);
    volatile jboolean inserted = JNI_FALSE;
    // Unmodified (Entry.e) and a full match, as the sync inserts them.
    CRASH_PROTECT_BEGIN()
    inserted = s_natives->insertOrUpdate(
        g_env, NULL, accessor, tokens, types, value,
        (jint)SHORTCUT_FREQUENCY, JNI_FALSE, JNI_TRUE);
    CRASH_PROTECT_END("nativeInsertOrUpdate(shortcut)")
    jni_release_string_array(tokens);
    jni_release_array(types);
    jni_release_string(value);
    return inserted;
}

// Builds the complete dictionary in a temporary accessor and persists it.
static bool write_shortcuts_file(const HmmShortcut *entries, int count,
                                 bool english, const char *path) {
    const ShortcutDict *dict = &s_shortcut_dicts[english ? 1 : 0];
    jstring name = jni_NewStringUTF(g_env, dict->accessor);
    jstring locale = jni_NewStringUTF(g_env, "");
    jstring type = jni_NewStringUTF(g_env, dict->filename);
    volatile jlong accessor = 0;
    CRASH_PROTECT_BEGIN()
    accessor = s_natives->createAccessor(g_env, NULL, g_factory, name, locale, type);
    CRASH_PROTECT_END("nativeCreateMutableDictionaryAccessor(shortcuts)")
    jni_release_string(name);
    jni_release_string(locale);
    jni_release_string(type);
    if (!accessor) return false;

    volatile bool ok = false;
    CRASH_PROTECT_BEGIN()
    ok = s_natives->newEmptyDictionary(g_env, NULL, accessor);
    CRASH_PROTECT_END("nativeNewEmptyDictionary(shortcuts)")
    for (int i = 0; ok && i < count; ++i) {
        if (phrase_is_ascii(entries[i].phrase) != english) continue;
        ok = insert_shortcut(accessor, &entries[i]);
        if (!ok) LOGERR("Cannot insert shortcut '%s'", entries[i].shortcut);
    }
    if (ok) {
        jstring jpath = jni_NewStringUTF(g_env, path);
        ok = false;
        CRASH_PROTECT_BEGIN()
        ok = s_natives->persist(g_env, NULL, accessor, jpath);
        CRASH_PROTECT_END("nativePersist(shortcuts)")
        jni_release_string(jpath);
    }
    if (s_natives->closeAccessor) {
        CRASH_PROTECT_BEGIN()
        s_natives->closeAccessor(g_env, NULL, accessor);
        CRASH_PROTECT_END("nativeClose(shortcuts)")
    }
    return ok;
}

bool hmm_user_dict_replace_shortcuts(const HmmShortcut *entries, int count) {
    if (!s_ready || !s_user_data_dir[0] || count < 0 || (count > 0 && !entries) ||
        !s_natives->createAccessor || !s_natives->newEmptyDictionary ||
        !s_natives->insertOrUpdate ||
        !s_natives->persist) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (!hmm_shortcut_is_valid(entries[i].shortcut, entries[i].phrase)) {
            LOGERR("Rejecting invalid shortcut at index %d", i);
            return false;
        }
    }
    // Build both files before replacing either one.
    char primary[SHORTCUT_DICT_COUNT][4096], tmp_path[SHORTCUT_DICT_COUNT][4096];
    bool ok = true;
    for (int i = 0; i < SHORTCUT_DICT_COUNT; ++i) {
        shortcuts_path(primary[i], sizeof(primary[i]), &s_shortcut_dicts[i], "");
        shortcuts_path(tmp_path[i], sizeof(tmp_path[i]), &s_shortcut_dicts[i], "_tmp");
        ok = ok && (unlink(tmp_path[i]) == 0 || errno == ENOENT) &&
             write_shortcuts_file(entries, count, i == 1, tmp_path[i]);
    }
    // Enroll each file as soon as it replaces the old one, so the enrolled
    // dictionary always matches its file. On a partial failure the caller
    // restores consistency by replacing again with the previous list.
    bool changed = false;
    for (int i = 0; ok && i < SHORTCUT_DICT_COUNT; ++i) {
        ok = rename(tmp_path[i], primary[i]) == 0;
        if (!ok) break;
        changed = true;
        ok = enroll_shortcuts_file(&s_shortcut_dicts[i]);
    }
    for (int i = 0; i < SHORTCUT_DICT_COUNT; ++i) unlink(tmp_path[i]);
    if (changed) hmm_engine_mark_shortcuts_dirty();
    return ok;
}

bool hmm_user_dict_is_shortcut_candidate(int candidate_index) {
    char tokens[HMM_SHORTCUT_MAX_LETTERS][16];
    int types[HMM_SHORTCUT_MAX_LETTERS];
    int count = hmm_user_dict_extract_tokens(
        candidate_index, tokens, types, HMM_SHORTCUT_MAX_LETTERS);
    for (int i = 0; i < count; ++i) {
        if (types[i] != SHORTCUT_DECODED_TYPE && types[i] != SHORTCUT_TOKEN_TYPE) {
            return false;
        }
    }
    return count > 0;
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
    s_pinyin_settings_valid = true;
    atomic_store(&s_decoder_refresh_pending, false);
    s_user_data_dir[0] = '\0';
    s_pack_dir[0] = '\0';
}

bool hmm_user_dict_is_ready(void) { return s_ready; }
