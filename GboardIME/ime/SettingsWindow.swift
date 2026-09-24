import Cocoa
import SwiftUI

struct SettingsView: View {
    @ObservedObject var prefs = PreferencesManager.shared

    var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack(spacing: 12) {
                ZStack {
                    Circle()
                        .fill(LinearGradient(
                            colors: [Color(red: 0.10, green: 0.45, blue: 0.91), Color(red: 0.20, green: 0.60, blue: 0.95)],
                            startPoint: .topLeading,
                            endPoint: .bottomTrailing
                        ))
                        .frame(width: 44, height: 44)
                    Text("G")
                        .font(.system(size: 24, weight: .bold, design: .rounded))
                        .foregroundColor(.white)
                }

                VStack(alignment: .leading, spacing: 2) {
                    Text("Gboard 偏好设置")
                        .font(.system(size: 16, weight: .semibold))
                    Text("适用于 macOS 的原生 HMM 拼音输入法")
                        .font(.system(size: 12))
                        .foregroundColor(.secondary)
                }
                Spacer()
            }
            .padding(.horizontal, 20)
            .padding(.top, 18)
            .padding(.bottom, 14)

            Divider()

            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    // 1. 中英输入切换
                    GroupBox {
                        VStack(alignment: .leading, spacing: 10) {
                            HStack {
                                Label("中英文切换按键", systemImage: "keyboard")
                                    .font(.system(size: 13, weight: .medium))
                                Spacer()
                            }

                            Picker("", selection: $prefs.switchMode) {
                                ForEach(ChineseEnglishSwitchMode.allCases, id: \.self) { mode in
                                    Text(mode.displayName).tag(mode)
                                }
                            }
                            .pickerStyle(.radioGroup)

                            Text("提示：若玩游戏或编码时容易误触 Shift，可选择关闭或改用 Caps Lock。")
                                .font(.system(size: 11))
                                .foregroundColor(.secondary)
                        }
                        .padding(6)
                    }

                    // 2. 简繁体切换
                    GroupBox {
                        VStack(alignment: .leading, spacing: 10) {
                            HStack {
                                Label("繁体中文模式", systemImage: "character.book.closed")
                                    .font(.system(size: 13, weight: .medium))
                                Spacer()
                                Toggle("", isOn: $prefs.isTraditional)
                                    .toggleStyle(.switch)
                            }

                            HStack {
                                Text("快捷键快速切换：")
                                    .font(.system(size: 12))
                                    .foregroundColor(.secondary)
                                Text("Command + Shift + F")
                                    .font(.system(size: 12, weight: .semibold, design: .monospaced))
                                    .padding(.horizontal, 6)
                                    .padding(.vertical, 2)
                                    .background(Color.secondary.opacity(0.15))
                                    .cornerRadius(4)
                                Spacer()
                                Button(prefs.isTraditional ? "切回简体" : "开启繁体") {
                                    prefs.toggleTraditional()
                                }
                                .buttonStyle(.bordered)
                                .controlSize(.small)
                            }

                            Text("开启后输入候选词与上屏文本均实时转换为标准繁体。")
                                .font(.system(size: 11))
                                .foregroundColor(.secondary)
                        }
                        .padding(6)
                    }

                    // 3. 标点和数字快捷键 (?123)
                    GroupBox {
                        VStack(alignment: .leading, spacing: 10) {
                            HStack {
                                Label("标点与数字模式 (?123)", systemImage: "number")
                                    .font(.system(size: 13, weight: .medium))
                                Spacer()
                            }

                            HStack {
                                Text("快速唤出快捷键：")
                                    .font(.system(size: 12))
                                    .foregroundColor(.secondary)
                                Text("Control + 1  或  Option + ?")
                                    .font(.system(size: 12, weight: .semibold, design: .monospaced))
                                    .padding(.horizontal, 6)
                                    .padding(.vertical, 2)
                                    .background(Color.secondary.opacity(0.15))
                                    .cornerRadius(4)
                            }

                            Text("如同手机 Gboard 的 ?123 键，唤出后可按 1-9 快速选择输入常用中文标点、全半角特殊符号与货币符号。")
                                .font(.system(size: 11))
                                .foregroundColor(.secondary)
                        }
                        .padding(6)
                    }

                    // 4. 外观与候选词
                    GroupBox {
                        VStack(alignment: .leading, spacing: 10) {
                            HStack {
                                Label("外观主题与候选词", systemImage: "paintpalette")
                                    .font(.system(size: 13, weight: .medium))
                                Spacer()
                            }

                            HStack {
                                Text("悬浮窗主题：")
                                    .font(.system(size: 12))
                                Picker("", selection: $prefs.themeMode) {
                                    ForEach(ThemeMode.allCases, id: \.self) { theme in
                                        Text(theme.displayName).tag(theme)
                                    }
                                }
                                .pickerStyle(.segmented)
                                .frame(width: 240)
                            }

                            HStack {
                                Text("每页候选词数量：")
                                    .font(.system(size: 12))
                                Picker("", selection: $prefs.candidatePageSize) {
                                    Text("5 个").tag(5)
                                    Text("7 个").tag(7)
                                    Text("9 个 (推荐)").tag(9)
                                }
                                .pickerStyle(.menu)
                                .frame(width: 140)
                            }
                        }
                        .padding(6)
                    }
                }
                .padding(20)
            }

            Divider()

            HStack {
                let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "1.0"
                Text("Gboard for macOS v\(version)")
                    .font(.system(size: 11))
                    .foregroundColor(.secondary)
                Spacer()
                Button("完成") {
                    SettingsWindowController.shared.close()
                }
                .keyboardShortcut(.defaultAction)
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 12)
            .background(Color(NSColor.windowBackgroundColor))
        }
        .frame(width: 460, height: 530)
    }
}

final class SettingsWindowController: NSObject {
    static let shared = SettingsWindowController()

    private var window: NSWindow?

    func show() {
        if window == nil {
            let win = NSWindow(
                contentRect: NSRect(x: 0, y: 0, width: 460, height: 530),
                styleMask: [.titled, .closable, .miniaturizable],
                backing: .buffered,
                defer: false
            )
            win.title = "Gboard 偏好设置"
            win.center()
            win.isReleasedWhenClosed = false
            win.level = .floating
            win.contentView = NSHostingView(rootView: SettingsView())
            window = win
        }
        window?.center()
        window?.makeKeyAndOrderFront(nil)
        window?.orderFrontRegardless()
        NSApp.activate(ignoringOtherApps: true)
    }

    func close() {
        window?.orderOut(nil)
    }
}
