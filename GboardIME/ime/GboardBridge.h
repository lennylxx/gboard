#pragma once
#include <stdbool.h>

bool  gboard_init(const char *so_path, const char *pack_dir);
bool  gboard_append(const char *pinyin);
int   gboard_get_candidates(char **out, int max);
bool  gboard_select(int index);
int   gboard_get_candidate_consumed(int index);
int   gboard_get_separator(int vertex_index);
bool  gboard_set_separator(int vertex_index, int type);
int   gboard_get_syllable_breaks(int *breaks, int max_breaks);
void  gboard_reset(void);
void  gboard_destroy(void);
