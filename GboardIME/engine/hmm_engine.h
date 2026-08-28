#pragma once
#include <stddef.h>
#include <stdbool.h>

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

// Append one or more pinyin key characters (e.g. "n", "ni", "nihao").
// Returns true if the engine accepted the input.
bool hmm_engine_append(const char *pinyin_input);

// Fill the candidate list.  candidates[] is an array of UTF-8 strings.
// max_count: size of candidates[].
// Returns the number of candidates filled.
int  hmm_engine_get_candidates(char **candidates, int max_count);

// Fill a page of candidates starting at the given 0-based engine index.
int  hmm_engine_get_candidates_page(char **candidates, int offset, int max_count);

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

// Get syllable boundary positions from the engine's segment/token structure.
// Must be called after hmm_engine_get_candidates().
// Writes vertex positions where syllable breaks occur into breaks[].
// Returns the number of breaks written, or 0 on failure.
int hmm_engine_get_syllable_breaks(int *breaks, int max_breaks);

// Reset / clear current composition without committing.
void hmm_engine_reset(void);

// Tear down.
void hmm_engine_destroy(void);

#ifdef __cplusplus
}
#endif
