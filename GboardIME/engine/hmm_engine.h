#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the Gboard HMM Pinyin engine.
// so_path:      path to libintegrated_shared_object.so
// pack_dir:     path to hmmoemdata/current/
// Returns true on success.
bool hmm_engine_init(const char *so_path, const char *pack_dir);

// Initialize with user data directory for persistent user dictionary.
// user_data_dir: path to writable directory for user dictionary files
//                (e.g. ~/Library/Application Support/GboardIME/).
//                NULL disables user dictionary persistence.
bool hmm_engine_init_with_user_data(const char *so_path, const char *pack_dir,
                                     const char *user_data_dir);

enum {
    HMM_FUZZY_Z_ZH = 1u << 0,
    HMM_FUZZY_C_CH = 1u << 1,
    HMM_FUZZY_S_SH = 1u << 2,
    HMM_FUZZY_AN_ANG = 1u << 3,
    HMM_FUZZY_EN_ENG = 1u << 4,
    HMM_FUZZY_IN_ING = 1u << 5,
    HMM_FUZZY_L_N = 1u << 6,
    HMM_FUZZY_F_H = 1u << 7,
    HMM_FUZZY_R_L = 1u << 8,
    HMM_FUZZY_K_G = 1u << 9,
    HMM_FUZZY_IAN_IANG = 1u << 10,
    HMM_FUZZY_UAN_UANG = 1u << 11,
    HMM_FUZZY_ALL = (1u << 12) - 1
};

// Enroll updated settings without replacing decoders or disturbing input.
// Each decoder adopts them on its next reset via nativeRefreshData.
// To update active candidates immediately, reset and replay the composition.
// Failed enrollment retains the previous configuration and composition.
bool hmm_engine_set_fuzzy_pinyin(uint32_t groups);
uint32_t hmm_engine_get_fuzzy_pinyin(void);

typedef enum {
    HMM_PINYIN_FULL = 0,
    HMM_PINYIN_SHUANGPIN_ABC,
    HMM_PINYIN_SHUANGPIN_FLYPY,
    HMM_PINYIN_SHUANGPIN_JIAJIA,
    HMM_PINYIN_SHUANGPIN_MS,
    HMM_PINYIN_SHUANGPIN_ZIGUANG,
    HMM_PINYIN_SHUANGPIN_ZIRANMA
} HmmPinyinScheme;

// Like fuzzy settings, a new scheme is adopted on the next decoder reset.
// Call only at a composition boundary; raw keys belong to their old scheme.
bool hmm_engine_set_pinyin_scheme(HmmPinyinScheme scheme);
HmmPinyinScheme hmm_engine_get_pinyin_scheme(void);

// Set the committed text immediately before the cursor. The engine keeps the
// same trailing context window as Gboard Java: 5 Chinese UTF-16 code units or
// 20 Latin UTF-16 code units, stopping at a language or punctuation boundary.
// Call before appending a new composition.
bool hmm_engine_set_context(const char *text_before_cursor);

// Submit the official HMM around-cursor snapshot for the active native
// session. Strings are UTF-8 and may be empty.
bool hmm_engine_update_input_context(const char *before_selection,
                                     const char *selected_text,
                                     const char *after_selection);

// Select the ranking policy for a complete pinyin composition before reset.
void hmm_engine_prepare_input(const char *pinyin_input);

// Append one or more pinyin key characters (e.g. "n", "ni", "nihao").
// Returns true if the engine accepted the input.
bool hmm_engine_append(const char *pinyin_input);

// Fill the candidate list.  candidates[] is an array of UTF-8 strings.
// max_count: size of candidates[].
// Returns the number of candidates filled.
int  hmm_engine_get_candidates(char **candidates, int max_count);

// Fill a page of candidates starting at the given 0-based engine index.
int  hmm_engine_get_candidates_page(char **candidates, int offset, int max_count);

// Fill next-word predictions for the context set by hmm_engine_set_context().
// Discards any active composition and leaves the engine reset. Returns 0 when
// the normalized context is empty. Caller frees each returned string.
int  hmm_engine_get_predictions(char **candidates, int max_count);

// Commit candidate at index (0-based) and reset composition.
bool hmm_engine_select(int index);

// Get the vertex range consumed by candidate at index.
// Returns the end vertex index (number of pinyin chars consumed from start).
// Returns -1 on failure.
int hmm_engine_get_candidate_consumed(int index);

// Get separator type at vertex position i.
// Returns: 0 = none, 1 = token separator, 2 = segment separator. -1 on error.
int hmm_engine_get_separator(int vertex_index);

// Set separator type at vertex position.
// separator_type: 0 = none, 1 = token separator, 2 = segment separator.
bool hmm_engine_set_separator(int vertex_index, int separator_type);

// Get the current composing reading using the same segment/token traversal as
// Gboard's Java AbstractHmmEngineWrapper.e(), with apostrophes inserted between
// tokens so the candidate window can visualize the engine's actual split.
// Must be called after hmm_engine_get_candidates().
// Returns the number of UTF-8 bytes written, excluding the trailing NUL.
int hmm_engine_get_segmented_pinyin(char *text, int max_bytes);

enum { HMM_TOKEN_TEXT_MAX = 16, HMM_TOKEN_LANGUAGE_LATIN = 0 };

typedef struct {
    char raw[HMM_TOKEN_TEXT_MAX];         // typed spelling, e.g. "hoa"
    char normalized[HMM_TOKEN_TEXT_MAX];  // corrected spelling, e.g. "hao"
    int language;                         // HMM_TOKEN_LANGUAGE_LATIN for English
} HmmTokenReading;

// Fills the token readings of the candidate at index when the engine's pinyin
// corrector produced it (e.g. "nihoa" -> 你好). Returns the token count, or 0
// when the candidate is not corrected, has more than max_tokens tokens, or the
// engine fails. Must be called after hmm_engine_get_candidates().
int hmm_engine_get_corrected_candidate_reading(int index,
                                               HmmTokenReading *tokens,
                                               int max_tokens);

// Reset / clear current composition without committing.
void hmm_engine_reset(void);

// Tear down.
void hmm_engine_destroy(void);

#ifdef __cplusplus
}
#endif
