# Gboard for macOS

[English](README.md) | [简体中文](README.zh-CN.md)

A macOS input method that runs Gboard's native HMM Pinyin engine via a custom ARM64 ELF loader. Type Mandarin Chinese on macOS using the same engine that powers Gboard on Android.

> **This project does not include any Google binaries or data.** You must supply your own legally obtained Gboard APK. See [NOTICE](NOTICE) for details.

## Preview

![preview](preview.png)

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
- **Automatic user-dictionary learning** — Stores selected Chinese phrases in
  `user_dict_3_3` under `~/Library/Application Support/GboardIME/` and
  persists the dictionary every four hours and on app teardown.
- **Complete keyboard workflow** — Supports candidate paging, keyboard
  selection, and Chinese/English mode switching.
- **Native macOS interface** — Uses InputMethodKit and SwiftUI for the input
  method and candidate window.

**Learning limitation:** Undo of a just-committed learned entry is not
supported. InputMethodKit does not reliably expose the context needed to
distinguish it from normal editing.

## How it works

Three layers make this possible:

1. **ELF loader** — Loads the ARM64 `libintegrated_shared_object.so` directly into memory on macOS. Handles relocations, symbol resolution, and TPIDR (thread-local storage) patching.
2. **JNI bridge** — Fakes a minimal Android JNI environment so the native engine can initialize, enroll dictionary packs, and process input.
3. **macOS IME** — A Swift/InputMethodKit app that captures keystrokes, drives the engine, and shows candidates in a SwiftUI panel.

## Prerequisites

- macOS 13.0+ on Apple Silicon
- Xcode (command line tools)
- `brew install jadx jq`

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

### build.sh commands

```
build       Build the app (default)
install     Build, install to ~/Library/Input Methods, and sign
uninstall   Remove from ~/Library/Input Methods
clean       Remove build artifacts
```

## Usage

Switch to GboardIME from the menu bar input source picker, then type pinyin.

| Key | Action |
|-----|--------|
| a-z | Append to pinyin composition |
| 1-9 | Select nth candidate |
| - | Show previous candidate page |
| = | Show next candidate page |
| Space | Commit highlighted candidate |
| ← → | Move candidate selection, crossing page boundaries |
| Backspace | Delete last pinyin character |
| Escape | Cancel composition |
| Return | Commit raw pinyin |
| Shift | Switch between Chinese and English; commit active composition as raw pinyin |
| Caps Lock | Switch between GboardIME and ABC when enabled in macOS Text Input settings; commit active composition as raw pinyin |

## Test

```bash
./test.sh
```

Runs both C engine tests and Swift IME tests.

## Legal

This is an independent research project. Google, Gboard, and Android are trademarks of Google LLC. Not affiliated with or endorsed by Google. See [NOTICE](NOTICE).
