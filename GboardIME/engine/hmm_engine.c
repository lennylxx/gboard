// Wrapper around Gboard's native HMM Pinyin engine.
// Loads libintegrated_shared_object.so via our ELF loader and calls
// the JNI entry points directly.
//
// This file is the orchestration layer. The heavy lifting lives in:
//   hmm_native.c     — JNI function pointer resolution
//   hmm_enroll.c     — data/setting scheme enrollment
//   hmm_candidates.c — append, candidates, select, reset

#include "hmm_internal.h"
#include "hmm_user_dict.h"

// ── Global state definitions (declared extern in hmm_internal.h) ────────────

sigjmp_buf s_hmm_jmp;
volatile void *s_crash_addr = NULL;

void hmm_crash_handler_sa(int sig, siginfo_t *info, void *ctx) {
    (void)ctx;
    s_crash_addr = info ? info->si_addr : NULL;
    siglongjmp(s_hmm_jmp, sig);
}

void hmm_alrm_handler(int sig) {
    (void)sig;
    siglongjmp(s_hmm_jmp, SIGALRM);
}

ElfHandle *g_elf = NULL;
JNIEnv    *g_env = NULL;
jlong      g_factory = 0;
jlong      g_dm      = 0;
jlong      g_sm      = 0;
jlong      g_engine  = 0;
jint       g_end_vertex = 0;

fn_CreateFactory    g_createFactory    = NULL;
fn_DeleteFactory    g_deleteFactory    = NULL;
fn_GetDataManager   g_getDataManager   = NULL;
fn_GetSettingManager g_getSettingManager = NULL;
fn_CreateEngine     g_createEngine     = NULL;
fn_EnrollDataScheme g_enrollScheme     = NULL;
fn_EnrollDataFile   g_enrollFile       = NULL;
fn_EnrollDataFd     g_enrollFd         = NULL;
fn_EnrollBuiltInDataScheme g_enrollBuiltInScheme = NULL;
fn_EnrollBuiltInData g_enrollBuiltInData = NULL;
fn_EnrollEmptyMutableDict g_enrollEmptyMutableDict = NULL;
fn_EnrollSettingScheme g_enrollSettingScheme = NULL;
fn_LoadBuiltInSettingScheme g_loadBuiltInSettingScheme = NULL;
fn_Append           g_append           = NULL;
fn_FillCandList     g_fillCandList     = NULL;
fn_GetCandCount     g_getCandCount     = NULL;
fn_GetCandString    g_getCandString    = NULL;
fn_GetCandRange     g_getCandRange     = NULL;
fn_SelectCand       g_selectCand       = NULL;
fn_GetDecodingRange g_getDecodingRange = NULL;
fn_SelectRange      g_selectRange      = NULL;
fn_Reset            g_reset            = NULL;
fn_SetKeyLayout     g_setKeyLayout     = NULL;
fn_BeginSession     g_beginSession     = NULL;
fn_HandleInputContext g_handleInputCtx  = NULL;
fn_FinishSession    g_finishSession    = NULL;
fn_FillTokenCandList g_fillTokenCandList = NULL;
fn_GetTokenCandCount g_getTokenCandCount = NULL;

static char s_pack_dir[4096];
static jlong s_personalized_engine;
static jlong s_context_engine;
static bool s_has_external_context;
static uint64_t s_session_id;
static uint64_t s_last_session_id;
static bool s_session_active;

static size_t put_varint(uint8_t *out, uint64_t value) {
    size_t written = 0;
    while (value >= 0x80) {
        out[written++] = (uint8_t)((value & 0x7f) | 0x80);
        value >>= 7;
    }
    out[written++] = (uint8_t)value;
    return written;
}

static size_t put_int_field(uint8_t *out, uint32_t field,
                            uint64_t value) {
    size_t written = put_varint(out, ((uint64_t)field << 3));
    return written + put_varint(out + written, value);
}

static size_t put_bytes_field(uint8_t *out, uint32_t field,
                              const uint8_t *bytes, size_t length) {
    size_t written =
        put_varint(out, ((uint64_t)field << 3) | 2);
    written += put_varint(out + written, length);
    if (length > 0) memcpy(out + written, bytes, length);
    return written + length;
}

static size_t encode_span(uint8_t *out, const char *text) {
    const char *value = text ? text : "";
    size_t length = strlen(value);
    size_t written = put_int_field(out, 1, 6);
    return written + put_bytes_field(
        out + written, 2, (const uint8_t *)value, length);
}

static jbyteArray make_byte_array(const uint8_t *bytes, size_t length) {
    jbyteArray result = jni_NewByteArray(g_env, (jsize)length);
    if (result && length > 0) {
        jni_SetByteArrayRegion(g_env, result, 0, (jsize)length,
                               (const jbyte *)bytes);
    }
    return result;
}

static uint64_t next_session_id(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    uint64_t result =
        (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_usec / 1000;
    if (result <= s_last_session_id) result = s_last_session_id + 1;
    s_last_session_id = result;
    return result;
}

static void begin_native_session(jlong engine) {
    if (!g_beginSession || !engine) return;
    uint8_t request[16];
    size_t length = put_int_field(request, 1, s_session_id);
    jbyteArray payload = make_byte_array(request, length);
    CRASH_PROTECT_BEGIN()
    g_beginSession(g_env, NULL, engine, payload);
    CRASH_PROTECT_END("nativeBeginSession")
}

static void finish_native_session(jlong engine) {
    if (!s_session_active || !g_finishSession || !engine) return;
    uint8_t request[16];
    size_t length = put_int_field(request, 1, s_session_id);
    jbyteArray payload = make_byte_array(request, length);
    CRASH_PROTECT_BEGIN()
    g_finishSession(g_env, NULL, engine, payload);
    CRASH_PROTECT_END("nativeFinishSession")
}

static jlong create_decoder(const char *const *etypes) {
    jlong engine = 0;
    for (int ei = 0; etypes[ei] && !engine; ei++) {
        jstring engine_type = jni_NewStringUTF(g_env, etypes[ei]);
        jstring user_id = jni_NewStringUTF(g_env, "");
        CRASH_PROTECT_BEGIN()
        engine = g_createEngine(g_env, NULL, g_factory, engine_type, user_id);
        LOGERR("nativeCreateEngine('%s') → %lld",
               etypes[ei], (long long)engine);
        CRASH_PROTECT_END("nativeCreateEngine")
    }
    if (!engine) return 0;

    if (g_setKeyLayout && s_pack_dir[0]) {
        char scheme_path[4096];
        snprintf(scheme_path, sizeof(scheme_path),
                 "%s/pinyin_qwerty_setting_scheme", s_pack_dir);
        FILE *sfp = fopen(scheme_path, "rb");
        if (sfp) {
            fseek(sfp, 0, SEEK_END);
            long size = ftell(sfp);
            fseek(sfp, 0, SEEK_SET);
            uint8_t *bytes = malloc((size_t)size);
            fread(bytes, 1, (size_t)size, sfp);
            fclose(sfp);
            jbyteArray layout = jni_NewByteArray(g_env, (jsize)size);
            jni_SetByteArrayRegion(g_env, layout, 0, (jsize)size,
                                   (jbyte *)bytes);
            free(bytes);
            CRASH_PROTECT_BEGIN()
            g_setKeyLayout(g_env, NULL, engine, layout);
            CRASH_PROTECT_END("nativeSetKeyboardLayout")
        }
    }

    return engine;
}

static bool create_decoders(void) {
    const char *personalized_types[] = {
        "zh-t-i0-pinyin-x-f0-delight", "zh-t-i0-pinyin", NULL
    };
    const char *context_types[] = {
        "zh-t-i0-pinyin-x-f0-delight-context", NULL
    };
    s_personalized_engine = create_decoder(personalized_types);
    if (!s_personalized_engine) return false;
    s_context_engine = create_decoder(context_types);
    if (!s_context_engine) return false;
    g_engine = s_personalized_engine;
    g_end_vertex = 0;
    return true;
}

void hmm_engine_set_external_context(bool has_context) {
    s_has_external_context = has_context;
}

void hmm_engine_prepare_input(const char *pinyin_input) {
    size_t input_length = pinyin_input ? strlen(pinyin_input) : 0;
    bool prioritize_context =
        s_has_external_context || input_length >= 12;
    g_engine = prioritize_context
        ? s_context_engine
        : s_personalized_engine;
}

bool hmm_engine_update_input_context(const char *before_selection,
                                     const char *selected_text,
                                     const char *after_selection) {
    if (!s_session_active || !g_handleInputCtx ||
        !s_personalized_engine || !s_context_engine) return false;

    const char *before = before_selection ? before_selection : "";
    const char *selected = selected_text ? selected_text : "";
    const char *after = after_selection ? after_selection : "";
    size_t span_capacity =
        strlen(before) + strlen(selected) + strlen(after) + 96;
    uint8_t *context = malloc(span_capacity);
    uint8_t *span = malloc(span_capacity);
    if (!context || !span) {
        free(context);
        free(span);
        return false;
    }

    size_t context_length = 0;
    const char *texts[] = {before, selected, after};
    for (size_t i = 0; i < 3; i++) {
        size_t span_length = encode_span(span, texts[i]);
        context_length += put_bytes_field(
            context + context_length, 2, span, span_length);
    }
    context_length += put_int_field(context + context_length, 3, 1);
    context_length += put_int_field(context + context_length, 4, 1);
    context_length += put_int_field(context + context_length, 5, 1);
    context_length += put_int_field(context + context_length, 6, 0);
    context_length += put_int_field(
        context + context_length, 7, selected[0] ? 2 : 1);
    context_length += put_int_field(context + context_length, 8, 0);

    size_t request_capacity = context_length + 32;
    uint8_t *request = malloc(request_capacity);
    if (!request) {
        free(context);
        free(span);
        return false;
    }
    size_t request_length =
        put_bytes_field(request, 1, context, context_length);
    request_length += put_int_field(
        request + request_length, 2, s_session_id);
    jbyteArray payload = make_byte_array(request, request_length);

    const jlong engines[] = {
        s_personalized_engine, s_context_engine
    };
    for (size_t i = 0; i < 2; ++i) {
        CRASH_PROTECT_BEGIN()
        g_handleInputCtx(g_env, NULL, engines[i], payload);
        CRASH_PROTECT_END("nativeHandleInputContext")
    }

    free(request);
    free(context);
    free(span);
    return true;
}

bool hmm_engine_refresh_user_dictionary(void) {
    if (!g_engine || !g_refreshData) return false;
    CRASH_PROTECT_BEGIN()
    g_refreshData(g_env, NULL, g_engine);
    CRASH_PROTECT_END("HmmEngine nativeRefreshData")
    return true;
}
fn_GetTokenCandString g_getTokenCandString = NULL;
fn_SelectTokenCand  g_selectTokenCand  = NULL;
fn_GetSeparator     g_getSeparator     = NULL;
fn_SetSeparator     g_setSeparator     = NULL;
fn_GetSegmentCount  g_getSegmentCount  = NULL;
fn_GetSegment       g_getSegment       = NULL;
fn_GetSegmentRange  g_getSegmentRange  = NULL;
fn_GetSegmentTokenCount g_getSegmentTokenCount = NULL;
fn_GetSegmentToken  g_getSegmentToken  = NULL;
fn_GetTokenString   g_getTokenString   = NULL;
fn_RefreshData      g_refreshData      = NULL;

// ── Public API ──────────────────────────────────────────────────────────────

bool hmm_engine_init(const char *so_path, const char *pack_dir) {
    return hmm_engine_init_with_user_data(so_path, pack_dir, NULL);
}

bool hmm_engine_init_with_user_data(const char *so_path, const char *pack_dir,
                                     const char *user_data_dir) {
    LOG("hmm_engine_init: so=%s pack=%s user_data=%s", so_path, pack_dir,
        user_data_dir ? user_data_dir : "(none)");

    android_stubs_init(pack_dir);

    g_elf = elf_load(so_path);
    if (!g_elf) { LOGERR("elf_load failed"); return false; }
    LOG("elf_load OK");

    g_env = jni_env_create();

    // 1. Call EngineFactory.initJNI to register native methods
    void *init_jni = hmm_sym(g_elf,
        "Java_com_google_android_apps_inputmethod_libs_hmm_EngineFactory_initJNI");
    if (!init_jni) { LOGERR("initJNI symbol not found"); return false; }
    LOG("calling initJNI at %p", init_jni);

    CRASH_PROTECT_BEGIN()
    ((void(*)(JNIEnv*,jobject))init_jni)(g_env, NULL);
    LOGERR("EngineFactory.initJNI OK");
    CRASH_PROTECT_END("EngineFactory.initJNI")
    LOG("initJNI done");

    // 2. Resolve native method function pointers
    hmm_resolve_natives();

    if (!g_createFactory || !g_getDataManager || !g_createEngine) {
        LOGERR("missing critical methods");
        return false;
    }

    // 3. Create EngineFactory
    CRASH_PROTECT_BEGIN()
    g_factory = g_createFactory(g_env, NULL);
    LOGERR("EngineFactory handle: %lld", (long long)g_factory);
    CRASH_PROTECT_END("nativeCreateEngineFactory")
    if (!g_factory) { LOGERR("factory creation failed"); return false; }

    // 4. Get DataManager + SettingManager
    CRASH_PROTECT_BEGIN()
    g_dm = g_getDataManager(g_env, NULL, g_factory);
    LOGERR("DataManager handle: %lld", (long long)g_dm);
    CRASH_PROTECT_END("nativeGetDataManager")
    if (!g_dm) { LOGERR("failed to get DataManager"); return false; }

    if (g_getSettingManager) {
        CRASH_PROTECT_BEGIN()
        g_sm = g_getSettingManager(g_env, NULL, g_factory);
        LOGERR("SettingManager handle: %lld", (long long)g_sm);
        CRASH_PROTECT_END("nativeGetSettingManager")
    }

    // 5. Enroll all data + settings
    if (pack_dir) {
        snprintf(s_pack_dir, sizeof(s_pack_dir), "%s", pack_dir);
        hmm_enroll_all(pack_dir);
    }

    // 6. Load and bind the persistent user dictionary before engine creation.
    if (hmm_user_dict_init(user_data_dir, pack_dir)) {
        LOGERR("user dictionary initialized (size=%d)", hmm_user_dict_get_size());
    } else {
        LOGERR("user dictionary init failed or not available (non-fatal)");
    }

    // 7. Create HMM engine
    if (!create_decoders()) {
        LOGERR("Failed to create decoders");
        return false;
    }
    s_session_id = next_session_id();
    begin_native_session(s_personalized_engine);
    begin_native_session(s_context_engine);
    s_session_active = true;

    LOGERR("init complete! engine=%lld", (long long)g_engine);
    LOG("append=%p fillCand=%p getCandCount=%p getCandStr=%p",
        (void*)g_append, (void*)g_fillCandList, (void*)g_getCandCount, (void*)g_getCandString);

    return true;
}

void hmm_engine_destroy(void) {
    finish_native_session(s_personalized_engine);
    finish_native_session(s_context_engine);
    s_session_active = false;
    if (g_engine) hmm_engine_reset();

    // Tear down user dictionary (persists if configured)
    hmm_user_dict_destroy();

    // Tear down native factory (calls into native code which may use JNI env)
    if (g_deleteFactory && g_factory) {
        CRASH_PROTECT_BEGIN()
        g_deleteFactory(g_env, NULL, g_factory);
        LOGERR("nativeDeleteEngineFactory OK");
        CRASH_PROTECT_END("nativeDeleteEngineFactory")
    }

    g_factory = 0; g_dm = 0; g_sm = 0; g_engine = 0; g_end_vertex = 0;
    s_personalized_engine = 0;
    s_context_engine = 0;
    s_has_external_context = false;
    s_session_id = 0;
    s_session_active = false;
    s_pack_dir[0] = '\0';

    elf_unload(g_elf);
    g_elf = NULL;
    jni_env_destroy(g_env);
    g_env = NULL;
}
