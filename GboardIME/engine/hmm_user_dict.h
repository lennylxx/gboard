// User dictionary (user_dict_3_3) automatic learning support.
// Wraps MutableDictionaryAccessorImpl JNI calls for learning, persistence, and reload.
#pragma once

#include <stdbool.h>

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

// Apply a dictionary snapshot enrolled by persist after the decoder resets.
void hmm_user_dict_refresh_decoder_if_needed(void);

// Extract token information for the candidate at the given engine index.
// Must be called after hmm_engine_get_candidates() fills the candidate list.
// Writes syllable strings into tokens[] and type IDs into types[].
// Returns the number of tokens extracted, or 0 on failure.
int hmm_user_dict_extract_tokens(int candidate_index,
                                 char tokens[][16], int *types, int max_tokens);

#ifdef __cplusplus
}
#endif
