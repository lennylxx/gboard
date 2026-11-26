// The u-mode (拆字) tables as read-only data. scripts/gen_chaizi_tables.py
// compiles the TSV files into chaizi_tables.h before each build.

#include "GboardBridge.h"
#include "chaizi_tables.h"

static const ChaiziTables tables = {
    .reading_pool = chaizi_reading_pool,
    .reading_offsets = chaizi_reading_offsets,
    .component_reading_ids = chaizi_component_reading_ids,
    .component_reading_starts = chaizi_component_reading_starts,
    .split_components = chaizi_split_components,
    .split_starts = chaizi_split_starts,
    .character_pool = chaizi_character_pool,
    .character_starts = chaizi_character_starts,
    .entry_split_starts = chaizi_entry_split_starts,
    .entry_first_letters = chaizi_entry_first_letters,
    .entry_count = CHAIZI_ENTRY_COUNT,
};

const ChaiziTables *gboard_chaizi_tables(void) {
    return &tables;
}
