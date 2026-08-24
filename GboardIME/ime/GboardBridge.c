#include "GboardBridge.h"
#include "../engine/hmm_engine.h"
#include "../engine/hmm_user_dict.h"
#include <string.h>

bool gboard_init(const char *so_path, const char *pack_dir) {
    return hmm_engine_init(so_path, pack_dir);
}

bool gboard_init_with_user_data(const char *so_path, const char *pack_dir,
                                 const char *user_data_dir) {
    return hmm_engine_init_with_user_data(so_path, pack_dir, user_data_dir);
}

bool gboard_append(const char *pinyin)              { return hmm_engine_append(pinyin); }
int  gboard_get_candidates(char **out, int max)     { return hmm_engine_get_candidates(out, max); }
int  gboard_get_candidates_page(char **out, int offset, int max) {
    return hmm_engine_get_candidates_page(out, offset, max);
}
bool gboard_select(int index)                       { return hmm_engine_select(index); }
int  gboard_get_candidate_consumed(int index)       { return hmm_engine_get_candidate_consumed(index); }
int  gboard_get_separator(int i)                    { return hmm_engine_get_separator(i); }
bool gboard_set_separator(int i, int t)             { return hmm_engine_set_separator(i, t); }
int  gboard_get_syllable_breaks(int *breaks, int m) { return hmm_engine_get_syllable_breaks(breaks, m); }
void gboard_reset(void)                             { hmm_engine_reset(); }
void gboard_destroy(void)                           { hmm_engine_destroy(); }

// ── User dictionary ──────────────────────────────────────────────────────────

bool gboard_user_dict_learn(const char **tokens, const int *token_types,
                             int token_count, const char *value,
                             bool is_full_match) {
    return hmm_user_dict_learn(tokens, token_types, token_count, value,
                               is_full_match);
}

bool gboard_user_dict_unlearn(const char **tokens, const int *token_types,
                               int token_count, const char *value) {
    return hmm_user_dict_unlearn(tokens, token_types, token_count, value);
}

bool gboard_user_dict_persist(void)   { return hmm_user_dict_persist(); }
int  gboard_user_dict_get_size(void)  { return hmm_user_dict_get_size(); }
bool gboard_user_dict_clear(void)     { return hmm_user_dict_clear(); }
bool gboard_user_dict_is_ready(void)  { return hmm_user_dict_is_ready(); }

int gboard_user_dict_extract_tokens(int candidate_index,
                                     char tokens[][16], int *types, int max_tokens) {
    return hmm_user_dict_extract_tokens(candidate_index, tokens, types, max_tokens);
}

// Cache for simplified per-token access from Swift
static char s_cached_tokens[32][16];
static int  s_cached_types[32];
static int  s_cached_count = 0;
static int  s_cached_cand_idx = -1;

int gboard_user_dict_extract_token_count(int candidate_index) {
    s_cached_cand_idx = candidate_index;
    s_cached_count = hmm_user_dict_extract_tokens(candidate_index,
                                                   s_cached_tokens, s_cached_types, 32);
    return s_cached_count;
}

bool gboard_user_dict_get_token(int candidate_index, int token_index,
                                 char *token_out, int *type_out) {
    // Re-extract if candidate index changed
    if (candidate_index != s_cached_cand_idx) {
        gboard_user_dict_extract_token_count(candidate_index);
    }
    if (token_index < 0 || token_index >= s_cached_count) return false;
    if (token_out) {
        strncpy(token_out, s_cached_tokens[token_index], 15);
        token_out[15] = '\0';
    }
    if (type_out) *type_out = s_cached_types[token_index];
    return true;
}
