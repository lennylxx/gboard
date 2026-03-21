#include "GboardBridge.h"
#include "../engine/hmm_engine.h"

bool gboard_init(const char *so_path, const char *pack_dir) {
    return hmm_engine_init(so_path, pack_dir);
}
bool gboard_append(const char *pinyin)            { return hmm_engine_append(pinyin); }
int  gboard_get_candidates(char **out, int max)   { return hmm_engine_get_candidates(out, max); }
bool gboard_select(int index)                     { return hmm_engine_select(index); }
int  gboard_get_candidate_consumed(int index)     { return hmm_engine_get_candidate_consumed(index); }
void gboard_reset(void)                           { hmm_engine_reset(); }
void gboard_destroy(void)                         { hmm_engine_destroy(); }
