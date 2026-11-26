#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "../engine/hmm_engine.h"
#include "../engine/hmm_user_dict.h"

bool  gboard_init(const char *so_path, const char *pack_dir);
bool  gboard_init_with_user_data(const char *so_path, const char *pack_dir,
                                  const char *user_data_dir);
bool  gboard_set_fuzzy_pinyin(uint32_t groups);
uint32_t gboard_get_fuzzy_pinyin(void);
bool  gboard_set_pinyin_scheme(HmmPinyinScheme scheme);
HmmPinyinScheme gboard_get_pinyin_scheme(void);
bool  gboard_set_shortcuts_enabled(bool enabled);
bool  gboard_get_shortcuts_enabled(void);
bool  gboard_set_pinyin_correction(bool enabled);
bool  gboard_get_pinyin_correction(void);
void  gboard_set_shuangpin_key_correction(bool enabled);
bool  gboard_get_shuangpin_key_correction(void);
bool  gboard_input_edges_stale(void);
bool  gboard_shortcut_is_valid(const char *shortcut, const char *phrase);
// Replaces every user shortcut; see hmm_user_dict_replace_shortcuts.
bool  gboard_replace_shortcuts(const HmmShortcut *entries, int count);
// True when the candidate is exactly one user-defined shortcut phrase.
bool  gboard_is_shortcut_candidate(int candidate_index);
bool  gboard_set_context(const char *text_before_cursor);
bool  gboard_update_input_context(const char *before_selection,
                                  const char *selected_text,
                                  const char *after_selection);
void  gboard_prepare_input(const char *pinyin);
bool  gboard_append(const char *pinyin);
int   gboard_get_candidates(char **out, int max);
int   gboard_get_candidates_page(char **out, int offset, int max);
int   gboard_get_predictions(char **out, int max);
bool  gboard_select(int index);
bool  gboard_unconvert_last(void);
int   gboard_get_converted_count(void);
bool  gboard_delete_last_input(void);
uint64_t gboard_input_generation(void);
bool  gboard_can_edit_in_place(uint64_t generation, int letters,
                               int next_letters);
int   gboard_get_candidate_consumed(int index);
int   gboard_get_separator(int vertex_index);
bool  gboard_set_separator(int vertex_index, int type);
int   gboard_get_segmented_pinyin(char *text, int max_bytes);
int   gboard_get_corrected_candidate_reading(int index,
                                             HmmTokenReading *tokens,
                                             int max_tokens);
void  gboard_reset(void);
void  gboard_destroy(void);

// u-mode (拆字) lookup tables, generated from the TSV files at build time
// by scripts/gen_chaizi_tables.py. Each `*_starts`/`*_offsets` array has
// one more element than the items it delimits.
typedef struct {
    // Distinct readings, concatenated lowercase ASCII.
    const char *reading_pool;
    const uint32_t *reading_offsets;
    // Reading ids of each component, preferred first.
    const uint16_t *component_reading_ids;
    const uint32_t *component_reading_starts;
    // Component ids of each split.
    const uint16_t *split_components;
    const uint32_t *split_starts;
    // Characters in encoding order, UTF-8 concatenated.
    const char *character_pool;
    const uint32_t *character_starts;
    // Splits of each character, authored first, then expanded.
    const uint32_t *entry_split_starts;
    // Bit n: some split's first component has a reading starting 'a' + n.
    const uint32_t *entry_first_letters;
    uint32_t entry_count;
} ChaiziTables;

const ChaiziTables *gboard_chaizi_tables(void);

// ── User dictionary ──────────────────────────────────────────────────────────

// Learn a committed candidate (tokens + types extracted from engine).
bool  gboard_user_dict_learn(const char **tokens, const int *token_types,
                              int token_count, const char *value,
                              bool is_full_match);
// Undo learning of a previously committed candidate.
bool  gboard_user_dict_unlearn(const char **tokens, const int *token_types,
                                int token_count, const char *value);
// True if the candidate came only from a user dictionary and can be removed.
bool  gboard_user_dict_is_candidate_removable(int candidate_index);
// Remove a learned candidate from the user dictionaries and decoder list.
bool  gboard_user_dict_remove_candidate(int candidate_index);
// True while a saved snapshot awaits the decoder refresh at the next reset.
bool  gboard_user_dict_decoder_refresh_pending(void);
// Learned word management; see hmm_user_dict.h.
int   gboard_user_dict_export_entries(HmmUserDictEntry **out);
void  gboard_user_dict_free_entries(HmmUserDictEntry *entries, int count);
bool  gboard_user_dict_remove_entry(const HmmUserDictEntry *entry);
bool  gboard_user_dict_set_entry_count(const HmmUserDictEntry *entry, int count,
                                       bool full_match);
// Persist the user dictionary to disk.
bool  gboard_user_dict_persist(void);
// Get the current dictionary size.
int   gboard_user_dict_get_size(void);
// Clear all learned entries.
bool  gboard_user_dict_clear(void);
// Check if user dictionary is initialized.
bool  gboard_user_dict_is_ready(void);
// Extract token info for a candidate (must be called after get_candidates).
// tokens: char[][16] array, types: int[] array, max_tokens: array size.
// Returns number of tokens extracted.
int   gboard_user_dict_extract_tokens(int candidate_index,
                                       char tokens[][16], int *types, int max_tokens);

// Simplified token extraction for Swift: copies token string at given index.
// Must be called after gboard_user_dict_extract_tokens.
// Returns token count, and fills token_out (up to 16 chars) and type_out for given idx.
int   gboard_user_dict_extract_token_count(int candidate_index);
bool  gboard_user_dict_get_token(int candidate_index, int token_index,
                                  char *token_out, int *type_out);
