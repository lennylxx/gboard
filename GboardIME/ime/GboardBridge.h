#pragma once
#include <stdbool.h>

bool  gboard_init(const char *so_path, const char *pack_dir);
bool  gboard_append(const char *pinyin);
int   gboard_get_candidates(char **out, int max);
bool  gboard_select(int index);
void  gboard_reset(void);
void  gboard_destroy(void);
