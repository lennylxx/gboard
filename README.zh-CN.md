# Gboard for macOS

[English](README.md) | [简体中文](README.zh-CN.md)

GboardIME 是一款适用于 macOS 的中文输入法。本项目通过自定义 ARM64 ELF
加载器，在 macOS 上运行 Android 版 Gboard 使用的原生 HMM 拼音引擎。

> **本项目不包含任何 Google 二进制文件或数据。** 用户须自行提供通过合法
> 途径获取的 Gboard APK。详情请参阅 [NOTICE](NOTICE)。

## 预览

![预览](preview.png)

## 功能列表

- **原生拼音引擎**：在搭载 Apple 芯片的 Mac 上直接运行 Gboard 原生 HMM
  拼音引擎。
- **离线候选生成与排序**：候选生成和神经语言模型重排序均在本地完成。
- **上下文感知排序**：利用光标前已上屏的文本优化候选顺序。实现沿用
  Gboard 原生的 TARGET_TOKEN 上下文注入机制：中文保留最近 5 个 UTF-16
  代码单元，英文保留最近 20 个；遇到标点、空白字符或语言边界时停止截取。
  例如，直接输入 `bushu` 时首选通常为“部署”，在“我对这里很”之后输入时，
  首选会调整为“不熟”。
- **分段选词**：选择候选词时仅消耗与其对应的拼音，未消耗的部分会保留并
  继续参与转换。例如，输入 `nihao` 后选择“你”，`hao` 会保留用于后续选词。
- **拼音切分显示**：根据引擎的 segment/token 结果显示实际拼音切分，例如
  `fang'an`；输入撇号强制分隔时会显示 `xi'an`。
- **用户词典自动学习**：自动学习用户选择的中文词组，并写入
  `user_dict_3_3`。词典保存在
  `~/Library/Application Support/GboardIME/`，每 4 小时以及应用退出时
  自动持久化。
- **完整的键盘与中英切换**：支持候选翻页、键盘选词，以及可配置的中英文切换按键（Shift、Caps Lock 或关闭）。
- **简繁体实时转换**：支持通过 `Ctrl+Shift+F` 快捷键、状态栏菜单或偏好设置即时切换繁体中文，实时更新候选词与上屏文本。
- **标点与符号模式（?123）**：支持 `Ctrl+1` 或 `Option+?` 快速唤出 4 页常用中文标点、全半角特殊符号与货币符号。
- **毛玻璃外观与偏好设置**：原生 `NSVisualEffectView` 半透明毛玻璃候选词窗口，自适应浅色与深色外观，提供直观的偏好设置面板。
- **原生 macOS 界面**：基于 InputMethodKit 和 SwiftUI 实现输入法与候选词
  窗口。

**自动学习限制：** 当前不支持撤销某次上屏操作产生的学习记录。
InputMethodKit 无法可靠区分“撤销已上屏文本”和普通文本编辑。

## 技术架构

项目主要由以下三部分组成：

1. **ELF 加载器** — 将 ARM64 版本的
   `libintegrated_shared_object.so` 加载到 macOS 进程中，并处理重定位、
   符号解析和 TPIDR（线程本地存储）修补。
2. **JNI 兼容层** — 提供原生引擎运行所需的最小 Android JNI 环境，负责
   初始化引擎、加载词典数据并转发输入请求。
3. **macOS 输入法前端** — 基于 Swift、SwiftUI 和 InputMethodKit 实现，
   负责处理按键事件、调用引擎并显示候选词窗口。

## 环境要求

- 搭载 Apple 芯片且运行 macOS 13.0 或更高版本的 Mac
- Xcode 或 Xcode Command Line Tools
- `jadx` 和 `jq`

可通过 Homebrew 安装所需的命令行工具：

```bash
brew install jadx jq
```

## 快速开始

```bash
# 1. 解包 APK 并获取词典数据
./setup.sh --xapk /path/to/com.google.android.inputmethod.latin.xapk

# 2. 构建并安装输入法
./build.sh install

# 3. 注销并重新登录，然后前往：
#    系统设置 → 键盘 → 文字输入 → 编辑 → + → 简体中文 → GboardIME
```

### `setup.sh` 选项

```
--xapk PATH     Gboard XAPK 文件路径（必填）
--dict PATH     使用本地词典 ZIP 文件，而不从网络下载
--locale CODE   数据区域：zh_CN（默认）、zh_TW、zh_HK 或 ko
```

### `build.sh` 命令

```
build       构建应用（默认）
install     构建并签名，然后安装到 ~/Library/Input Methods
uninstall   从 ~/Library/Input Methods 中卸载
clean       清理构建产物
```

## 使用方法

从菜单栏的输入法菜单中切换到 GboardIME，即可开始输入拼音。

| 按键 | 操作 |
|------|------|
| `a-z` | 输入拼音字母 |
| `1-9` | 选择当前页中对应序号的候选词 |
| `-` | 显示上一页候选词 |
| `=` | 显示下一页候选词 |
| `空格` | 上屏当前选中的候选词 |
| `←` / `→` | 移动候选词光标，支持跨页选择 |
| `退格` | 删除最后一个拼音字母 |
| `Esc` | 取消当前输入 |
| `回车` | 直接上屏未转换的拼音 |
| `Shift` | 切换中英文模式；若存在未完成的拼音，则先将其原样上屏 |
| `Caps Lock` | 在 macOS“文字输入”设置中启用后，可在 GboardIME 与 ABC 输入法之间切换；若存在未完成的拼音，则先将其原样上屏 |

## 测试

```bash
./test.sh
```

该脚本会依次运行 C 语言引擎测试和 Swift 输入法测试。

## 法律声明

本项目为独立研究项目，与 Google 无关，也未获得 Google 的认可或支持。
Google、Gboard 和 Android 均为 Google LLC 的商标。详情请参阅
[NOTICE](NOTICE)。
