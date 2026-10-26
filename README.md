# Gboard for macOS

[English](README.md) | [简体中文](README.zh-CN.md)

A macOS input method that runs Gboard's native HMM Pinyin engine via a custom ARM64 ELF loader. Type Mandarin Chinese on macOS using the same engine that powers Gboard on Android.

> **This project does not include any Google binaries or data.** You must supply your own legally obtained Gboard APK. See [NOTICE](NOTICE) for details.

## Preview

![preview](https://github.com/user-attachments/assets/5840ead1-65f6-44c9-9c3a-6347d9271a02)

## Features

- **Native Pinyin engine** — Runs Gboard's native HMM Pinyin engine directly
  on Apple Silicon.
- **Offline candidate generation and ranking** — Generates candidates and
  performs neural language-model reranking entirely on-device.
- **Context-aware ranking** — Uses committed text before the cursor to improve
  candidate ordering. The native TARGET_TOKEN path keeps the last 5 UTF-16
  units for Chinese or 20 for Latin text and stops at punctuation,
  whitespace, or a language boundary. For example, `bushu` normally prefers
  `部署`, while the context `我对这里很` promotes `不熟`.
- **Partial candidate selection** — Consumes only the Pinyin matched by the
  selected candidate and preserves the remainder. For example, after typing
  `nihao` and selecting `你`, `hao` remains available for continued
  composition.
- **Pinyin segmentation display** — Shows the engine's segment/token split,
  such as `fang'an`, and explicit apostrophe separators such as `xi'an`.
- **Pinyin typo correction** — Uses Gboard's built-in corrector for swapped,
  missing, or extra letters. For example, `nihoa` yields `你好` and
  `zhonguo` yields `中国`. The Pinyin strip marks the fix: extra letters are
  struck out and inserted letters are shown in orange. Settings offers
  independent correction alignment: insert/delete marks (default, LCS) or
  swap-aware marks (restricted Damerau-Levenshtein, DL/OSA). Swap-aware marks
  highlight adjacent swapped letters together in corrected order:
  `nihoa` shows `ni'hao`, with `ao` in orange.
  Alignment changes take effect immediately and are saved; they work with
  all three marking styles and do not affect native candidate generation.
- **Fuzzy Pinyin** — Independently enable 12 pronunciation pairs in Settings:
  z/zh, c/ch, s/sh, an/ang, en/eng, in/ing, l/n, f/h, r/l, k/g, ian/iang,
  and uan/uang. All are off by default. Changes are saved and take effect
  immediately without restarting, preserving active Pinyin, explicit
  separators, and learned data. For example, enabling l/n lets `hulan`
  produce `湖南`.
- **Chinese-English mixed input** — Offers English words inside Pinyin
  input using Gboard's English token and system dictionaries. For example,
  `woyaogithub` yields `我要GitHub` and `yongpythonxie` yields `用Python写`.
- **Double Pinyin (Shuangpin)** — Choose Flypy, Ziranma, Microsoft, Smart ABC,
  Ziguang, or Pinyin Jiajia in Settings. Full Pinyin remains the default.
  Uses Gboard's native token dictionaries, with raw scheme keys shown in the
  Pinyin strip (for example, Flypy `nihc` → `你好`). Changes are saved and
  take effect after the current composition ends, preserving partial
  selections and learning. Microsoft and Ziguang accept `;` while composing.
- **Chinese next-word prediction** — After a candidate is committed, shows
  the engine's next-word predictions for the last committed Chinese text.
  Press `Space` for the first one, `1-9` or click to insert one and continue
  the chain, and `-`/`=` to page; any other key dismisses them and is
  handled normally. For example, committing `生日快` predicts `乐`.
- **Automatic user-dictionary learning** — Stores selected Chinese and mixed
  phrases in `user_dict_3_3` and pure English words in
  `user_dict_3_3_english` under `~/Library/Application Support/GboardIME/`,
  persisting both every four hours and on app teardown.
- **Remove and undo learned words** — `Shift+Backspace` removes the
  highlighted candidate when it comes only from the user dictionary (as
  Gboard's long-press "remove suggestion"); while one is highlighted, a
  "⇧⌫ 删除学习词" hint appears beside the settings button (only when
  pinyin is shown in the candidate window). Pressing Backspace right
  after committing a learned phrase undoes that learning once and deletes
  the text as usual.
- **Learned-word manager** — Settings → 个性化 → 管理学习词… lists the
  learned Chinese and English words with their readings and counts.
  Search, sort, delete multiple entries, edit counts, or export and
  import the list as a JSON Lines file (import adds missing words and
  only raises counts). Removing a word that also exists in the system
  dictionary does not hide it from candidates. Words learned from
  abbreviated pinyin (such as `cs`) are not listed, because Gboard's
  export omits them.
- **Shortcuts** — Settings → 快捷短语 expands a full abbreviation into a
  custom phrase, offered as the first candidate (for example, `dz` →
  北京市朝阳区建国路 88 号). Off by default. Manage the list in 管理快捷短语…:
  add, edit, delete, search, and import or export it as JSON Lines.
  Shortcuts use 1–32 letters; phrases may be Chinese, mixed, or plain
  ASCII such as an email address. Selecting a shortcut is not learned.
- **Complete keyboard workflow** — Supports candidate paging, keyboard
  selection, and Chinese/English mode switching.
- **Simplified / Traditional Chinese toggle** — Convert candidates and
  next-word predictions to Traditional Chinese in real time while keeping
  user-dictionary learning and context scoring in Simplified Chinese.
- **?123 symbols panel** — Open a 6-page symbol picker (`Ctrl+1` or
  `Option+?` in Chinese mode, or from the input menu) and select symbols
  with `1-9`.
- **Native macOS interface** — Uses InputMethodKit and a non-activating
  `NSPanel`. On macOS 26 and later, the candidate window uses Liquid Glass
  (`NSGlassEffectView`) following Apple's guidelines: untinted regular
  glass, system label colors, and an accent-colored selection. Transparency
  follows the system Appearance and Reduce Transparency settings. Earlier
  macOS versions fall back to a frosted-glass panel with Gboard Google Blue
  light/dark themes and an adjustable opacity.

## How it works

Three layers make this possible:

1. **ELF loader** — Loads the ARM64 `libintegrated_shared_object.so` directly into memory on macOS. Handles relocations, symbol resolution, and TPIDR (thread-local storage) patching.
2. **JNI bridge** — Fakes a minimal Android JNI environment so the native engine can initialize, enroll dictionary packs, and process input.
3. **macOS IME** — A Swift/InputMethodKit app that captures keystrokes, drives the engine, and shows candidates in a SwiftUI panel.

## Prerequisites

- macOS 13.0+ on Apple Silicon
- Xcode or the Xcode Command Line Tools (`build.sh` falls back to `swiftc`
  when `xcodebuild` is unavailable)
- `brew install jq`

## Quick start

```bash
# 1. Extract APK and download dictionary pack
./setup.sh --xapk /path/to/com.google.android.inputmethod.latin.xapk

# 2. Build and install
./build.sh install

# 3. Log out and log back in, then:
#    System Settings → Keyboard → Input Sources → Edit → + → Chinese, Simplified → GboardIME
```

### setup.sh options

```
--xapk PATH     Path to Gboard XAPK file (required)
--dict PATH     Use a local dict zip instead of downloading
--locale CODE   zh_CN (default), zh_TW, zh_HK, ko
```

Optional: `./decompile.sh` decompiles the Java sources into `gboard_apk_source/jadx/` for reverse-engineering (requires `brew install jadx`; not needed to build).

### build.sh commands

```
build       Build the app (default)
install     Build, install to ~/Library/Input Methods, and sign
uninstall   Remove from ~/Library/Input Methods
clean       Remove build artifacts
```

## Usage

Switch to GboardIME from the menu bar input source picker, then type pinyin.

Choose **设置…** (Settings) in the input method menu to open the settings
window; changes are saved automatically.

| Key | Action |
|-----|--------|
| a-z | Append to pinyin composition |
| ; | Append a Shuangpin key in Microsoft/Ziguang while composing; otherwise Chinese punctuation |
| 1-9 | Select nth candidate or next-word prediction |
| - | Show previous candidate or prediction page |
| = | Show next candidate or prediction page |
| Space | Commit highlighted candidate or first prediction |
| ← → | Move candidate selection, crossing page boundaries |
| Backspace | Delete last pinyin character; right after a learned commit (without modifiers), also undo that learning |
| Shift+Backspace | Remove the highlighted learned candidate from the user dictionary |
| Escape | Cancel composition or dismiss predictions |
| Return | Commit raw pinyin |
| Shift | Switch between Chinese and English; commit active composition as raw pinyin |
| Cmd+Shift+F | Toggle Simplified / Traditional Chinese (in Chinese mode) |
| Ctrl+1 / Option+? | Toggle ?123 punctuation & symbols panel (in Chinese mode) |
| Caps Lock | Switch between GboardIME and ABC when enabled in macOS Text Input settings; commit active composition as raw pinyin |

## Test

```bash
./test.sh
```

Runs both C engine tests and Swift IME tests.

## Legal

This is an independent research project. Google, Gboard, and Android are trademarks of Google LLC. Not affiliated with or endorsed by Google. See [NOTICE](NOTICE).

Contributions must follow the copyright and legal principles in
[CONTRIBUTING](CONTRIBUTING.md).
