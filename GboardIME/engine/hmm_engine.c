// Wrapper around Gboard's native HMM Pinyin engine.
// Loads libintegrated_shared_object.so via our ELF loader and calls
// the JNI entry points directly.
//
// This file is the orchestration layer. The heavy lifting lives in:
//   hmm_native.c     — JNI function pointer resolution
//   hmm_enroll.c     — data/setting scheme enrollment
//   hmm_candidates.c — append, candidates, select, reset

#include "hmm_internal.h"

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
fn_RefreshData      g_refreshData      = NULL;
fn_Append           g_append           = NULL;
fn_FillCandList     g_fillCandList     = NULL;
fn_GetCandCount     g_getCandCount     = NULL;
fn_GetCandString    g_getCandString    = NULL;
fn_SelectCand       g_selectCand       = NULL;
fn_Reset            g_reset            = NULL;
fn_SetKeyLayout     g_setKeyLayout     = NULL;
fn_BeginSession     g_beginSession     = NULL;
fn_HandleInputContext g_handleInputCtx  = NULL;
fn_FillTokenCandList g_fillTokenCandList = NULL;
fn_GetTokenCandCount g_getTokenCandCount = NULL;
fn_GetTokenCandString g_getTokenCandString = NULL;
fn_SelectTokenCand  g_selectTokenCand  = NULL;

// ── Public API ──────────────────────────────────────────────────────────────

bool hmm_engine_init(const char *so_path, const char *pack_dir) {
    LOG("hmm_engine_init: so=%s pack=%s", so_path, pack_dir);

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
        hmm_enroll_all(pack_dir);
    }

    // 6. Create HMM engine
    {
        const char *etypes[] = { "zh-t-i0-pinyin-x-f0-delight", "zh-t-i0-pinyin", NULL };
        for (int ei = 0; etypes[ei] && !g_engine; ei++) {
            jstring engine_type = jni_NewStringUTF(g_env, etypes[ei]);
            jstring user_id    = jni_NewStringUTF(g_env, "");
            CRASH_PROTECT_BEGIN()
            g_engine = g_createEngine(g_env, NULL, g_factory, engine_type, user_id);
            LOGERR("nativeCreateEngine('%s') → %lld", etypes[ei], (long long)g_engine);
            CRASH_PROTECT_END("nativeCreateEngine")
        }
    }

    if (!g_engine) {
        LOGERR("Failed to create engine");
        return false;
    }

    // 7. Set keyboard layout
    if (g_setKeyLayout && pack_dir) {
        char scheme_path[4096];
        snprintf(scheme_path, sizeof(scheme_path), "%s/pinyin_qwerty_setting_scheme", pack_dir);
        FILE *sfp = fopen(scheme_path, "rb");
        if (sfp) {
            fseek(sfp, 0, SEEK_END);
            long ssz = ftell(sfp);
            fseek(sfp, 0, SEEK_SET);
            uint8_t *sbuf = malloc((size_t)ssz);
            fread(sbuf, 1, (size_t)ssz, sfp);
            fclose(sfp);
            jbyteArray layout = jni_NewByteArray(g_env, (jsize)ssz);
            jni_SetByteArrayRegion(g_env, layout, 0, (jsize)ssz, (jbyte*)sbuf);
            free(sbuf);
            CRASH_PROTECT_BEGIN()
            g_setKeyLayout(g_env, NULL, g_engine, layout);
            LOG("setKeyboardLayout OK (%ld bytes)", ssz);
            CRASH_PROTECT_END("nativeSetKeyboardLayout")
        } else {
            LOG("pinyin_qwerty_setting_scheme not found");
        }
    }

    // 8. Begin session
    if (g_beginSession && g_engine) {
        jbyteArray session_cfg = jni_NewByteArray(g_env, 0);
        CRASH_PROTECT_BEGIN()
        g_beginSession(g_env, NULL, g_engine, session_cfg);
        LOGERR("nativeBeginSession OK");
        CRASH_PROTECT_END("nativeBeginSession")
    }

    LOGERR("init complete! engine=%lld", (long long)g_engine);
    LOG("append=%p fillCand=%p getCandCount=%p getCandStr=%p",
        (void*)g_append, (void*)g_fillCandList, (void*)g_getCandCount, (void*)g_getCandString);
    return true;
}

void hmm_engine_destroy(void) {
    if (g_deleteFactory && g_factory)
        g_deleteFactory(g_env, NULL, g_factory);
    jni_env_destroy(g_env);
    elf_unload(g_elf);
    g_elf = NULL; g_env = NULL; g_factory = 0; g_dm = 0; g_engine = 0;
}
