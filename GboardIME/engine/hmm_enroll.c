// Data and setting scheme enrollment logic.
//
// Deobfuscated class name mapping (from Gboard APK via ProGuard):
//
//   Obfuscated  →  Likely Original             Notes
//   ─────────────────────────────────────────────────────────────────
//   aogz        →  DataScheme                  Outer protobuf for nativeEnrollDataScheme;
//                                              field 1 = repeated DataSchemeEntry,
//                                              field 2 = prefix (string),
//                                              field 3 = base_path (string)
//   aogw        →  DataSchemeEntry             Inner entry within DataScheme;
//                                              field 1 = type (varint),
//                                              field 2 = data_id (string),
//                                              field 3 = creator_type (varint, set to 5 = file system),
//                                              field 4 = filename (string)
//   aohv        →  SettingScheme               Protobuf for nativeEnrollSettingScheme;
//                                              field 4 = DictionaryConfig
//   aoht        →  DictionaryConfig            Container inside SettingScheme;
//                                              field 1 = repeated DictionaryEntry
//   aohs        →  DictionaryEntry             Individual dict entry;
//                                              field 1 = type-1, field 2 = name, field 3 = flags-1
//   lcj         →  DataSchemeModifier          Sets DataSchemeEntry.creator_type = 5 (file system loading)
//   nsg         →  HmmEngineBuilder            Orchestrates system dict enrollment via S(type, name, flags)
//   kzw         →  MutableDictEnroller         Enrolls mutable dicts (contacts, user, shortcuts) via f()
//
#include "hmm_internal.h"

// ── Protobuf encoding helpers ───────────────────────────────────────────────

static size_t pb_varint_size(uint64_t v) {
    size_t n = 1; while (v >= 0x80) { n++; v >>= 7; } return n;
}
static size_t pb_write_varint(uint8_t *buf, uint64_t v) {
    size_t n = 0;
    while (v >= 0x80) { buf[n++] = (uint8_t)(v | 0x80); v >>= 7; }
    buf[n++] = (uint8_t)v; return n;
}
static size_t pb_write_tag(uint8_t *buf, int field, int wire) {
    return pb_write_varint(buf, (uint64_t)((field << 3) | wire));
}
static size_t pb_write_string(uint8_t *buf, int field, const char *s) {
    size_t slen = strlen(s);
    size_t n = pb_write_tag(buf, field, 2);
    n += pb_write_varint(buf + n, slen);
    memcpy(buf + n, s, slen); n += slen;
    return n;
}

// ── Data scheme (DataScheme / aogz) parsing ─────────────────────────────────

typedef struct {
    int type;       // DataSchemeEntry.type (field 1)
    char data_id[256];   // DataSchemeEntry.data_id (field 2)
    char filename[256];  // DataSchemeEntry.filename (field 4)
} ParsedEntry;

// Parse DataScheme protobuf: extract repeated DataSchemeEntry (field 1) submessages
static int parse_data_scheme(const uint8_t *data, size_t len, ParsedEntry *entries, int max_entries) {
    size_t pos = 0;
    int count = 0;
    while (pos < len && count < max_entries) {
        // Expect DataScheme field 1 (DataSchemeEntry), wire type 2 (submessage)
        if ((data[pos] >> 3) != 1 || (data[pos] & 7) != 2) break;
        pos++;
        uint64_t sublen = 0; int shift = 0;
        while (pos < len && (data[pos] & 0x80)) {
            sublen |= (uint64_t)(data[pos++] & 0x7f) << shift; shift += 7;
        }
        if (pos < len) sublen |= (uint64_t)(data[pos++] & 0x7f) << shift;

        ParsedEntry *e = &entries[count];
        e->type = 0; e->data_id[0] = 0; e->filename[0] = 0;

        size_t sp = 0;
        while (sp < (size_t)sublen) {
            uint8_t tag = data[pos + sp]; sp++;
            int fnum = tag >> 3, wtype = tag & 7;
            if (wtype == 0) {
                uint64_t v = 0; int vs = 0;
                while (sp < (size_t)sublen && (data[pos + sp] & 0x80)) {
                    v |= (uint64_t)(data[pos + sp] & 0x7f) << vs; vs += 7; sp++;
                }
                if (sp < (size_t)sublen) { v |= (uint64_t)(data[pos + sp] & 0x7f) << vs; sp++; }
                if (fnum == 1) e->type = (int)v;
            } else if (wtype == 2) {
                uint64_t slen = 0; int ss = 0;
                while (sp < (size_t)sublen && (data[pos + sp] & 0x80)) {
                    slen |= (uint64_t)(data[pos + sp] & 0x7f) << ss; ss += 7; sp++;
                }
                if (sp < (size_t)sublen) { slen |= (uint64_t)(data[pos + sp] & 0x7f) << ss; sp++; }
                if (fnum == 2 && slen < sizeof(e->data_id)) {
                    memcpy(e->data_id, data + pos + sp, (size_t)slen);
                    e->data_id[slen] = 0;
                } else if (fnum == 4 && slen < sizeof(e->filename)) {
                    memcpy(e->filename, data + pos + sp, (size_t)slen);
                    e->filename[slen] = 0;
                }
                sp += (size_t)slen;
            } else break;
        }
        pos += (size_t)sublen;
        if (e->data_id[0]) count++;
    }
    return count;
}

// ── Data scheme modification (replicates DataSchemeModifier / lcj) ───────────
// Modifies DataScheme protobuf: sets base_path and changes each
// DataSchemeEntry.creator_type (field 3) to 5 (= load from file system).

static uint8_t *modify_data_scheme(const uint8_t *orig, size_t orig_len,
                                    const char *abs_path, size_t *out_len) {
    size_t path_len = strlen(abs_path);
    size_t outer_add = 2;
    outer_add += 1 + pb_varint_size(path_len) + path_len;

    size_t out_cap = orig_len + outer_add + 64;
    uint8_t *out = malloc(out_cap);
    size_t opos = 0;

    size_t pos = 0;
    int modified = 0, skipped = 0;
    while (pos < orig_len) {
        if (pos >= orig_len || (orig[pos] >> 3) != 1 || (orig[pos] & 7) != 2) break;
        size_t entry_start = pos;
        pos++;

        uint64_t sublen = 0; int shift = 0;
        while (pos < orig_len && (orig[pos] & 0x80)) {
            sublen |= (uint64_t)(orig[pos++] & 0x7f) << shift; shift += 7;
        }
        if (pos < orig_len) sublen |= (uint64_t)(orig[pos++] & 0x7f) << shift;
        size_t sub_data_start = pos;

        // Extract filename to check if file exists
        char filename[512] = {0};
        size_t sp = 0;
        while (sp < (size_t)sublen) {
            uint8_t tag_byte = orig[sub_data_start + sp];
            int field_num = tag_byte >> 3;
            int wire_type = tag_byte & 7;
            sp++;
            if (wire_type == 0) {
                while (sp < (size_t)sublen && (orig[sub_data_start + sp] & 0x80)) sp++;
                if (sp < (size_t)sublen) sp++;
            } else if (wire_type == 2) {
                uint64_t flen = 0; int fshift = 0;
                while (sp < (size_t)sublen && (orig[sub_data_start + sp] & 0x80)) {
                    flen |= (uint64_t)(orig[sub_data_start + sp] & 0x7f) << fshift;
                    fshift += 7; sp++;
                }
                if (sp < (size_t)sublen) {
                    flen |= (uint64_t)(orig[sub_data_start + sp] & 0x7f) << fshift;
                    sp++;
                }
                if (field_num == 4 && flen < sizeof(filename)) {
                    memcpy(filename, orig + sub_data_start + sp, (size_t)flen);
                    filename[flen] = '\0';
                }
                sp += (size_t)flen;
            } else break;
        }

        // Skip entry if file missing
        if (filename[0]) {
            char fpath[4608];
            snprintf(fpath, sizeof(fpath), "%s/%s", abs_path, filename);
            if (access(fpath, R_OK) != 0) {
                LOGERR("  skip entry '%s' (file not found)", filename);
                pos = sub_data_start + (size_t)sublen;
                skipped++;
                continue;
            }
        }

        // Copy tag + length varint
        out[opos++] = orig[entry_start];
        size_t lv_start = entry_start + 1;
        size_t lv_len = sub_data_start - lv_start;
        memcpy(out + opos, orig + lv_start, lv_len);
        opos += lv_len;

        // Copy submessage and patch DataSchemeEntry.creator_type (field 3) to 5
        size_t sub_start = opos;
        memcpy(out + opos, orig + sub_data_start, (size_t)sublen);

        sp = 0;
        while (sp < (size_t)sublen) {
            uint8_t tag_byte = out[sub_start + sp];
            int field_num = tag_byte >> 3;
            int wire_type = tag_byte & 7;
            sp++;
            if (wire_type == 0) {
                size_t varint_start = sp;
                while (sp < (size_t)sublen && (out[sub_start + sp] & 0x80)) sp++;
                if (sp < (size_t)sublen) sp++;
                if (field_num == 3) {
                    out[sub_start + varint_start] = 0x05;
                    modified++;
                }
            } else if (wire_type == 2) {
                uint64_t flen = 0; int fshift = 0;
                while (sp < (size_t)sublen && (out[sub_start + sp] & 0x80)) {
                    flen |= (uint64_t)(out[sub_start + sp] & 0x7f) << fshift;
                    fshift += 7; sp++;
                }
                if (sp < (size_t)sublen) {
                    flen |= (uint64_t)(out[sub_start + sp] & 0x7f) << fshift;
                    sp++;
                }
                sp += (size_t)flen;
            } else break;
        }

        opos += (size_t)sublen;
        pos = sub_data_start + (size_t)sublen;
    }

    // Append DataScheme outer fields: prefix (field 2) = "", base_path (field 3) = abs_path
    opos += pb_write_string(out + opos, 2, "");
    opos += pb_write_string(out + opos, 3, abs_path);

    LOGERR("Modified data_scheme: %zu → %zu bytes, %d entries creator_type→5, %d skipped (path=%s)",
           orig_len, opos, modified, skipped, abs_path);
    *out_len = opos;
    return out;
}

// ── Data pack enrollment ────────────────────────────────────────────────────

static bool enroll_pack(const char *pack_dir) {
    int enrolled = 0;
    char abs_pack[4096];
    if (realpath(pack_dir, abs_pack) == NULL)
        snprintf(abs_pack, sizeof(abs_pack), "%s", pack_dir);

    char scheme_path[4096];
    snprintf(scheme_path, sizeof(scheme_path), "%s/data_scheme", pack_dir);
    FILE *fp = fopen(scheme_path, "rb");
    if (!fp) { LOGERR("data_scheme not found"); return false; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *raw = malloc((size_t)sz);
    fread(raw, 1, (size_t)sz, fp);
    fclose(fp);

    ParsedEntry all_entries[128];
    int nentries = parse_data_scheme(raw, (size_t)sz, all_entries, 128);
    LOGERR("Parsed data_scheme: %d entries from %ld bytes", nentries, sz);

    // Method A: nativeEnrollDataScheme with modified protobuf
    if (g_enrollScheme) {
        size_t mod_len = 0;
        uint8_t *mod = modify_data_scheme(raw, (size_t)sz, abs_pack, &mod_len);
        jbyteArray schema_bytes = jni_NewByteArray(g_env, (jsize)mod_len);
        jni_SetByteArrayRegion(g_env, schema_bytes, 0, (jsize)mod_len, (jbyte*)mod);
        free(mod);
        CRASH_PROTECT_BEGIN()
        jboolean ok = g_enrollScheme(g_env, NULL, g_dm, schema_bytes);
        LOGERR("nativeEnrollDataScheme → %d", ok);
        if (ok) enrolled = nentries;
        CRASH_PROTECT_END("nativeEnrollDataScheme")
    }

    // Method B: nativeEnrollDataFd for ALL entries
    if (g_enrollFd) {
        int fd_enrolled = 0, fd_skipped = 0;
        for (int i = 0; i < nentries; i++) {
            if (!all_entries[i].filename[0]) continue;
            char fpath[4608];
            snprintf(fpath, sizeof(fpath), "%s/%s", abs_pack, all_entries[i].filename);
            struct stat st;
            if (stat(fpath, &st) != 0) { fd_skipped++; continue; }
            int fd = open(fpath, O_RDONLY);
            if (fd < 0) { fd_skipped++; continue; }
            jstring jid = jni_NewStringUTF(g_env, all_entries[i].data_id);
            jobject jfd = jni_create_file_descriptor(g_env, fd);
            CRASH_PROTECT_BEGIN()
            jboolean ok = g_enrollFd(g_env, NULL, g_dm, jid,
                                      (jint)all_entries[i].type, jfd,
                                      0, (jint)st.st_size);
            if (ok) fd_enrolled++; else fd_skipped++;
            CRASH_PROTECT_END("nativeEnrollDataFd")
        }
        LOGERR("enrollDataFd: %d/%d OK, %d skipped", fd_enrolled, nentries, fd_skipped);
        if (fd_enrolled > enrolled) enrolled = fd_enrolled;
    }

    free(raw);
    LOGERR("enroll_pack total: %d enrolled", enrolled);
    return enrolled > 0;
}

// ── Public enrollment entry point ───────────────────────────────────────────

bool hmm_enroll_all(const char *pack_dir) {
    if (!pack_dir) return false;

    // 1. Enroll data pack
    bool ok = enroll_pack(pack_dir);
    LOG("enroll_pack → %s", ok ? "OK" : "FAILED (all methods)");

    // 2. Enroll empty mutable dictionaries (replicates MutableDictEnroller / kzw)
    if (g_enrollEmptyMutableDict && g_dm) {
        const struct { const char *name; int type; int capacity; } mut_dicts[] = {
            {"new_words_dictionary_accessor", 24, 1},
            {"contacts_dictionary_accessor", 24, 2},
            {"user_dictionary_accessor", 23, 3},
            {"shortcuts_dictionary_accessor", 24, 4},
            {NULL, 0, 0}
        };
        for (int i = 0; mut_dicts[i].name; i++) {
            jstring jname = jni_NewStringUTF(g_env, mut_dicts[i].name);
            CRASH_PROTECT_BEGIN()
            jboolean r = g_enrollEmptyMutableDict(g_env, NULL, g_dm, jname,
                                                    (jint)mut_dicts[i].type, (jint)mut_dicts[i].capacity);
            LOGERR("enrollEmptyMutableDict('%s', type=%d, cap=%d) → %d",
                   mut_dicts[i].name, mut_dicts[i].type, mut_dicts[i].capacity, r);
            CRASH_PROTECT_END("nativeEnrollEmptyMutableDict")
        }
    }

    // 3. Enroll setting schemes (SettingScheme / aohv)
    bool setting_ok = false;
    jlong handles[] = { g_sm, 0 };

    if (g_enrollSettingScheme) {
        const char *scheme_files[] = { "pinyin_qwerty_setting_scheme", NULL };
        const char *type_strs[] = { "zh-t-i0-pinyin-x-f0-delight", "zh-t-i0-pinyin", NULL };
        for (int si = 0; scheme_files[si]; si++) {
            char spath[4096];
            snprintf(spath, sizeof(spath), "%s/%s", pack_dir, scheme_files[si]);
            FILE *sfp = fopen(spath, "rb");
            if (!sfp) continue;
            fseek(sfp, 0, SEEK_END);
            long ssz = ftell(sfp);
            fseek(sfp, 0, SEEK_SET);
            uint8_t *sbuf = malloc((size_t)ssz);
            fread(sbuf, 1, (size_t)ssz, sfp);
            fclose(sfp);
            jbyteArray ba = jni_NewByteArray(g_env, (jsize)ssz);
            jni_SetByteArrayRegion(g_env, ba, 0, (jsize)ssz, (jbyte*)sbuf);
            free(sbuf);
            for (int hi = 0; handles[hi]; hi++) {
                for (int ti = 0; type_strs[ti]; ti++) {
                    jstring st = jni_NewStringUTF(g_env, type_strs[ti]);
                    jstring sl = jni_NewStringUTF(g_env, "");
                    CRASH_PROTECT_BEGIN()
                    jboolean r = g_enrollSettingScheme(g_env, NULL, handles[hi], st, sl, ba);
                    LOGERR("enrollSettingScheme(handle=%lld,'%s','%s') → %d",
                        (long long)handles[hi], type_strs[ti], scheme_files[si], r);
                    if (r) setting_ok = true;
                    CRASH_PROTECT_END("nativeEnrollSettingScheme")
                }
            }
        }
    }

    if (g_loadBuiltInSettingScheme && !setting_ok) {
        const char *type_strs[] = { "zh-t-i0-pinyin", "zh-t-i0-pinyin-x-f0-delight", NULL };
        const char *loc_strs[]  = { "zh_CN", pack_dir, NULL };
        for (int hi = 0; !setting_ok && handles[hi]; hi++) {
            for (int ti = 0; !setting_ok && type_strs[ti]; ti++) {
                for (int li = 0; !setting_ok && loc_strs[li]; li++) {
                    jstring st = jni_NewStringUTF(g_env, type_strs[ti]);
                    jstring sl = jni_NewStringUTF(g_env, loc_strs[li]);
                    jbyteArray result = NULL;
                    CRASH_PROTECT_BEGIN()
                    result = g_loadBuiltInSettingScheme(g_env, NULL, handles[hi], st, sl);
                    LOGERR("loadBuiltInSettingScheme(handle=%lld,'%s','%s') → %p",
                        (long long)handles[hi], type_strs[ti], loc_strs[li], result);
                    CRASH_PROTECT_END("nativeLoadBuiltInSettingScheme")
                    if (result) setting_ok = true;
                }
            }
        }
    }
    LOGERR("setting scheme enrollment: %s", setting_ok ? "OK" : "FAILED");

    // 4. Enroll mutable dictionary accessor setting schemes (HmmEngineBuilder / nsg → y() method)
    if (g_enrollSettingScheme && g_sm) {
        static const struct { const char *accessor; const char *scheme_file; } mut_schemes[] = {
            {"zh_t_i0_pinyin_new_words_dictionary_accessor",     "pinyin_mutable_dictionary_accessor_setting_scheme"},
            {"zh_t_i0_pinyin_contacts_dictionary_accessor",      "pinyin_mutable_dictionary_accessor_setting_scheme"},
            {"zh_t_i0_pinyin_user_dictionary_accessor",          "pinyin_mutable_dictionary_accessor_setting_scheme"},
            {"zh_t_i0_pinyin_shortcuts_dictionary_accessor",     "shortcuts_mutable_dictionary_accessor_setting_scheme"},
            {"zh_t_i0_pinyin_user_dictionary_accessor",          "pinyin_mutable_dictionary_accessor_setting_scheme_secondary"},
            {NULL, NULL}
        };
        for (int mi = 0; mut_schemes[mi].accessor; mi++) {
            char mpath[4096];
            snprintf(mpath, sizeof(mpath), "%s/%s", pack_dir, mut_schemes[mi].scheme_file);
            FILE *mfp = fopen(mpath, "rb");
            if (!mfp) { LOGERR("  mut scheme '%s' not found", mut_schemes[mi].scheme_file); continue; }
            fseek(mfp, 0, SEEK_END);
            long msz = ftell(mfp);
            fseek(mfp, 0, SEEK_SET);
            uint8_t *mbuf = malloc((size_t)msz);
            fread(mbuf, 1, (size_t)msz, mfp);
            fclose(mfp);
            jbyteArray mba = jni_NewByteArray(g_env, (jsize)msz);
            jni_SetByteArrayRegion(g_env, mba, 0, (jsize)msz, (jbyte*)mbuf);
            free(mbuf);
            jstring macc = jni_NewStringUTF(g_env, mut_schemes[mi].accessor);
            jstring mloc = jni_NewStringUTF(g_env, "");
            CRASH_PROTECT_BEGIN()
            jboolean r = g_enrollSettingScheme(g_env, NULL, g_sm, macc, mloc, mba);
            LOGERR("enrollMutSettingScheme('%s','%s') → %d",
                mut_schemes[mi].accessor, mut_schemes[mi].scheme_file, r);
            CRASH_PROTECT_END("enrollMutSettingScheme")
        }
    }

    // 5. Refresh data
    if (g_refreshData) {
        CRASH_PROTECT_BEGIN()
        g_refreshData(g_env, NULL, g_dm);
        LOGERR("refreshData OK");
        CRASH_PROTECT_END("nativeRefreshData")
    }

    return ok;
}
