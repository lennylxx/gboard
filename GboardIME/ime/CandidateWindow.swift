import Cocoa
import SwiftUI

// MARK: - Non-Activating Panel
// CRITICAL: Must be an NSPanel with canBecomeKey = false so mouse clicks
// (e.g. clicking candidates, paging chevrons, symbols, or gear button)
// never steal keyboard focus from the active client application.
// Otherwise, macOS IMKit deactivates the input controller and commits/resets input.

final class CandidatePanel: NSPanel {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

// MARK: - AppKit First-Mouse Hosting View
// Allows single-click interaction on background/floating windows without needing a preliminary click to activate.

final class FirstMouseHostingView<Content: View>: NSHostingView<Content> {
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool {
        return true
    }

    override func hitTest(_ point: NSPoint) -> NSView? {
        return super.hitTest(point)
    }
}

// MARK: - Modern Candidate View (Crisp Google Gboard Palette with Native Glass Accent)

struct CandidateView: View {
    let candidates: [String]
    let pinyin: String
    let selectedIndex: Int
    let canGoPrevious: Bool
    let canGoNext: Bool
    let onSelect: (Int) -> Void
    let onPrevious: () -> Void
    let onNext: () -> Void

    @Environment(\.colorScheme) private var systemColorScheme
    @ObservedObject private var prefs = PreferencesManager.shared

    private var isDark: Bool {
        switch prefs.themeMode {
        case .system: return systemColorScheme == .dark
        case .light: return false
        case .dark: return true
        }
    }

    // Google Gboard signature blue
    private var accentColor: Color {
        if isDark {
            return Color(red: 0.54, green: 0.71, blue: 0.97) // Google Blue 200
        } else {
            return Color(red: 0.098, green: 0.451, blue: 0.910) // Google Blue 600 (#1973E8)
        }
    }

    private var textColor: Color {
        if isDark {
            return Color(red: 0.95, green: 0.96, blue: 0.98)
        } else {
            return Color(red: 0.125, green: 0.129, blue: 0.141) // #202124 Google dark grey
        }
    }

    private var hintColor: Color {
        if isDark {
            return Color(red: 0.65, green: 0.68, blue: 0.74)
        } else {
            return Color(red: 0.098, green: 0.451, blue: 0.910).opacity(0.72) // Gboard accent blue
        }
    }

    private var headerBackground: Color {
        if isDark {
            return Color(red: 0.13, green: 0.14, blue: 0.16).opacity(0.92)
        } else {
            return Color(red: 0.957, green: 0.961, blue: 0.965).opacity(0.95) // #F4F5F6
        }
    }

    private var rowBackground: Color {
        if isDark {
            return Color(red: 0.18, green: 0.19, blue: 0.22).opacity(0.90)
        } else {
            return Color.white.opacity(0.96) // Crisp white card for high contrast
        }
    }

    private var dividerColor: Color {
        if isDark {
            return Color.white.opacity(0.12)
        } else {
            return Color(red: 0.878, green: 0.878, blue: 0.878) // #E0E0E0
        }
    }

    private var borderColor: Color {
        if isDark {
            return Color.white.opacity(0.18)
        } else {
            return Color(red: 0.82, green: 0.82, blue: 0.84)
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            // Header: Pinyin preedit strip + Status Badges + Settings shortcut
            HStack(spacing: 6) {
                Text(pinyin)
                    .font(.system(size: 13, weight: .semibold, design: .rounded))
                    .foregroundColor(accentColor)

                if prefs.isTraditional {
                    Text("繁")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(accentColor)
                        .padding(.horizontal, 4)
                        .padding(.vertical, 1)
                        .background(accentColor.opacity(0.18))
                        .clipShape(RoundedRectangle(cornerRadius: 3))
                }

                if pinyin.contains("?123") {
                    Text("符号")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(accentColor)
                        .padding(.horizontal, 4)
                        .padding(.vertical, 1)
                        .background(accentColor.opacity(0.18))
                        .clipShape(RoundedRectangle(cornerRadius: 3))
                }

                Spacer(minLength: 8)

                // Gear icon: Open preferences without interfering with input session
                Button {
                    DispatchQueue.main.async {
                        SettingsWindowController.shared.show()
                    }
                } label: {
                    Image(systemName: "gearshape.fill")
                        .font(.system(size: 11))
                        .foregroundColor(hintColor.opacity(0.85))
                        .padding(4)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .help("打开 Gboard 偏好设置")
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(headerBackground)

            Divider().background(dividerColor)

            // Candidate row
            ScrollViewReader { proxy in
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 0) {
                        ForEach(Array(candidates.enumerated()), id: \.offset) { i, cand in
                            let isSelected = (i == selectedIndex)
                            Button {
                                onSelect(i)
                            } label: {
                                HStack(alignment: .firstTextBaseline, spacing: 3) {
                                    Text("\(i + 1).")
                                        .font(.system(size: 11, weight: isSelected ? .semibold : .regular))
                                        .foregroundColor(isSelected ? accentColor : hintColor)
                                    Text(cand)
                                        .font(.system(size: 18, weight: isSelected ? .medium : .regular))
                                        .foregroundColor(isSelected ? accentColor : textColor)
                                }
                                .padding(.horizontal, 7)
                                .frame(minWidth: 36, minHeight: 36)
                                .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                            .background(
                                RoundedRectangle(cornerRadius: 6, style: .continuous)
                                    .fill(isSelected ? accentColor.opacity(isDark ? 0.22 : 0.12) : Color.clear)
                            )
                            .overlay(
                                RoundedRectangle(cornerRadius: 6, style: .continuous)
                                    .stroke(isSelected ? accentColor.opacity(isDark ? 0.45 : 0.3) : Color.clear, lineWidth: 1)
                            )
                            .id(i)

                            if i < candidates.count - 1 {
                                Divider()
                                    .frame(height: 22)
                                    .background(dividerColor)
                            }
                        }

                        Divider()
                            .frame(height: 22)
                            .background(dividerColor)

                        VStack(spacing: 0) {
                            pageButton(
                                systemName: "chevron.up",
                                enabled: canGoPrevious,
                                help: "上一页 (-)",
                                action: onPrevious
                            )
                            pageButton(
                                systemName: "chevron.down",
                                enabled: canGoNext,
                                help: "下一页 (=)",
                                action: onNext
                            )
                        }
                        .frame(width: 28, height: 36)
                    }
                    .padding(.horizontal, 4)
                }
                .onAppear {
                    proxy.scrollTo(selectedIndex, anchor: .center)
                }
            }
            .id(selectedIndex)
            .frame(height: 38)
            .background(rowBackground)
        }
        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(borderColor, lineWidth: 1)
        )
    }

    private func pageButton(
        systemName: String,
        enabled: Bool,
        help: String,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            Image(systemName: systemName)
                .font(.system(size: 9, weight: .semibold))
                .foregroundColor(enabled ? hintColor : hintColor.opacity(0.3))
                .frame(width: 28, height: 16)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .help(help)
    }
}

// MARK: - Window Controller

final class CandidateWindowController: NSObject {
    private var window: CandidatePanel?
    private var visualEffectView: NSVisualEffectView?
    private var hostingView: FirstMouseHostingView<CandidateView>?
    private var onSelect: ((Int) -> Void)?

    func update(
        candidates: [String],
        pinyin: String,
        selectedIndex: Int = 0,
        canGoPrevious: Bool,
        canGoNext: Bool,
        onSelect: @escaping (Int) -> Void,
        onPrevious: @escaping () -> Void,
        onNext: @escaping () -> Void
    ) {
        self.onSelect = onSelect
        let view = CandidateView(
            candidates: candidates,
            pinyin: pinyin,
            selectedIndex: selectedIndex,
            canGoPrevious: canGoPrevious,
            canGoNext: canGoNext,
            onSelect: { [weak self] idx in self?.onSelect?(idx) },
            onPrevious: onPrevious,
            onNext: onNext
        )
        let width = candidateWindowWidth(candidates: candidates, pinyin: pinyin)
        let height: CGFloat = 70

        if window == nil {
            let w = CandidatePanel(
                contentRect: NSRect(x: 0, y: 0, width: width, height: height),
                styleMask: [.nonactivatingPanel, .borderless],
                backing: .buffered,
                defer: false
            )
            w.level = NSWindow.Level(rawValue: Int(CGWindowLevelKey.popUpMenuWindow.rawValue))
            w.isOpaque = false
            w.backgroundColor = .clear
            w.hasShadow = true
            w.isFloatingPanel = true
            w.becomesKeyOnlyIfNeeded = false

            // NSVisualEffectView backdrop with rounded corners
            let vfx = NSVisualEffectView(frame: NSRect(x: 0, y: 0, width: width, height: height))
            vfx.material = .popover
            vfx.blendingMode = .behindWindow
            vfx.state = .active
            vfx.wantsLayer = true
            vfx.layer?.cornerRadius = 10
            vfx.layer?.masksToBounds = true
            w.contentView = vfx
            visualEffectView = vfx

            let hv = FirstMouseHostingView(rootView: view)
            hv.frame = vfx.bounds
            hv.autoresizingMask = [.width, .height]
            hv.wantsLayer = true
            hv.layer?.backgroundColor = NSColor.clear.cgColor
            vfx.addSubview(hv)

            window = w
            hostingView = hv
        } else {
            hostingView?.rootView = view
            window?.setContentSize(NSSize(width: width, height: height))
            visualEffectView?.frame = NSRect(x: 0, y: 0, width: width, height: height)
        }

        let isDark: Bool
        switch PreferencesManager.shared.themeMode {
        case .system:
            isDark = NSApp.effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        case .dark:
            isDark = true
        case .light:
            isDark = false
        }
        visualEffectView?.material = isDark ? .hudWindow : .popover
    }

    private func candidateWindowWidth(candidates: [String], pinyin: String) -> CGFloat {
        let candidateWidths = candidates.enumerated().map { index, candidate in
            let number = "\(index + 1)." as NSString
            let text = candidate as NSString
            let numberWidth = number.size(withAttributes: [
                .font: NSFont.systemFont(ofSize: 11)
            ]).width
            let textWidth = text.size(withAttributes: [
                .font: NSFont.systemFont(ofSize: 18)
            ]).width
            return max(36, numberWidth + 3 + textWidth) + 14
        }

        let rowWidth = candidateWidths.reduce(0, +)
            + CGFloat(candidates.count)
            + 36
        let pinyinWidth = (pinyin as NSString).size(withAttributes: [
            .font: NSFont.systemFont(ofSize: 13, weight: .semibold)
        ]).width + 65
        let contentWidth = ceil(max(130, max(rowWidth, pinyinWidth)))
        let screenWidth = NSScreen.main?.visibleFrame.width ?? contentWidth
        return min(contentWidth, screenWidth - 24)
    }

    func show(near rect: NSRect) {
        guard let w = window else { return }
        var origin = NSPoint(x: rect.minX, y: rect.minY - 76)
        if let screen = NSScreen.main {
            let sw = screen.visibleFrame
            if origin.x + w.frame.width > sw.maxX { origin.x = sw.maxX - w.frame.width }
            if origin.x < sw.minX { origin.x = sw.minX }
            if origin.y < sw.minY { origin.y = rect.maxY + 4 }
        }
        w.setFrameOrigin(origin)
        w.orderFront(nil)
    }

    func close() { window?.orderOut(nil) }
}
