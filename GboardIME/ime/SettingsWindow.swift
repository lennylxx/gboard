import Cocoa
import SwiftUI

private struct SettingsView: View {
    @ObservedObject private var preferences = Preferences.shared
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
                        toggle("单击 Shift 切换中英文", isOn: $preferences.shiftToggleEnabled)
                        note("关闭时恢复中文；不影响系统 Caps Lock 设置。")
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
                            note("例：nihoa →")
                            Text(correctionExample)
                        }
                        .help("只调整标记显示，不影响实际纠错。")
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
        var example = PinyinReading(spans: [
            .init(text: "ni'h", isCorrected: false),
            .init(text: "o", isCorrected: false, isTypo: true),
            .init(text: "a", isCorrected: false),
            .init(text: "o", isCorrected: true)
        ]).styled(preferences.correctionStyle).attributedText
        example.font = .system(size: 12)
        return example
    }
}

final class SettingsWindowController: NSWindowController, NSWindowDelegate {
    static let shared = SettingsWindowController()

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
        if !NSApp.setActivationPolicy(.prohibited) {
            imeLog("Unable to restore background activation policy")
        }
    }
}
