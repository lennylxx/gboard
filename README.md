# Gboard for macOS

[English](README.md) | [简体中文](README.zh-CN.md)

A macOS input method that runs Gboard's native HMM Pinyin engine through a
custom ARM64 ELF loader, so you can type Chinese on macOS with the same engine
that powers Gboard on Android.

> [!IMPORTANT]
> **This project does not include any Google binaries or data.** You must supply
> your own legally obtained Gboard APK. See [NOTICE](NOTICE) for details.

## Preview

![preview](https://github.com/user-attachments/assets/5840ead1-65f6-44c9-9c3a-6347d9271a02)

## Features

### Engine

- **Native Pinyin engine**: runs Gboard's native HMM Pinyin engine directly on
  Apple Silicon.
- **Offline candidate generation and ranking**: candidate generation and neural
  language-model reranking run on-device.
- **Context-aware ranking**: committed text before the cursor improves candidate
  order. For example, `bushu` normally prefers `部署`, but after `我对这里很` it
  promotes `不熟`.

### Typing

- **Partial selection**: selecting a candidate consumes only the Pinyin it
  matches. For example, typing `nihao` and selecting `你` leaves `hao` for
  further composition. Backspace undoes the selection.
- **Segmentation display**: the Pinyin strip shows the engine's actual
  segmentation (such as `fang'an`) and explicit apostrophe separators (such as
  `xi'an`).
- **Typo correction**: Gboard's built-in corrector handles swapped, missing, or
  extra letters, and the Pinyin strip marks each fix. For example, `nihoa`
  yields `你好` and `zhonguo` yields `中国`.
- **Shuangpin key correction**: separate from typo correction, with its own
  switch. In Shuangpin it fixes a mistyped same-row neighbour or swapped keys
  within a syllable, so Flypy `uhjd` still yields `上海`.
- **Fuzzy Pinyin**: 12 optional pronunciation pairs. For example, with l/n
  enabled, `hulan` produces `湖南`.
- **Chinese-English mixed input**: English words are offered inside Pinyin
  input. For example, `woyaogithub` yields `我要GitHub` and `yongpythonxie`
  yields `用Python写`.
- **Shuangpin (Double Pinyin)**: six schemes; full Pinyin remains the default.
  For example, Flypy `nihc` yields `你好`.
- **u-mode character composition**: in full Pinyin, type `u` followed by the
  Pinyin of each component to enter a character you cannot pronounce. For
  example, `uniuniuniu` yields `犇` and `ushuiguyue` yields `湖`. Composed
  characters come first; the engine's own candidates, such as English words,
  follow.
- **Next-word prediction**: after a commit, the engine predicts the next Chinese
  word. For example, committing `生日快` predicts `乐`.
- **Simplified / Traditional toggle**: candidates and predictions are converted
  to Traditional Chinese in real time.
- **`?123` symbols panel**: a 6-page picker for Chinese and full-width symbols.

### Personalization

- **Automatic learning**: selected Chinese, mixed, and English words are learned
  into a local user dictionary.
- **Remove and undo learned words**: remove a learned candidate with
  `Shift+Backspace`, or undo the learning of a phrase just committed with
  Backspace.
- **Learned-word manager**: browse, search, edit, and import or export learned
  words.
- **Shortcuts**: expand a custom abbreviation into a phrase. For example, `dz` →
  北京市朝阳区建国路 88 号.

### Interface

- **Native macOS UI**: built on InputMethodKit with a non-activating `NSPanel`.
  On macOS 26 and later the candidate window uses Liquid Glass; earlier versions
  fall back to a frosted-glass panel.

## Requirements

- macOS 13.0 or later on Apple Silicon
- Xcode or the Xcode Command Line Tools (`build.sh` falls back to `swiftc` when
  `xcodebuild` is unavailable)
- `jq` (`brew install jq`)

## Installation

```bash
# 1. Extract the APK and download the dictionary pack
#    (a .apk file also works)
./setup.sh --xapk /path/to/com.google.android.inputmethod.latin.xapk

# 2. Build and install
./build.sh install
```

> [!IMPORTANT]
> Log out and log back in before adding the input source.

Then add the input source:

**System Settings → Keyboard → Input Sources → Edit → + → Chinese, Simplified →
GboardIME**

See [Development](#development) for all script options.

## Usage

Switch to GboardIME from the input source menu in the menu bar, then type
Pinyin.

### Keyboard shortcuts

| Key | Action |
|-----|--------|
| `a-z` | Append to the Pinyin composition |
| `;` | Append a Shuangpin key in Microsoft/Ziguang while composing; otherwise Chinese punctuation |
| `1-9` | Select the nth candidate or next-word prediction |
| `-` | Show the previous candidate or prediction page |
| `=` | Show the next candidate or prediction page |
| `Space` | Commit the highlighted candidate or the first prediction |
| `←` / `→` | Move the candidate selection, crossing page boundaries |
| `Backspace` | Delete the last Pinyin letter, or undo the last partial selection once nothing typed after it remains; right after a learned commit (without modifiers), also undo that learning |
| `Shift+Backspace` | While composing, remove the highlighted learned candidate from the user dictionary |
| `'` | Insert a Pinyin separator while composing (such as `xi'an`); otherwise Chinese quotation marks |
| Other punctuation | Commit the highlighted candidates, then insert the Chinese form (such as `,` → `，` and `\` → `、`) |
| `Escape` | Cancel the composition or dismiss predictions |
| `Return` | Commit the raw Pinyin (after any partially selected text) |
| `Shift` | Switch between Chinese and English (when 单击 Shift 切换中英文 is on); commit an active composition as raw Pinyin |
| `Cmd+Shift+F` | Toggle Simplified / Traditional Chinese (Chinese mode) |
| `Ctrl+1` / `Option+?` | Toggle the `?123` punctuation and symbols panel (Chinese mode) |
| `Caps Lock` | Switch between GboardIME and ABC when enabled in macOS Text Input settings; commit an active composition as raw Pinyin. Otherwise, with Caps Lock on, letters are typed as uppercase English |

Notes:

- **Next-word prediction**: clicking a prediction also inserts it. After a
  prediction is inserted, the chain continues. Predictions take only `1-9`, `-`,
  `=`, `Space`, and `Escape`; any other key dismisses them and is then handled
  normally.
- **Symbols panel**: also available as 标点与符号 (?123) in the input menu.
  Select symbols with `1-9` or `Space`, move with `←` / `→`, and page with `-` /
  `=`. `Escape`, `Return`, or `Backspace` closes the panel; any other key closes
  it and is then handled normally.
- **Removing learned words**: `Shift+Backspace` only removes candidates that
  come solely from the user dictionary, matching Gboard's long-press "remove
  suggestion". While such a candidate is highlighted, a "⇧⌫ 删除学习词" hint
  appears beside the settings button (only when 拼音显示位置 is set to
  候选窗口).
- **Undoing learning**: Backspace right after committing a learned phrase undoes
  that learning once and deletes the text as usual.

### Settings

Choose **设置…** in the input method menu, or click the gear button in the
candidate window, to open the settings window. Changes are saved automatically.

**输入 (Input)**

- **Pinyin scheme** (拼音方案): full Pinyin (default) or one of six Shuangpin
  schemes: Flypy, Ziranma, Microsoft, Smart ABC, Ziguang, or Pinyin Jiajia. The
  Pinyin strip shows the raw scheme keys. A change takes effect after the
  current composition ends and preserves partial selections and learning.
  Microsoft and Ziguang accept `;` while composing.
- **Shift toggle** (单击 Shift 切换中英文, on by default): pressing Shift
  switches between Chinese and English. English mode is temporary: switching
  back to Gboard, or moving to another app or text field, returns to Chinese.
  Turning it off returns to Chinese; it does not affect the system Caps Lock
  setting.
- **Traditional Chinese** (繁体中文, off by default): convert candidates and
  next-word predictions to Traditional Chinese (also `Cmd+Shift+F`, or
  切换为繁体中文 / 切换为简体中文 in the input menu).

**模糊音 (Fuzzy Pinyin)**

- Enable any of 12 pairs independently: z/zh, c/ch, s/sh, an/ang, en/eng,
  in/ing, l/n, f/h, r/l, k/g, ian/iang, and uan/uang. All are off by default.
  Changes take effect immediately without restarting and preserve the active
  Pinyin, explicit separators, and learned data.

**纠错 (Correction)**

- **Pinyin correction** (拼音纠错, on by default): Gboard's built-in corrector
  fixes swapped, missing, or extra letters in Pinyin spellings, for example
  `nihoa` → `你好`. Gboard itself has no switch for it; turning it off removes
  the `CorrectorSetting` that loads `PinyinCorrectionModel` from the decoder
  setting.
- **Shuangpin key correction** (双拼按键纠错, on by default): in Shuangpin
  input of four or more keys, also decodes each key as its left or right
  neighbour on the same keyboard row and the two keys of each syllable in
  swapped order. A typed `'` keeps the keys around it from being swapped.
  Corrected keys are marked in the Pinyin strip.
- **Correction marking style** (纠错标记): 删除线与插入高亮 (strikethrough and
  insert highlight, default: extra letters are struck out and inserted letters
  are shown in orange), 仅颜色 (color only), or 隐藏标记 (hidden). It affects
  display only.
- **Correction marking alignment** (纠错对齐): how fixes are aligned in the
  Pinyin strip:
  - 增删标记 (insert/delete marks, default): based on LCS.
  - 交换标记 (swap-aware marks): adjacent swapped letters are highlighted
    together in corrected order. For example, `nihoa` shows `ni'hao` with `ao`
    in orange.

  The change takes effect immediately, works with all three marking styles, and
  affects display only, not native candidate generation.

**候选与预测 (Candidates and predictions)**

- **Next-word prediction** (下一词预测, on by default).
- **Context awareness** (上下文感知, on by default): reads text near the cursor
  to improve ranking. When off, predictions use only the text just committed.
- **Candidates per page** (每页候选数): 5 to 9 (default 9).

**外观 (Appearance)**

- **Theme** (候选窗口主题): 跟随系统 (follow system, default), 浅色 (light), or
  深色 (dark), on all macOS versions. Before macOS 26 the light and dark themes
  use Gboard Google Blue colors.
- **Opacity** (候选窗口不透明度, before macOS 26 only): adjust the candidate
  window opacity (default 70%); lower values show more of the frosted glass. On
  macOS 26 and later, Liquid Glass transparency follows the system settings
  instead.
- **Pinyin display** (拼音显示位置): 候选窗口 (in the candidate window,
  default), 输入框（原生） (inline, using the app's native marked text), or 隐藏
  (hidden).

**快捷短语 (Shortcuts)**

- **Shortcuts** (启用快捷短语, off by default): when you type a complete
  abbreviation, its phrase appears as the first candidate. Manage the list in
  管理快捷短语…: add, edit, delete, search, and import or export it as JSON
  Lines. Abbreviations are 1–32 letters; phrases are single-line, up to 1,000
  characters, and may be Chinese, mixed, or plain ASCII such as an email
  address. Selecting a shortcut is not learned.

**个性化 (Personalization)**

- **Learning** (学习输入习惯, on by default): turning it off stops learning new
  input and keeps existing learned data.
- **Save interval** (用户词典自动保存间隔): 15 minutes, 30 minutes, 1 hour, or 4
  hours (default). The user dictionary is also saved when the input method
  exits.
- **Learned words** (管理学习词… button): lists learned Chinese and English
  words with their readings and counts. You can search, sort, delete multiple
  entries, edit counts, and export or import the list as a JSON Lines file.
  Import only adds missing words and raises lower counts.
- **Clear learning data** (清除学习数据… button): after confirmation, erases the
  learned records in both the Chinese and English user dictionaries. System
  dictionaries and settings are unaffected. This cannot be undone.

> [!NOTE]
> Removing a learned word that also exists in the system dictionary does not
> hide it from candidates. Words learned from abbreviated Pinyin (such as `cs`)
> are not listed, because Gboard's export omits them.

## How it works

Three layers make this possible:

1. **ELF loader**: loads the ARM64 `libintegrated_shared_object.so` directly
   into memory on macOS, handling relocations, symbol resolution, and TPIDR
   (thread-local storage) patching.
2. **JNI bridge**: fakes a minimal Android JNI environment so the native engine
   can initialize, enroll dictionary packs, and process input.
3. **macOS IME**: a Swift/InputMethodKit app that captures keystrokes, drives
   the engine, and shows candidates in a SwiftUI panel.

Implementation details:

- **Context window**: context is injected through the native TARGET_TOKEN path.
  It keeps the last 5 UTF-16 units for Chinese or 20 for Latin text and stops at
  punctuation, whitespace, or a language boundary.
- **Mixed input**: uses Gboard's English token and system dictionaries.
- **Shuangpin**: uses Gboard's native token dictionaries.
- **u-mode composition**: Gboard has no component data. The project
  maintains its own tables of about 7,400 characters
  (`chaizi_characters.tsv`) and component readings
  (`chaizi_components.tsv`), written for this project rather than taken
  from a third-party dataset. Each build compiles them with
  `scripts/gen_chaizi_tables.py` into static lookup arrays, which
  matching reads in place without parsing.
- **Shuangpin key correction**: Gboard's built-in corrector targets full
  Pinyin spellings. Key correction reuses the native input lattice instead:
  each key is appended with its same-row neighbours as lower-scored
  `ScoredInput` alternatives, as Gboard's touch spatial model does, and the
  two keys of each syllable get a swapped input edge, as Gboard's Korean
  decoder does. Shorter input stays literal to keep syllables and
  initial-letter abbreviations.
- **Traditional Chinese**: user-dictionary learning and context scoring always
  use the Simplified text.
- **User dictionary**: Chinese and mixed phrases are stored in `user_dict_3_3`
  and pure English words in `user_dict_3_3_english`, both under
  `~/Library/Application Support/GboardIME/`. Both are persisted every four
  hours by default (configurable in Settings) and on app teardown.
- **Correction alignment**: insert/delete marks use LCS; swap-aware marks use
  restricted Damerau-Levenshtein (DL/OSA).
- **Liquid Glass**: on macOS 26 and later the candidate window uses
  `NSGlassEffectView` following Apple's guidelines: untinted regular glass,
  system label colors, and an accent-colored selection. Transparency follows the
  system Appearance and Reduce Transparency settings.

## Development

### setup.sh options

```
--xapk PATH     Path to Gboard XAPK or APK file (required)
--dict PATH     Use a local dict zip instead of downloading
--locale CODE   Dictionary locale: zh_CN (default), zh_TW, zh_HK, ko
```

### build.sh commands

```
build       Build the app (default)
install     Build, install to ~/Library/Input Methods, and sign
uninstall   Remove from ~/Library/Input Methods
clean       Remove build artifacts
```

### Decompiling (optional)

`./decompile.sh` decompiles the Java sources into `gboard_apk_source/jadx/` for
reverse engineering. It requires `brew install jadx` and is not needed to build.
By default it reads `xapk_unpacked/com.google.android.inputmethod.latin.apk`; if
that file doesn't exist (for example, `setup.sh` was given a plain APK), pass it
with `./decompile.sh --apk /path/to/gboard.apk`.

### Tests

```bash
./test.sh
```

Runs both the C engine tests and the Swift IME tests.

## Legal

This is an independent research project. Google, Gboard, and Android are
trademarks of Google LLC. Not affiliated with or endorsed by Google. See
[NOTICE](NOTICE).

Contributions must follow the copyright and legal principles in
[CONTRIBUTING](CONTRIBUTING.md).
