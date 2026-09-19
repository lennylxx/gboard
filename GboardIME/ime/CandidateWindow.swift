import Cocoa
import SwiftUI

// MARK: - AppKit First-Mouse Hosting View
// Allows single-click interaction on background/floating windows without needing a preliminary click to activate.

final class FirstMouseHostingView<Content: View>: NSHostingView<Content> {
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool {
        return true
    }

    // Allow mouse events to pass through to SwiftUI views even when the window isn't key
    override func hitTest(_ point: NSPoint) -> NSView? {
        return super.hitTest(point)
    }
}

// MARK: - Modern Candidate View (all backgrounds transparent for frosted glass)

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

    private var accentColor: Color {
        if isDark {
            return Color(red: 0.54, green: 0.71, blue: 0.97) // Google Blue 200
        } else {
            return Color(red: 0.10, green: 0.45, blue: 0.91) // Google Blue 600
        }
    }

    private var textColor: Color {
        if isDark {
            return Color(red: 0.95, green: 0.96, blue: 0.98)
        } else {
            return Color(red: 0.12, green: 0.13, blue: 0.15)
        }
    }

    private var hintColor: Color {
        if isDark {
            return Color(red: 0.65, green: 0.68, blue: 0.74)
        } else {
            return Color(red: 0.42, green: 0.46, blue: 0.52)
        }
    }

    private var dividerColor: Color {
        if isDark {
            return Color.white.opacity(0.12)
        } else {
            return Color.black.opacity(0.10)
        }
    }

    private var borderColor: Color {
        if isDark {
            return Color.white.opacity(0.18)
        } else {
            return Color.black.opacity(0.15)
        }
    }

    // Light semi-transparent tint over the blur
    private var headerTint: Color {
        if isDark {
            return Color.white.opacity(0.05)
        } else {
            return Color.black.opacity(0.04)
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            // Header: Pinyin preedit strip + Status Badges + Settings shortcut
            HStack(spacing: 6) {
                Circle()
                    .fill(accentColor)
                    .frame(width: 6, height: 6)

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

                // Gear icon — dispatches async to escape IMKit event chain
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
            .background(headerTint)

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
                                        .font(.system(size: 17, weight: isSelected ? .medium : .regular))
                                        .foregroundColor(isSelected ? (isDark ? accentColor : Color(red: 0.06, green: 0.36, blue: 0.82)) : textColor)
                                }
                                .padding(.horizontal, 7)
                                .frame(minWidth: 34, minHeight: 34)
                                .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                            .background(
                                RoundedRectangle(cornerRadius: 6, style: .continuous)
                                    .fill(isSelected ? accentColor.opacity(isDark ? 0.25 : 0.15) : Color.clear)
                            )
                            .overlay(
                                RoundedRectangle(cornerRadius: 6, style: .continuous)
                                    .stroke(isSelected ? accentColor.opacity(isDark ? 0.5 : 0.35) : Color.clear, lineWidth: 1)
                            )
                            .id(i)

                            if i < candidates.count - 1 {
                                Divider()
                                    .frame(height: 20)
                                    .background(dividerColor)
                            }
                        }

                        Divider()
                            .frame(height: 20)
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
                        .frame(width: 28, height: 34)
                    }
                    .padding(.horizontal, 4)
                }
                .onAppear {
                    proxy.scrollTo(selectedIndex, anchor: .center)
                }
            }
            .id(selectedIndex)
            .frame(height: 36)
        }
        // ALL backgrounds transparent — the real blur is at the AppKit NSVisualEffectView layer
        .background(Color.clear)
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
                .frame(width: 28, height: 14)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .help(help)
    }
}

// MARK: - Window controller (NSVisualEffectView as contentView for real frosted glass)

final class CandidateWindowController: NSObject {
    private var window: NSWindow?
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
        let height: CGFloat = 68

        if window == nil {
            let w = NSWindow(
                contentRect: NSRect(x: 0, y: 0, width: width, height: height),
                styleMask: [.borderless],
                backing: .buffered,
                defer: false
            )
            w.level = NSWindow.Level(rawValue: Int(CGWindowLevelKey.popUpMenuWindow.rawValue))
            w.isOpaque = false
            w.backgroundColor = .clear
            w.hasShadow = true
            w.ignoresMouseEvents = false

            // 1. NSVisualEffectView as the direct contentView — this IS the frosted glass
            let vfx = NSVisualEffectView(frame: NSRect(x: 0, y: 0, width: width, height: height))
            vfx.material = .popover
            vfx.blendingMode = .behindWindow
            vfx.state = .active
            vfx.wantsLayer = true
            vfx.layer?.cornerRadius = 10
            vfx.layer?.masksToBounds = true
            w.contentView = vfx
            visualEffectView = vfx

            // 2. NSHostingView layered ON TOP of the blur with fully transparent backgrounds
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

        // Update material based on dark/light theme preference
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
                .font: NSFont.systemFont(ofSize: 17)
            ]).width
            return max(34, numberWidth + 3 + textWidth) + 12
        }

        let rowWidth = candidateWidths.reduce(0, +)
            + CGFloat(candidates.count)
            + 36
        let pinyinWidth = (pinyin as NSString).size(withAttributes: [
            .font: NSFont.systemFont(ofSize: 13, weight: .semibold)
        ]).width + 70
        let contentWidth = ceil(max(130, max(rowWidth, pinyinWidth)))
        let screenWidth = NSScreen.main?.visibleFrame.width ?? contentWidth
        return min(contentWidth, screenWidth - 24)
    }

    func show(near rect: NSRect) {
        guard let w = window else { return }
        var origin = NSPoint(x: rect.minX, y: rect.minY - 74)
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
