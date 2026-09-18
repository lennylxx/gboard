// Internal shared header for hmm_engine modules.
// Not part of the public API — use hmm_engine.h instead.
#pragma once

#include "config.h"
#include "hmm_engine.h"
#include "elf_loader.h"
#include "android_stubs.h"
#include "jni_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/time.h>
#include <sys/stat.h>

// ── Logging ─────────────────────────────────────────────────────────────────
#include <os/log.h>

static inline os_log_t hmm_os_log(void) {
    static os_log_t log;
    static int once;
    if (!once) { log = os_log_create(BUNDLE_ID, "hmm"); once = 1; }
    return log;
}

#if DEBUG
#define LOG(fmt, ...) do { \
    char _b[1024]; snprintf(_b, sizeof(_b), fmt, ##__VA_ARGS__); \
    os_log_debug(hmm_os_log(), "%{public}s", _b); \
} while(0)
#else
#define LOG(...) ((void)0)
#endif

#define LOGERR(fmt, ...) do { \
    char _b[1024]; snprintf(_b, sizeof(_b), fmt, ##__VA_ARGS__); \
    os_log_error(hmm_os_log(), "%{public}s", _b); \
} while(0)

// ── Crash protection ────────────────────────────────────────────────────────
extern sigjmp_buf s_hmm_jmp;
extern volatile void *s_crash_addr;

void hmm_crash_handler_sa(int sig, siginfo_t *info, void *ctx);
void hmm_alrm_handler(int sig);

#define CRASH_PROTECT_BEGIN() \
    { struct sigaction _sa = {0}, _old_segv, _old_bus, _old_ill, _old_abrt, _old_alrm; \
      _sa.sa_sigaction = hmm_crash_handler_sa; \
      _sa.sa_flags = SA_SIGINFO; \
      sigaction(SIGSEGV, &_sa, &_old_segv); sigaction(SIGBUS, &_sa, &_old_bus); \
      sigaction(SIGILL, &_sa, &_old_ill); sigaction(SIGABRT, &_sa, &_old_abrt); \
      { struct sigaction _alrm = {0}; _alrm.sa_handler = hmm_alrm_handler; \
        sigemptyset(&_alrm.sa_mask); sigaction(SIGALRM, &_alrm, &_old_alrm); } \
      struct itimerval _timer = {{0,0},{30,0}}; \
      setitimer(ITIMER_REAL, &_timer, NULL); \
      int _sig = sigsetjmp(s_hmm_jmp, 1); \
      if (_sig == 0) {
#define CRASH_PROTECT_END(label) \
      } else { \
        const char *_kind = (_sig == SIGALRM) ? "hung" : "crashed"; \
        char _b[256]; int _n=snprintf(_b,sizeof(_b),"[hmm_engine] %s %s (sig=%d addr=%p)\n",label,_kind,_sig,(void*)s_crash_addr); \
        write(STDERR_FILENO,_b,_n>0?(size_t)_n:0); } \
      { struct itimerval _zero = {{0,0},{0,0}}; setitimer(ITIMER_REAL, &_zero, NULL); } \
      sigaction(SIGSEGV, &_old_segv, NULL); sigaction(SIGBUS, &_old_bus, NULL); \
      sigaction(SIGILL, &_old_ill, NULL); sigaction(SIGABRT, &_old_abrt, NULL); \
      sigaction(SIGALRM, &_old_alrm, NULL); }

// ── JNI function pointer types ──────────────────────────────────────────────
// Registered via EngineFactory.initJNI → RegisterNatives.
// Obfuscated Java class hierarchy (deobfuscated names in parens):
//   EngineFactory    — creates factory, data/setting managers, engines
//   DataManager      — enrolls data schemes and data files
//   SettingManager   — enrolls setting schemes
//   HmmEngine        — runtime: append input, fill/get candidates, select, reset
//   ScoredInput      — input unit passed to nativeAppend (string + score + vertices)
//   Range            — vertex range passed to nativeFillCandidateList
typedef jlong    (*fn_CreateFactory)(JNIEnv *, jclass);
typedef void     (*fn_DeleteFactory)(JNIEnv *, jclass, jlong);
typedef jlong    (*fn_GetDataManager)(JNIEnv *, jclass, jlong);
typedef jlong    (*fn_GetSettingManager)(JNIEnv *, jclass, jlong);
typedef jlong    (*fn_CreateEngine)(JNIEnv *, jclass, jlong, jstring, jstring);
typedef jboolean (*fn_EnrollDataScheme)(JNIEnv *, jclass, jlong, jbyteArray);
typedef jboolean (*fn_EnrollDataFile)(JNIEnv *, jclass, jlong, jstring, jint, jstring);
typedef jboolean (*fn_EnrollDataFd)(JNIEnv *, jclass, jlong, jstring, jint, jobject, jint, jint);
typedef jboolean (*fn_EnrollBuiltInDataScheme)(JNIEnv *, jclass, jlong, jstring, jstring);
typedef jboolean (*fn_EnrollBuiltInData)(JNIEnv *, jclass, jlong, jstring, jint, jstring, jstring);
typedef jboolean (*fn_EnrollEmptyMutableDict)(JNIEnv *, jclass, jlong, jstring, jint, jint);
typedef jboolean (*fn_EnrollSettingScheme)(JNIEnv *, jclass, jlong, jstring, jstring, jbyteArray);
typedef jbyteArray (*fn_LoadBuiltInSettingScheme)(JNIEnv *, jclass, jlong, jstring, jstring);
typedef void     (*fn_CloseManager)(JNIEnv *, jclass, jlong);
typedef void     (*fn_RefreshData)(JNIEnv *, jclass, jlong);
typedef jint     (*fn_Append)(JNIEnv *, jobject, jlong, jobject, jint);
typedef jboolean (*fn_FillCandList)(JNIEnv *, jobject, jlong, jobject);
typedef jint     (*fn_GetCandCount)(JNIEnv *, jobject, jlong);
typedef jstring  (*fn_GetCandString)(JNIEnv *, jobject, jlong, jint);
typedef jobject  (*fn_GetCandRange)(JNIEnv *, jobject, jlong, jint);
typedef jboolean (*fn_SelectCand)(JNIEnv *, jobject, jlong, jint);
typedef jobject  (*fn_GetDecodingRange)(JNIEnv *, jobject, jlong);
typedef jboolean (*fn_SelectRange)(JNIEnv *, jobject, jlong, jobject);
typedef void     (*fn_Reset)(JNIEnv *, jobject, jlong);
typedef void     (*fn_SetKeyLayout)(JNIEnv *, jobject, jlong, jbyteArray);
typedef void     (*fn_BeginSession)(JNIEnv *, jobject, jlong, jbyteArray);
typedef void     (*fn_HandleInputContext)(JNIEnv *, jobject, jlong, jbyteArray);
typedef void     (*fn_FinishSession)(JNIEnv *, jobject, jlong, jbyteArray);
typedef jboolean (*fn_FillTokenCandList)(JNIEnv *, jobject, jlong, jobject);
typedef jint     (*fn_GetTokenCandCount)(JNIEnv *, jobject, jlong);
typedef jstring  (*fn_GetTokenCandString)(JNIEnv *, jobject, jlong, jint);
typedef jboolean (*fn_SelectTokenCand)(JNIEnv *, jobject, jlong, jint);
typedef jint     (*fn_GetSeparator)(JNIEnv *, jobject, jlong, jint);
typedef jboolean (*fn_SetSeparator)(JNIEnv *, jobject, jlong, jint, jint);
typedef jint     (*fn_GetSegmentCount)(JNIEnv *, jobject, jlong);
typedef jlong    (*fn_GetSegment)(JNIEnv *, jobject, jlong, jint);
typedef jobject  (*fn_GetSegmentRange)(JNIEnv *, jobject, jlong, jlong);
typedef jint     (*fn_GetSegmentTokenCount)(JNIEnv *, jobject, jlong, jlong);
typedef jlong    (*fn_GetSegmentToken)(JNIEnv *, jobject, jlong, jlong, jint);
typedef jstring  (*fn_GetTokenString)(JNIEnv *, jobject, jlong, jlong);
typedef jlong    (*fn_CreateMutableDictionaryAccessor)(JNIEnv *, jclass, jlong, jstring, jstring, jstring);
typedef jboolean (*fn_AddDictionaryCount)(JNIEnv *, jclass, jlong, jobject, jintArray, jstring, jint, jboolean);
typedef jboolean (*fn_DecreaseDictionaryCount)(JNIEnv *, jclass, jlong, jobject, jintArray, jstring, jint);
typedef jboolean (*fn_DuplicateDictionary)(JNIEnv *, jclass, jlong);
typedef jboolean (*fn_CompactDictionary)(JNIEnv *, jclass, jlong, jint);
typedef jint     (*fn_GetDictionarySize)(JNIEnv *, jclass, jlong);
typedef jboolean (*fn_PersistDictionary)(JNIEnv *, jclass, jlong, jstring);
typedef jboolean (*fn_NewEmptyDictionary)(JNIEnv *, jclass, jlong);
typedef jboolean (*fn_EnrollMutableDictFd)(JNIEnv *, jclass, jlong, jstring, jint, jobject, jint, jint, jint);
typedef jint     (*fn_GetCandidateTokenCount)(JNIEnv *, jobject, jlong, jint);
typedef jlong    (*fn_GetCandidateToken)(JNIEnv *, jobject, jlong, jint, jint);
typedef jint     (*fn_GetTokenLanguage)(JNIEnv *, jobject, jlong, jlong);

typedef struct {
    fn_CreateMutableDictionaryAccessor createAccessor;
    fn_AddDictionaryCount addCount;
    fn_DecreaseDictionaryCount decreaseCount;
    fn_DuplicateDictionary duplicateDictionary;
    fn_CompactDictionary compact;
    fn_GetDictionarySize getDictionarySize;
    fn_PersistDictionary persist;
    fn_RefreshData refreshData;
    fn_CloseManager closeAccessor;
    fn_NewEmptyDictionary newEmptyDictionary;
    fn_EnrollMutableDictFd enrollMutableDictFd;
    fn_GetCandidateTokenCount getCandidateTokenCount;
    fn_GetCandidateToken getCandidateToken;
    fn_GetTokenLanguage getTokenLanguage;
} HmmUserDictNatives;

// ── Global engine state ─────────────────────────────────────────────────────
extern ElfHandle *g_elf;
extern JNIEnv    *g_env;
extern jlong      g_factory;
extern jlong      g_dm;
extern jlong      g_sm;
extern jlong      g_engine;
extern jint       g_end_vertex;

extern fn_CreateFactory    g_createFactory;
extern fn_DeleteFactory    g_deleteFactory;
extern fn_GetDataManager   g_getDataManager;
extern fn_GetSettingManager g_getSettingManager;
extern fn_CreateEngine     g_createEngine;
extern fn_EnrollDataScheme g_enrollScheme;
extern fn_EnrollDataFile   g_enrollFile;
extern fn_EnrollDataFd     g_enrollFd;
extern fn_EnrollBuiltInDataScheme g_enrollBuiltInScheme;
extern fn_EnrollBuiltInData g_enrollBuiltInData;
extern fn_EnrollEmptyMutableDict g_enrollEmptyMutableDict;
extern fn_EnrollSettingScheme g_enrollSettingScheme;
extern fn_LoadBuiltInSettingScheme g_loadBuiltInSettingScheme;
extern fn_Append           g_append;
extern fn_FillCandList     g_fillCandList;
extern fn_GetCandCount     g_getCandCount;
extern fn_GetCandString    g_getCandString;
extern fn_GetCandRange     g_getCandRange;
extern fn_SelectCand       g_selectCand;
extern fn_GetDecodingRange g_getDecodingRange;
extern fn_SelectRange      g_selectRange;
extern fn_Reset            g_reset;
extern fn_SetKeyLayout     g_setKeyLayout;
extern fn_BeginSession     g_beginSession;
extern fn_HandleInputContext g_handleInputCtx;
extern fn_FinishSession    g_finishSession;
extern fn_FillTokenCandList g_fillTokenCandList;
extern fn_GetTokenCandCount g_getTokenCandCount;
extern fn_GetTokenCandString g_getTokenCandString;
extern fn_SelectTokenCand  g_selectTokenCand;
extern fn_GetSeparator     g_getSeparator;
extern fn_SetSeparator     g_setSeparator;
extern fn_GetSegmentCount  g_getSegmentCount;
extern fn_GetSegment       g_getSegment;
extern fn_GetSegmentRange  g_getSegmentRange;
extern fn_GetSegmentTokenCount g_getSegmentTokenCount;
extern fn_GetSegmentToken  g_getSegmentToken;
extern fn_GetTokenString   g_getTokenString;
extern fn_RefreshData      g_refreshData;

// Refresh the decoder after a mutable dictionary snapshot is re-enrolled.
bool hmm_engine_refresh_user_dictionary(void);
void hmm_engine_set_external_context(bool has_context);

// ── Symbol helper ───────────────────────────────────────────────────────────
static inline void *hmm_sym(ElfHandle *h, const char *name) {
    void *p = elf_sym(h, name);
    if (!p) { char _b[256]; int _n=snprintf(_b,sizeof(_b),"[hmm_engine] missing symbol: %s\n",name); write(STDERR_FILENO,_b,_n>0?(size_t)_n:0); }
    return p;
}

// ── Module init functions ───────────────────────────────────────────────────
void hmm_resolve_natives(void);
const HmmUserDictNatives *hmm_get_user_dict_natives(void);
bool hmm_enroll_all(const char *pack_dir);
