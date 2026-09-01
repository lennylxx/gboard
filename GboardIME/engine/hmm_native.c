// Native method resolution — resolves JNI function pointers from RegisterNatives.
#include "hmm_internal.h"

void hmm_resolve_natives(void) {
    g_createFactory  = (fn_CreateFactory)  jni_find_registered_native_exact("nativeCreateEngineFactory");
    g_deleteFactory  = (fn_DeleteFactory)  jni_find_registered_native_exact("nativeDeleteEngineFactory");
    g_getDataManager = (fn_GetDataManager) jni_find_registered_native_exact("nativeGetDataManager");
    g_getSettingManager = (fn_GetSettingManager) jni_find_registered_native_exact("nativeGetSettingManager");
    g_createEngine   = (fn_CreateEngine)   jni_find_registered_native_exact("nativeCreateEngine");
    g_enrollScheme   = (fn_EnrollDataScheme)jni_find_registered_native_exact("nativeEnrollDataScheme");
    g_enrollFile     = (fn_EnrollDataFile) jni_find_registered_native_exact("nativeEnrollDataFile");
    g_enrollFd       = (fn_EnrollDataFd)   jni_find_registered_native_exact("nativeEnrollDataFd");
    g_enrollBuiltInScheme = (fn_EnrollBuiltInDataScheme) jni_find_registered_native_exact("nativeEnrollBuiltInDataScheme");
    g_enrollBuiltInData   = (fn_EnrollBuiltInData)       jni_find_registered_native_exact("nativeEnrollBuiltInData");
    g_enrollEmptyMutableDict = (fn_EnrollEmptyMutableDict) jni_find_registered_native_exact("nativeEnrollEmptyMutableDict");
    g_enrollSettingScheme = (fn_EnrollSettingScheme)      jni_find_registered_native_exact("nativeEnrollSettingScheme");
    g_loadBuiltInSettingScheme = (fn_LoadBuiltInSettingScheme) jni_find_registered_native_exact("nativeLoadBuiltInSettingScheme");
    g_append         = (fn_Append)         jni_find_registered_native_exact("nativeAppend");
    g_fillCandList   = (fn_FillCandList)   jni_find_registered_native_exact("nativeFillCandidateList");
    g_getCandCount   = (fn_GetCandCount)   jni_find_registered_native_exact("nativeGetCandidateCount");
    g_getCandString  = (fn_GetCandString)  jni_find_registered_native_exact("nativeGetCandidateString");
    g_getCandRange   = (fn_GetCandRange)   jni_find_registered_native_exact("nativeGetCandidateRange");
    g_selectCand     = (fn_SelectCand)     jni_find_registered_native_exact("nativeSelectCandidate");
    g_getDecodingRange = (fn_GetDecodingRange)jni_find_registered_native_exact("nativeGetDecodingRange");
    g_selectRange    = (fn_SelectRange)    jni_find_registered_native_exact("nativeSelectRange");
    g_reset          = (fn_Reset)          jni_find_registered_native_exact("nativeReset");
    g_setKeyLayout   = (fn_SetKeyLayout)   jni_find_registered_native_exact("nativeSetKeyboardLayout");
    g_beginSession   = (fn_BeginSession)   jni_find_registered_native_exact("nativeBeginSession");
    g_handleInputCtx = (fn_HandleInputContext) jni_find_registered_native_exact("nativeHandleInputContext");
    g_fillTokenCandList  = (fn_FillTokenCandList)  jni_find_registered_native_exact("nativeFillTokenCandidateList");
    g_getTokenCandCount  = (fn_GetTokenCandCount)  jni_find_registered_native_exact("nativeGetTokenCandidateCount");
    g_getTokenCandString = (fn_GetTokenCandString) jni_find_registered_native_exact("nativeGetTokenCandidateString");
    g_selectTokenCand    = (fn_SelectTokenCand)    jni_find_registered_native_exact("nativeSelectTokenCandidate");
    g_getSeparator       = (fn_GetSeparator)       jni_find_registered_native_exact("nativeGetSeparator");
    g_setSeparator       = (fn_SetSeparator)       jni_find_registered_native_exact("nativeSetSeparator");
    g_getSegmentCount    = (fn_GetSegmentCount)    jni_find_registered_native_exact("nativeGetSegmentCount");
    g_getSegment         = (fn_GetSegment)         jni_find_registered_native_exact("nativeGetSegment");
    g_getSegmentRange    = (fn_GetSegmentRange)    jni_find_registered_native_exact("nativeGetSegmentRange");
    g_getSegmentTokenCount = (fn_GetSegmentTokenCount) jni_find_registered_native_exact("nativeGetSegmentTokenCount");
    g_getSegmentToken    = (fn_GetSegmentToken)    jni_find_registered_native_exact("nativeGetSegmentToken");
    g_getTokenString     = (fn_GetTokenString)     jni_find_registered_native_by_sig("nativeGetTokenString", "(JJ)Ljava/lang/String;");

    LOGERR("Resolved: factory=%p dm=%p engine=%p enroll=%p append=%p fill=%p count=%p str=%p",
        (void*)g_createFactory, (void*)g_getDataManager, (void*)g_createEngine,
        (void*)g_enrollScheme, (void*)g_append, (void*)g_fillCandList,
        (void*)g_getCandCount, (void*)g_getCandString);
}
