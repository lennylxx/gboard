#include "GboardBridge.h"
#include "../engine/hmm_engine.h"

bool gboard_init(const char *so_path, const char *pack_dir) {
    return hmm_engine_init(so_path, pack_dir);
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
