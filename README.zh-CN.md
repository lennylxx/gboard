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
- **拼音纠错**：使用 Gboard 内置纠错，处理字母颠倒、漏打或多打。例如，
  `nihoa` 得到“你好”，`zhonguo` 得到“中国”。拼音条会标出改动：多打的
  字母加删除线，补上的字母显示为橙色。
- **中英文混合输入**：使用 Gboard 的英文 token 与系统词典，在拼音输入
  中直接给出英文单词。例如，`woyaogithub` 得到“我要GitHub”，
  `yongpythonxie` 得到“用Python写”。
- **中文下一词预测**：候选上屏后，根据最近上屏的中文显示引擎给出的下一词
  预测。按 `空格` 上屏第一个预测，按 `1-9` 或点击上屏对应预测，上屏后继续
  预测；按 `-`/`=` 翻页；按其他任意键会关闭预测，并按正常方式处理该按键。例如，上屏“生日快”后会预测“乐”。
- **用户词典自动学习**：自动学习用户选择的中文和中英混合词组，写入
  `user_dict_3_3`；纯英文单词写入 `user_dict_3_3_english`。词典保存在
  `~/Library/Application Support/GboardIME/`，每 4 小时以及应用退出时
  自动持久化。
- **完整的键盘操作**：支持候选翻页、键盘选词和中英文模式切换。
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
- Xcode 或 Xcode Command Line Tools（未安装 Xcode 时，`build.sh` 会改用
  `swiftc` 构建）
- `jq`

可通过 Homebrew 安装所需的命令行工具：

```bash
brew install jq
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

可选：`./decompile.sh` 会把 Java 源码反编译到 `gboard_apk_source/jadx/`，用于逆向分析（需要 `brew install jadx`；构建时不需要）。

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
| `1-9` | 选择当前页中对应序号的候选词或下一词预测 |
| `-` | 显示上一页候选词或预测 |
| `=` | 显示下一页候选词或预测 |
| `空格` | 上屏当前选中的候选词或第一个预测 |
| `←` / `→` | 移动候选词光标，支持跨页选择 |
| `退格` | 删除最后一个拼音字母 |
| `Esc` | 取消当前输入或关闭下一词预测 |
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

提交贡献须遵守[贡献指南](CONTRIBUTING.zh-CN.md)中的版权与法律原则。
