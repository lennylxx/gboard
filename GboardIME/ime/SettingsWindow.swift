import Cocoa
import SwiftUI

private struct SettingsView: View {
    @ObservedObject private var preferences = Preferences.shared
    @State private var confirmClear = false
    @State private var showClearError = false
    @State private var clearError = ""
    @State private var didClear = false
    @State private var showFuzzyError = false
    @State private var showPinyinSchemeError = false
    let onClose: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Image(systemName: "slider.horizontal.3")
                    .font(.system(size: 28, weight: .regular))
                    .foregroundStyle(.tint)
                    .frame(width: 40, height: 40)
                    .accessibilityHidden(true)

                VStack(alignment: .leading, spacing: 3) {
                    Text("GboardIME 设置")
                        .font(.system(size: 16, weight: .semibold))
                    Text("自定义输入体验")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                }
                Spacer()
            }
            .padding(.horizontal, 20)
            .padding(.top, 16)
            .padding(.bottom, 12)

            Divider()

            ScrollView {
                VStack(spacing: 10) {
                    section("输入", systemImage: "keyboard") {
                        HStack {
                            Text("拼音方案")
                            Spacer()
                            Picker("拼音方案", selection: pinyinSchemeBinding) {
                                ForEach(PinyinScheme.allCases, id: \.self) { scheme in
                                    Text(scheme.title).tag(scheme)
                                }
                            }
                            .labelsHidden()
                            .pickerStyle(.menu)
                            .frame(width: 150)
                        }
                        note("双拼使用原生键位，拼音栏显示双拼按键。切换在本次输入结束后生效。")
                        if preferences.pinyinSchemeApplyFailed {
                            Label("上次拼音方案切换失败", systemImage: "exclamationmark.triangle")
                                .foregroundStyle(.orange)
                            note("当前保存方案为「\(preferences.pinyinScheme.title)」；若输入暂停，请重新应用。")
                            Button("重新应用「\(preferences.pinyinScheme.title)」") {
                                if !preferences.applyPinyinScheme() {
                                    showPinyinSchemeError = true
                                }
                            }
                        } else if preferences.pinyinSchemePending {
                            note("等待本次输入结束后切换。")
                        }
                        toggle("单击 Shift 切换中英文", isOn: $preferences.shiftToggleEnabled)
                        note("关闭时恢复中文；不影响系统 Caps Lock 设置。")
                        toggle("繁体中文", isOn: $preferences.isTraditional)
                        note("开启后候选词与上屏内容实时转换为繁体中文；中文模式下可按 ⇧⌘F 切换，按 ⌃1 或 ⌥? 打开 ?123 符号面板。")
                    }
                    section("模糊音", systemImage: "character.phonetic") {
                        note("按需勾选易混淆的读音，立即生效，无需重启。默认全部关闭。")
                        if preferences.fuzzyPinyinApplyFailed {
                            Label("模糊音设置未生效", systemImage: "exclamationmark.triangle")
                                .foregroundStyle(.orange)
                            note("已保留原勾选项，输入暂使用引擎原有配置。")
                            Button("重试应用") {
                                if !preferences.applyFuzzyPinyin() {
                                    showFuzzyError = true
                                }
                            }
                        }
                        LazyVGrid(columns: [
                            GridItem(.flexible()), GridItem(.flexible())
                        ], alignment: .leading, spacing: 8) {
                            ForEach(FuzzyPinyinGroup.allCases, id: \.self) { group in
                                Toggle(group.title, isOn: fuzzyBinding(for: group))
                                    .toggleStyle(.checkbox)
                            }
                        }
                    }
                    section("候选与预测", systemImage: "list.bullet.rectangle") {
                        toggle("下一词预测", isOn: $preferences.predictionsEnabled)
                        note("例：生日快 → 乐")
                        toggle("上下文感知", isOn: $preferences.contextEnabled)
                        note("读取光标附近文字以改善排序；关闭后仅使用本次上屏内容预测。")
                        HStack {
                            Text("每页候选数")
                            Spacer()
                            Picker("每页候选数", selection: $preferences.candidatePageSize) {
                                ForEach(CandidatePageSize.allCases, id: \.self) { size in
                                    Text("\(size.rawValue) 个").tag(size)
                                }
                            }
                            .labelsHidden()
                            .pickerStyle(.menu)
                            .frame(width: 80)
                        }
                    }
                    section("外观", systemImage: "paintpalette") {
                        HStack {
                            Text("候选窗口主题")
                            Spacer()
                            Picker("候选窗口主题", selection: $preferences.themeMode) {
                                ForEach(ThemeMode.allCases, id: \.self) { theme in
                                    Text(theme.title).tag(theme)
                                }
                            }
                            .labelsHidden()
                            .pickerStyle(.segmented)
                            .frame(width: 270)
                        }
                        HStack {
                            Text("拼音显示位置")
                            Spacer()
                            Picker("拼音显示位置", selection: $preferences.pinyinDisplayMode) {
                                ForEach(PinyinDisplayMode.allCases, id: \.self) { mode in
                                    Text(mode.title).tag(mode)
                                }
                            }
                            .labelsHidden()
                            .pickerStyle(.segmented)
                            .frame(width: 270)
                            .help("输入框模式使用应用原生的 marked text。")
                        }
                        HStack {
                            Text("纠错对齐")
                            Spacer()
                            Picker("纠错对齐", selection: $preferences.correctionAlignment) {
                                ForEach(CorrectionAlignment.allCases, id: \.self) { alignment in
                                    Text(alignment.title).tag(alignment)
                                }
                            }
                            .labelsHidden()
                            .pickerStyle(.segmented)
                            .frame(width: 270)
                            .help("交换标记将相邻颠倒的两个字母按正确顺序一起标色；不改变实际纠错。")
                        }
                        HStack {
                            Text("纠错标记")
                            Spacer()
                            Picker("纠错标记", selection: $preferences.correctionStyle) {
                                ForEach(CorrectionStyle.allCases, id: \.self) { style in
                                    Text(style.title).tag(style)
                                }
                            }
                            .labelsHidden()
                            .pickerStyle(.segmented)
                            .frame(width: 270)
                        }
                        HStack(alignment: .firstTextBaseline, spacing: 8) {
                            note("例：niihoa →")
                            Text(correctionExample)
                        }
                        .help("只调整候选窗口中的拼音标记，不影响原生组合文本或实际纠错。")
                    }
                    section("个性化", systemImage: "person.crop.circle") {
                        toggle("学习输入习惯", isOn: $preferences.learningEnabled)
                        note("关闭后停止学习新输入，已有学习数据保留。")
                        HStack {
                            Text("用户词典自动保存间隔")
                            Spacer()
                            Picker("用户词典自动保存间隔",
                                   selection: $preferences.userDictionarySaveInterval) {
                                ForEach(UserDictionarySaveInterval.allCases, id: \.self) { interval in
                                    Text(interval.title).tag(interval)
                                }
                            }
                            .labelsHidden()
                            .pickerStyle(.menu)
                            .frame(width: 100)
                        }
                        note("退出输入法时也会保存。")
                        HStack {
                            Button("清除学习数据…", role: .destructive) {
                                confirmClear = true
                            }
                            .alert("清除学习数据？", isPresented: $confirmClear) {
                                Button("取消", role: .cancel) {}
                                Button("清除", role: .destructive) {
                                    didClear = false
                                    switch UserDictionary.shared.clearLearningData() {
                                    case .success:
                                        didClear = true
                                    case .failure(let error):
                                        clearError = error.localizedDescription
                                        showClearError = true
                                    }
                                }
                            } message: {
                                Text("将清除中英文用户词典中的学习记录，不影响系统词典或设置。此操作无法撤销。")
                            }
                            Spacer()
                            if didClear {
                                note("学习数据已清除。")
                            }
                        }
                    }
                }
                .toggleStyle(.switch)
                .font(.system(size: 13))
                .controlSize(.regular)
                .padding(16)
            }

            Divider()

            HStack {
                Text("更改自动保存")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                Spacer()
                Button("完成", action: onClose)
                    .keyboardShortcut(.defaultAction)
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 10)
        }
        .frame(width: 460, height: 530)
        .alert("无法清除学习数据", isPresented: $showClearError) {
            Button("好", role: .cancel) {}
        } message: {
            Text(clearError)
        }
        .alert("无法更新模糊音设置", isPresented: $showFuzzyError) {
            Button("好", role: .cancel) {}
        } message: {
            Text("原生引擎未能切换模糊音，已保留原设置和输入内容，请重试。")
        }
        .alert("无法切换拼音方案", isPresented: $showPinyinSchemeError) {
            Button("好", role: .cancel) {}
        } message: {
            Text("原生引擎未能完成方案切换。已保留原设置；若输入暂停，请重试应用。")
        }
    }

    private var pinyinSchemeBinding: Binding<PinyinScheme> {
        Binding(
            get: { preferences.pinyinScheme },
            set: {
                if !preferences.applyPinyinScheme($0) {
                    showPinyinSchemeError = true
                }
            }
        )
    }

    private func fuzzyBinding(for group: FuzzyPinyinGroup) -> Binding<Bool> {
        Binding(
            get: { preferences.fuzzyPinyinGroups.contains(group) },
            set: { enabled in
                var groups = preferences.fuzzyPinyinGroups
                if enabled { groups.insert(group) } else { groups.remove(group) }
                guard preferences.applyFuzzyPinyin(groups) else {
                    showFuzzyError = true
                    return
                }
            }
        )
    }

    private func section<Content: View>(
        _ title: String, systemImage: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                Label(title, systemImage: systemImage)
                    .font(.system(size: 13, weight: .medium))
                    .labelStyle(.titleAndIcon)
                content()
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(8)
        }
    }

    private func toggle(_ title: String, isOn: Binding<Bool>) -> some View {
        HStack(spacing: 8) {
            Text(title)
            Spacer()
            Toggle(title, isOn: isOn)
                .labelsHidden()
        }
    }

    private func note(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 12))
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }

    private var correctionExample: AttributedString {
        var example = PinyinReading(
            spans: PinyinSession.correctionSpans(
                typed: "nii", corrected: "ni",
                alignment: preferences.correctionAlignment) +
                [.init(text: "'", isCorrected: false)] +
                PinyinSession.correctionSpans(
                    typed: "hoa", corrected: "hao",
                    alignment: preferences.correctionAlignment)
        ).styled(preferences.correctionStyle).attributedText
        example.font = .system(size: 12)
        return example
    }
}

final class SettingsWindowController: NSWindowController, NSWindowDelegate {
    static let shared = SettingsWindowController()
    private var previousActiveApp: NSRunningApplication?

    private init() {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 460, height: 530),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered,
            defer: false
        )
        window.title = "GboardIME 设置"
        window.isReleasedWhenClosed = false
        window.hidesOnDeactivate = false
        window.contentView = NSHostingView(rootView: SettingsView {
            [weak window] in window?.performClose(nil)
        })
        window.center()
        super.init(window: window)
        window.delegate = self
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func show() {
        if let frontmost = NSWorkspace.shared.frontmostApplication,
           frontmost.bundleIdentifier != Bundle.main.bundleIdentifier {
            previousActiveApp = frontmost
        }
        // Input methods start as background-only processes.
        if NSApp.activationPolicy() != .accessory,
           !NSApp.setActivationPolicy(.accessory) {
            imeLog("Unable to activate settings window")
            return
        }
        window?.deminiaturize(nil)
        showWindow(nil)
        NSApp.activate(ignoringOtherApps: true)
        window?.makeKeyAndOrderFront(nil)
    }

    func windowWillClose(_ notification: Notification) {
        let appToRestore = previousActiveApp
        previousActiveApp = nil
        if !NSApp.setActivationPolicy(.prohibited) {
            imeLog("Unable to restore background activation policy")
        }
        if let app = appToRestore, !app.isTerminated {
            app.activate(options: [.activateIgnoringOtherApps])
        }
    }
}
