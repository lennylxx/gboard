// Native method resolution — resolves JNI function pointers from RegisterNatives.
#include "hmm_internal.h"

static HmmUserDictNatives s_userDictNatives;

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
    g_finishSession  = (fn_FinishSession)  jni_find_registered_native_exact("nativeFinishSession");
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
    g_refreshData        = (fn_RefreshData) jni_find_registered_native_by_sig("nativeRefreshData", "(J)V");

    s_userDictNatives.createAccessor = (fn_CreateMutableDictionaryAccessor) jni_find_registered_native_by_sig("nativeCreateMutableDictionaryAccessor", "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)J");
    s_userDictNatives.addCount = (fn_AddDictionaryCount) jni_find_registered_native_by_sig("nativeAddCount", "(J[Ljava/lang/String;[ILjava/lang/String;IZ)Z");
    s_userDictNatives.decreaseCount = (fn_DecreaseDictionaryCount) jni_find_registered_native_by_sig("nativeDecreaseCount", "(J[Ljava/lang/String;[ILjava/lang/String;I)Z");
    s_userDictNatives.duplicateDictionary = (fn_DuplicateDictionary) jni_find_registered_native_by_sig("nativeDuplicateDictionary", "(J)Z");
    s_userDictNatives.compact = (fn_CompactDictionary) jni_find_registered_native_by_sig("nativeCompact", "(JI)Z");
    s_userDictNatives.getDictionarySize = (fn_GetDictionarySize) jni_find_registered_native_by_sig("nativeGetDictionarySize", "(J)I");
    s_userDictNatives.persist = (fn_PersistDictionary) jni_find_registered_native_by_sig("nativePersist", "(JLjava/lang/String;)Z");
    s_userDictNatives.refreshData = g_refreshData;
    s_userDictNatives.closeAccessor = (fn_CloseManager) jni_find_registered_native_by_sig("nativeClose", "(J)V");
    s_userDictNatives.newEmptyDictionary = (fn_NewEmptyDictionary) jni_find_registered_native_by_sig("nativeNewEmptyDictionary", "(J)Z");
    s_userDictNatives.enrollMutableDictFd = (fn_EnrollMutableDictFd) jni_find_registered_native_by_sig("nativeEnrollMutableDictFd", "(JLjava/lang/String;ILjava/io/FileDescriptor;III)Z");
    s_userDictNatives.getCandidateTokenCount = (fn_GetCandidateTokenCount) jni_find_registered_native_by_sig("nativeGetCandidateTokenCount", "(JI)I");
    s_userDictNatives.getCandidateToken = (fn_GetCandidateToken) jni_find_registered_native_by_sig("nativeGetCandidateToken", "(JII)J");
    s_userDictNatives.getTokenLanguage = (fn_GetTokenLanguage) jni_find_registered_native_by_sig("nativeGetTokenLanguage", "(JJ)I");

    LOGERR("Resolved: factory=%p dm=%p engine=%p enroll=%p append=%p fill=%p count=%p str=%p",
        (void*)g_createFactory, (void*)g_getDataManager, (void*)g_createEngine,
        (void*)g_enrollScheme, (void*)g_append, (void*)g_fillCandList,
        (void*)g_getCandCount, (void*)g_getCandString);
}

const HmmUserDictNatives *hmm_get_user_dict_natives(void) {
    return &s_userDictNatives;
}
