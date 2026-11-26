#!/usr/bin/env python3
"""Compiles the u-mode (拆字) TSV tables into chaizi_tables.h.

The header holds flat, read-only arrays that ChaiziDictionary.swift
matches against directly, so nothing is parsed at runtime. Building
also validates the tables: every component needs a reading, and each
character may appear once.

Usage: scripts/gen_chaizi_tables.py <output-dir> [<table-dir>]
"""

import os
import sys

# Deeper splits are derived from components the table also decomposes,
# e.g. 骥 马冀 with 冀 北田共 also gives 马北田共.
MAX_EXPANSION_DEPTH = 2
MAX_PARTS = 6
MAX_FORMS_PER_COMPONENT = 6
MAX_SPLITS = 12


def fail(message):
    sys.exit("gen_chaizi_tables: " + message)


def records(path):
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                continue
            key, tab, value = line.partition("\t")
            if not tab or not value:
                fail("%s:%d: expected key<TAB>value" % (path, number))
            yield number, key, value


def encoding_order(character):
    """GB2312 order (level 1 by pinyin, level 2 by radical), then others
    by code point."""
    try:
        data = character.encode("gb2312")
        if len(data) == 2:
            return data[0] << 8 | data[1]
    except UnicodeEncodeError:
        pass
    return 0x10000 + ord(character)


def expand(authored, table):
    def product(choices):
        result = [[]]
        for options in choices:
            result = [prefix + option for prefix in result for option in options]
        return result

    def forms(component, depth):
        result = [[component]]
        if depth == 0 or component not in table:
            return result
        for split in table[component]:
            for form in product([forms(c, depth - 1) for c in split]):
                if len(form) <= MAX_PARTS and form not in result:
                    result.append(form)
                    if len(result) >= MAX_FORMS_PER_COMPONENT:
                        return result
        return result

    splits = [list(split) for split in authored]
    for split in authored:
        for form in product([forms(c, MAX_EXPANSION_DEPTH) for c in split]):
            if len(form) <= MAX_PARTS and form not in splits:
                if len(splits) >= MAX_SPLITS:
                    return splits
                splits.append(form)
    return splits


def c_string(text, width):
    """A string literal split into lines of `width` characters. Pools hold
    only letters and CJK characters, so nothing needs escaping."""
    if any(c in '"\\\n' for c in text):
        fail("pool text needs escaping")
    chunks = [text[i:i + width] for i in range(0, len(text), width)]
    return "\n".join('"%s"' % chunk for chunk in chunks) or '""'


def c_array(kind, name, values):
    body = []
    for i in range(0, len(values), 16):
        body.append("    " + ", ".join(str(v) for v in values[i:i + 16]) + ",")
    return "static const %s %s[%d] = {\n%s\n};\n" % (
        kind, name, max(len(values), 1), "\n".join(body) or "    0,")


def main():
    if len(sys.argv) not in (2, 3):
        fail("usage: gen_chaizi_tables.py <output-dir> [<table-dir>]")
    out_dir = sys.argv[1]
    table_dir = sys.argv[2] if len(sys.argv) == 3 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "GboardIME", "ime")
    components_path = os.path.join(table_dir, "chaizi_components.tsv")
    characters_path = os.path.join(table_dir, "chaizi_characters.tsv")

    readings_of = {}
    for number, key, value in records(components_path):
        readings = value.split()
        if len(key) != 1 or not readings or not all(
                r.isascii() and r.isalpha() and r.islower() for r in readings):
            fail("%s:%d: bad component line" % (components_path, number))
        if key in readings_of:
            fail("%s:%d: duplicate component %s" % (components_path, number, key))
        readings_of[key] = readings

    table = {}
    for number, key, value in records(characters_path):
        splits = [list(split) for split in value.split("|")]
        if len(key) != 1 or any(len(s) < 2 or key in s for s in splits):
            fail("%s:%d: bad character line" % (characters_path, number))
        if key in table:
            fail("%s:%d: duplicate character %s" % (characters_path, number, key))
        unreadable = sorted({c for s in splits for c in s if c not in readings_of})
        if unreadable:
            fail("%s:%d: no reading for %s" % (characters_path, number, "".join(unreadable)))
        table[key] = splits

    reading_ids, reading_pool, reading_offsets = {}, [], [0]
    component_ids, component_reading_ids, component_reading_starts = {}, [], [0]

    def component_id(component):
        if component not in component_ids:
            component_ids[component] = len(component_ids)
            for reading in readings_of[component]:
                if reading not in reading_ids:
                    reading_ids[reading] = len(reading_ids)
                    reading_pool.append(reading)
                    reading_offsets.append(reading_offsets[-1] + len(reading))
                component_reading_ids.append(reading_ids[reading])
            component_reading_starts.append(len(component_reading_ids))
        return component_ids[component]

    character_pool, character_starts = [], [0]
    split_components, split_starts, entry_split_starts, first_letters = [], [0], [0], []
    for character in sorted(table, key=encoding_order):
        mask = 0
        for split in expand(table[character], table):
            for component in split:
                split_components.append(component_id(component))
            split_starts.append(len(split_components))
            for reading in readings_of[split[0]]:
                mask |= 1 << (ord(reading[0]) - ord("a"))
        entry_split_starts.append(len(split_starts) - 1)
        first_letters.append(mask)
        character_pool.append(character)
        character_starts.append(character_starts[-1] + len(character.encode("utf-8")))

    if len(component_ids) > 0xFFFF or len(reading_ids) > 0xFFFF:
        fail("too many components or readings for 16-bit ids")

    header = "\n".join([
        "// Generated by scripts/gen_chaizi_tables.py; edit the TSV files.",
        "// %d characters, %d splits, %d components, %d readings." % (
            len(first_letters), len(split_starts) - 1, len(component_ids), len(reading_ids)),
        "",
        "#include <stdint.h>",
        "",
        "static const char chaizi_reading_pool[] =\n%s;\n" % c_string("".join(reading_pool), 72),
        c_array("uint32_t", "chaizi_reading_offsets", reading_offsets),
        c_array("uint16_t", "chaizi_component_reading_ids", component_reading_ids),
        c_array("uint32_t", "chaizi_component_reading_starts", component_reading_starts),
        c_array("uint16_t", "chaizi_split_components", split_components),
        c_array("uint32_t", "chaizi_split_starts", split_starts),
        "static const char chaizi_character_pool[] =\n%s;\n" % c_string("".join(character_pool), 32),
        c_array("uint32_t", "chaizi_character_starts", character_starts),
        c_array("uint32_t", "chaizi_entry_split_starts", entry_split_starts),
        c_array("uint32_t", "chaizi_entry_first_letters", first_letters),
        "#define CHAIZI_ENTRY_COUNT %d" % len(first_letters),
        "",
    ])

    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "chaizi_tables.h")
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == header:
                return  # Unchanged: keep the timestamp so nothing rebuilds.
    except FileNotFoundError:
        pass
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="utf-8") as f:
        f.write(header)
    os.replace(temporary, path)


if __name__ == "__main__":
    main()
