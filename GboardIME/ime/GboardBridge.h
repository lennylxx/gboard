#pragma once
#include <stdbool.h>

bool  gboard_init(const char *so_path, const char *pack_dir);
bool  gboard_init_with_user_data(const char *so_path, const char *pack_dir,
                                  const char *user_data_dir);
bool  gboard_append(const char *pinyin);
int   gboard_get_candidates(char **out, int max);
int   gboard_get_candidates_page(char **out, int offset, int max);
bool  gboard_select(int index);
int   gboard_get_candidate_consumed(int index);
int   gboard_get_separator(int vertex_index);
bool  gboard_set_separator(int vertex_index, int type);
int   gboard_get_syllable_breaks(int *breaks, int max_breaks);
void  gboard_reset(void);
void  gboard_destroy(void);

// ── User dictionary ──────────────────────────────────────────────────────────

// Learn a committed candidate (tokens + types extracted from engine).
bool  gboard_user_dict_learn(const char **tokens, const int *token_types,
                              int token_count, const char *value,
                              bool is_full_match);
// Undo learning of a previously committed candidate.
bool  gboard_user_dict_unlearn(const char **tokens, const int *token_types,
                                int token_count, const char *value);
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
