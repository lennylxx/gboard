// User dictionary (user_dict_3_3 and user_dict_3_3_english) learning support.
// Wraps MutableDictionaryAccessorImpl JNI calls for learning, persistence, and reload.
#pragma once

#include <stdbool.h>
#include "hmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the user dictionary subsystem.
// Called during engine initialization after data enrollment and before decoder
// creation. Resolves accessor natives and loads persisted data if available.
// user_data_dir: directory for persisted dictionary files
//                (e.g. ~/Library/Application Support/GboardIME/).
//                NULL disables persistence (in-memory only).
// pack_dir:     path to data pack (for loading setting scheme).
//               NULL if not available.
// Returns true if accessor was created successfully.
bool hmm_user_dict_init(const char *user_data_dir, const char *pack_dir);

// Learn a committed candidate.
// tokens:      array of pinyin syllable strings (e.g. ["ni", "hao"])
// token_types: array of token type IDs (parallel to tokens)
// token_count: number of tokens
// value:       the committed Chinese text (e.g. "你好")
// is_full_match: whether the candidate consumes the full composition
// Returns true on success.
bool hmm_user_dict_learn(const char **tokens, const int *token_types,
                         int token_count, const char *value,
                         bool is_full_match);

// Undo learning of a previously committed candidate (decrements count).
// Same parameters as hmm_user_dict_learn.
bool hmm_user_dict_unlearn(const char **tokens, const int *token_types,
                           int token_count, const char *value);

// Returns true when the candidate at the given engine index comes only from a
// user dictionary, so it can be removed. Must be called after the candidate
// list is filled.
bool hmm_user_dict_is_candidate_removable(int candidate_index);

// Removes a learned candidate: deletes its text from both user dictionaries
// and from the decoder's current candidate list. Refill candidates afterwards.
// Returns false when the candidate is not removable or nothing was removed.
bool hmm_user_dict_remove_candidate(int candidate_index);

// ── Learned word management ────────────────────────────────────────────────

/* The native stores keys of any length, and learning joins consecutive
 * selections, so the token count only guards against malformed input.
 * Token bytes match the limit used when extracting candidate tokens. */
#define HMM_USER_DICT_MAX_TOKENS 1024
#define HMM_USER_DICT_MAX_TOKEN_BYTES 15

typedef enum {
    HMM_USER_DICT_PINYIN = 0,   // user_dict_3_3
    HMM_USER_DICT_ENGLISH = 1,  // user_dict_3_3_english
} HmmUserDictKind;

// Token languages are values of Google's i18n Language enum. The pinyin
// dictionary accepts only these two; the English one stores only English.
#define HMM_LANGUAGE_ENGLISH 0
#define HMM_LANGUAGE_CHINESE 16  // zh-CN

// One learned MutableDictionaryAccessor$Entry. The native Entry.e
// (modified since the last sync export) is not exposed; Entry.f is the
// match type, where 2 marks keys derived from abbreviations, which
// nativeExportAllEntries never returns.
typedef struct {
    HmmUserDictKind dictionary;
    char *value;
    char **tokens;
    int *languages;
    int token_count;
    int count;
    bool full_match;  // Entry.f == 0; false when learned from a partial match
} HmmUserDictEntry;

// Lists the learned entries of both user dictionaries (nativeExportAllEntries).
// Entries with a zero count are removal tombstones and are skipped.
// Entries learned from abbreviations are not exported by the native code.
// Returns the entry count, or -1 on failure; free with
// hmm_user_dict_free_entries.
int hmm_user_dict_export_entries(HmmUserDictEntry **out);
void hmm_user_dict_free_entries(HmmUserDictEntry *entries, int count);

// Removes one entry by its reading, token languages and text
// (nativeRemoveByTokenAndValue). Persist to keep the change.
bool hmm_user_dict_remove_entry(const HmmUserDictEntry *entry);

// Inserts the entry or sets its count to exactly `count` (positive) with
// the given match type. `entry` holds the current count and match type,
// which are restored if the change fails (count 0 for a new entry).
// Persist to keep the change.
bool hmm_user_dict_set_entry_count(const HmmUserDictEntry *entry, int count,
                                   bool full_match);

// Persist the user dictionary to disk.
// Safe rename sequence: duplicate → compact → persist to _tmp →
// rename primary→_bak → rename _tmp→primary → delete _bak.
// Returns true on success.
bool hmm_user_dict_persist(void);

// Get the current dictionary size (number of entries).
int hmm_user_dict_get_size(void);

// Clear all learned entries (resets to empty dictionary).
// After clearing, the engine sees an empty user dictionary.
bool hmm_user_dict_clear(void);

// Tear down the user dictionary subsystem.
// Forces a persist if persistence is configured.
void hmm_user_dict_destroy(void);

// Returns true if the user dictionary subsystem is initialized.
bool hmm_user_dict_is_ready(void);
bool hmm_user_dict_set_pinyin_scheme(HmmPinyinScheme scheme);
bool hmm_user_dict_pinyin_settings_valid(void);

// Apply a dictionary snapshot enrolled by persist after the decoder resets.
void hmm_user_dict_refresh_decoder_if_needed(void);
// True while a persisted snapshot awaits the next decoder reset.
bool hmm_user_dict_decoder_refresh_pending(void);

// Extract token information for the candidate at the given engine index.
// Must be called after hmm_engine_get_candidates() fills the candidate list.
// Writes syllable strings into tokens[] and type IDs into types[].
// Returns the number of tokens extracted, or 0 on failure or when the
// candidate has more than max_tokens tokens.
int hmm_user_dict_extract_tokens(int candidate_index,
                                 char tokens[][16], int *types, int max_tokens);

#ifdef __cplusplus
}
#endif
