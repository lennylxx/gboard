#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the Gboard HMM Pinyin engine.
// so_path:      path to libintegrated_shared_object.so
// pack_dir:     path to hmmoemdata/zh_cn_2025090307/
// Returns true on success.
bool hmm_engine_init(const char *so_path, const char *pack_dir);

// Append one or more pinyin key characters (e.g. "n", "ni", "nihao").
// Returns true if the engine accepted the input.
bool hmm_engine_append(const char *pinyin_input);

// Fill the candidate list.  candidates[] is an array of UTF-8 strings.
// max_count: size of candidates[].
// Returns the number of candidates filled.
int  hmm_engine_get_candidates(char **candidates, int max_count);

// Commit candidate at index (0-based) and reset composition.
bool hmm_engine_select(int index);

// Reset / clear current composition without committing.
void hmm_engine_reset(void);

// Tear down.
void hmm_engine_destroy(void);

#ifdef __cplusplus
}
#endif
