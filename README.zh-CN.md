# Gboard for macOS

[English](README.md) | [简体中文](README.zh-CN.md)

GboardIME 是一款适用于 macOS 的中文输入法。项目通过自定义 ARM64 ELF 加载器，在 macOS 上运行 Android 版 Gboard 所使用的原生 HMM 拼音引擎。

> **本项目不包含任何 Google 二进制文件或数据。** 使用者须自行提供通过合法途径获取的 Gboard APK。详情请参阅 [NOTICE](NOTICE)。

## 预览

![预览](preview.png)

## 技术架构

项目由以下三部分组成：

1. **ELF 加载器** — 在 macOS 上将 ARM64 版本的 `libintegrated_shared_object.so` 加载至内存，并完成重定位、符号解析和 TPIDR（线程本地存储）修补。
2. **JNI 桥接层** — 提供原生引擎运行所需的最小化 Android JNI 环境，负责引擎初始化、词典数据加载和输入处理。
3. **macOS 输入法前端** — 基于 Swift 和 InputMethodKit 实现，负责按键事件处理、引擎调用，以及通过 SwiftUI 显示候选词窗口。

## 环境要求

- 搭载 Apple 芯片并运行 macOS 13.0 或更高版本的 Mac
- Xcode 或 Xcode Command Line Tools
- `brew install jadx jq`

## 快速开始

```bash
# 1. 解包 APK 并下载词典数据
./setup.sh --xapk /path/to/com.google.android.inputmethod.latin.xapk

# 2. 构建并安装
./build.sh install

# 3. 注销并重新登录，然后打开：
#    系统设置 → 键盘 → 文字输入 → 编辑 → + → 简体中文 → GboardIME
```

### setup.sh 选项

```
--xapk PATH     Gboard XAPK 文件路径（必填）
--dict PATH     使用本地词典 ZIP 文件，不从网络下载
--locale CODE   zh_CN（默认）、zh_TW、zh_HK、ko
```

### build.sh 命令

```
build       构建应用（默认）
install     构建、签名并安装到 ~/Library/Input Methods
uninstall   从 ~/Library/Input Methods 中移除
clean       清理构建产物
```

## 使用方法

从菜单栏的输入法菜单切换至 GboardIME，然后输入拼音。

| 按键 | 操作 |
|------|------|
| a-z | 输入拼音字母 |
| 1-9 | 选择当前页对应序号的候选词 |
| - | 上一页 |
| = | 下一页 |
| 空格 | 上屏当前选中的候选词 |
| ← → | 移动候选词选择，支持跨页 |
| 退格 | 删除最后一个拼音字母 |
| Esc | 取消当前输入 |
| 回车 | 直接上屏未转换的拼音 |
| Shift | 切换中英文模式；若存在未完成的输入，则先上屏未转换的拼音 |
| Caps Lock | 在 macOS“文字输入”设置中启用相应选项后，可在 GboardIME 与 ABC 之间切换；若存在未完成的输入，则先上屏未转换的拼音 |

支持分段选词。选择候选词时，仅消耗该候选词对应的拼音，其余拼音将保留并继续参与转换。例如，输入 `nihao` 后选择 `你`，`hao` 将保留用于后续选词。

### 自动学习

GboardIME 会自动学习用户的候选词选择。每次上屏的中文词组将被记录至用户词典（`user_dict_3_3`），存储路径为 `~/Library/Application Support/GboardIME/`。词典每 4 小时及应用退出时自动持久化，重启后保留。通过使用逐步个性化候选词排序。

**限制：** 当前架构下不支持撤销已上屏词条的学习。InputMethodKit 无法可靠地区分"撤销已上屏文本"与普通编辑操作。

## 测试

```bash
./test.sh
```

该脚本会依次运行 C 引擎测试和 Swift 输入法测试。

## 法律声明

本项目为独立研究项目，与 Google 无关，亦未获得 Google 的认可或支持。Google、Gboard 和 Android 均为 Google LLC 的商标。详情请参阅 [NOTICE](NOTICE)。
