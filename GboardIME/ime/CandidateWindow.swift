import Cocoa
import SwiftUI

// MARK: - Non-activating panel & first-mouse hosting view

final class CandidatePanel: NSPanel {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

/// Frosted background whose material follows the panel's effective
/// appearance, including live system appearance changes.
final class ThemedEffectView: NSVisualEffectView {
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        updateMaterial()
    }

    func updateMaterial() {
        let dark = effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        material = dark ? .hudWindow : .popover
    }
}

final class FirstMouseHostingView<Content: View>: NSHostingView<Content> {
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool {
        true
    }
}

// MARK: - SwiftUI candidate panel (Gboard Google Blue light & dark themes)

struct CandidateView: View {
    let candidates: [String]
    let pinyin: PinyinReading
    let selectedIndex: Int
    /// Shift+Delete can remove the highlighted candidate when it is a
    /// learned word; a hint shows beside the settings button. The pinyin
    /// strip always reserves its width, so the candidate row never moves.
    var selectedRemovable = false

    static let removeHint = "⇧⌫ 删除学习词"
    static let removeHintFont = NSFont.systemFont(ofSize: 11)
    let canGoPrevious: Bool
    let canGoNext: Bool
    let onSelect: (Int) -> Void
    let onPrevious: () -> Void
    let onNext: () -> Void
    var onOpenSettings: (() -> Void)? = nil
    /// Hosted in Liquid Glass: content sits directly on the glass with
    /// system label colors and an accent selection, per Apple's HIG.
    var glassBacked = false
    /// Captured with the window size so a live preference change never
    /// renders one layout inside a window sized for the other.
    var layout: CandidateLayout = .horizontal

    @Environment(\.colorScheme) private var systemColorScheme
    @ObservedObject var preferences: Preferences = .shared

    private var isDark: Bool {
        switch preferences.themeMode {
        case .system: return systemColorScheme == .dark
        case .light: return false
        case .dark: return true
        }
    }

    // Gboard official Google Blue light (`style_sheet_google_blue_light`) &
    // dark (`style_sheet_google_blue_dark`) theme colors.
    // Each area draws a single layer so the preference is the opacity
    // actually seen over the glass or frosted background.
    private var bgColor: Color {
        if glassBacked { return .clear }
        return (isDark
            ? Color(red: 0.161, green: 0.180, blue: 0.200) // #292E33
            : Color(red: 0.945, green: 0.953, blue: 0.957)) // #F1F3F4
            .opacity(preferences.candidateWindowOpacity)
    }

    private var keyColor: Color {
        if glassBacked { return .clear }
        return (isDark
            ? Color(red: 0.212, green: 0.231, blue: 0.251) // #363B40
            : Color.white)
            .opacity(preferences.candidateWindowOpacity)
    }

    private var labelColor: Color {
        if glassBacked { return Color(nsColor: .labelColor) }
        return isDark
            ? Color.white.opacity(0.8) // color_state_label_candidate (80% white)
            : Color(red: 0.259, green: 0.259, blue: 0.259) // #424242
    }

    private var selectedLabelColor: Color {
        if glassBacked { return Color(nsColor: .alternateSelectedControlTextColor) }
        return isDark
            ? Color.white // color_state_label_candidate_selected
            : Color(red: 0.259, green: 0.259, blue: 0.259) // #424242
    }

    private var accentColor: Color {
        if glassBacked { return Color(nsColor: .controlAccentColor) }
        return isDark
            ? Color(red: 0.369, green: 0.592, blue: 0.965) // #5E97F6
            : Color(red: 0.102, green: 0.451, blue: 0.910) // #1A73E8
    }

    private var divColor: Color {
        isDark
            ? Color.white.opacity(0.1) // color_keyboard_separator (10% white)
            : Color(red: 0.878, green: 0.878, blue: 0.878) // #E0E0E0
    }

    private var hintColor: Color {
        glassBacked ? Color(nsColor: .secondaryLabelColor) : accentColor.opacity(0.7)
    }

    private func indexColor(selected: Bool) -> Color {
        glassBacked && selected ? selectedLabelColor.opacity(0.75) : hintColor
    }

    private var cornerRadius: CGFloat {
        glassBacked ? CandidateWindowController.glassCornerRadius : 8
    }

    /// Concentric with the panel corner across the candidate row inset.
    private var selectionCornerRadius: CGFloat {
        glassBacked ? cornerRadius - 4 : 6
    }

    private var selectionFill: Color {
        glassBacked ? accentColor : accentColor.opacity(0.08)
    }

    var body: some View {
        VStack(spacing: 0) {
            // Pinyin preedit strip (absent for next-word predictions)
            if !pinyin.isEmpty {
                HStack(spacing: 6) {
                    Text(glassBacked
                        ? pinyin.attributedText(
                            accent: labelColor,
                            correction: Color(nsColor: .systemOrange),
                            typo: Color(nsColor: .secondaryLabelColor))
                        : pinyin.attributedText(accent: accentColor))
                        .layoutPriority(1)

                    if preferences.isTraditional {
                        Text("繁")
                            .font(.system(size: 9, weight: .regular))
                            .foregroundColor(accentColor)
                            .padding(.horizontal, 4)
                            .padding(.vertical, 1)
                            .background(accentColor.opacity(0.12))
                            .clipShape(RoundedRectangle(cornerRadius: 3))
                    }

                    if pinyin.displayText.hasPrefix(SymbolPanel.placeholder) {
                        Text("符号")
                            .font(.system(size: 9, weight: .regular))
                            .foregroundColor(accentColor)
                            .padding(.horizontal, 4)
                            .padding(.vertical, 1)
                            .background(accentColor.opacity(0.12))
                            .clipShape(RoundedRectangle(cornerRadius: 3))
                    }

                    Spacer()

                    if selectedRemovable {
                        Text(Self.removeHint)
                            .font(Font(Self.removeHintFont))
                            .foregroundColor(hintColor)
                            .fixedSize()
                    }

                    if let onOpenSettings = onOpenSettings {
                        Button {
                            onOpenSettings()
                        } label: {
                            Image(systemName: "gearshape")
                                .font(.system(size: 11, weight: .regular))
                                .foregroundColor(hintColor)
                                .frame(width: 16, height: 16)
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .background(bgColor)

                if glassBacked {
                    Divider().padding(.horizontal, 8)
                } else {
                    Divider().background(divColor).background(bgColor)
                }
            }

            if layout == .vertical {
                candidateList
            } else {
                candidateRow
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                .strokeBorder(glassBacked ? Color.clear : divColor, lineWidth: 1)
        )
    }

    private var candidateRow: some View {
        ScrollViewReader { proxy in
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 0) {
                    ForEach(Array(candidates.enumerated()), id: \.offset) { i, cand in
                        Button {
                            onSelect(i)
                        } label: {
                            HStack(alignment: .firstTextBaseline, spacing: 2) {
                                Text("\(i + 1).")
                                    .font(.system(size: 11))
                                    .foregroundColor(indexColor(selected: i == selectedIndex))
                                Text(cand)
                                    .font(.system(size: 18, weight: .regular))
                                    .foregroundColor(i == selectedIndex ? selectedLabelColor : labelColor)
                            }
                            // On glass the pill is 4pt inset from the 38pt row,
                            // concentric with the panel; the inset and the gap
                            // after it stay clickable.
                            .frame(minWidth: 36, minHeight: glassBacked ? 30 : 36)
                            .padding(.horizontal, 4)
                            .padding(.vertical, glassBacked ? 0 : 2)
                            .background(
                                RoundedRectangle(cornerRadius: selectionCornerRadius,
                                                 style: glassBacked ? .continuous : .circular)
                                    .fill(i == selectedIndex ? selectionFill : Color.clear)
                            )
                            .padding(.vertical, glassBacked ? 4 : 0)
                            .padding(.trailing, glassBacked
                                ? CandidateWindowController.glassCandidateGap : 0)
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .id(i)

                        if !glassBacked && i < candidates.count - 1 {
                            Divider()
                                .frame(height: 28)
                                .background(divColor)
                        }
                    }

                    if !glassBacked {
                        Divider()
                            .frame(height: 28)
                            .background(divColor)
                    }

                    VStack(spacing: 0) {
                        pageButton(
                            systemName: "chevron.up",
                            enabled: canGoPrevious,
                            action: onPrevious
                        )
                        pageButton(
                            systemName: "chevron.down",
                            enabled: canGoNext,
                            action: onNext
                        )
                    }
                    .frame(width: 30, height: 38)
                }
                .padding(.horizontal, 4)
            }
            .onAppear {
                proxy.scrollTo(selectedIndex, anchor: .center)
            }
        }
        .id(selectedIndex)
        .frame(height: 38)
        .background(keyColor)
    }

    /// Vertical layout: one candidate per row, the pager in a footer.
    private var candidateList: some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(candidates.enumerated()), id: \.offset) { i, cand in
                Button {
                    onSelect(i)
                } label: {
                    HStack(alignment: .firstTextBaseline, spacing: Self.listIndexGap) {
                        Text("\(i + 1).")
                            .font(.system(size: 11))
                            .foregroundColor(indexColor(selected: i == selectedIndex))
                            .frame(width: Self.listIndexWidth, alignment: .trailing)
                        Text(cand)
                            .font(.system(size: 18, weight: .regular))
                            .foregroundColor(i == selectedIndex ? selectedLabelColor : labelColor)
                            .lineLimit(1)
                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, Self.listRowPadding)
                    .frame(maxWidth: .infinity, minHeight: Self.listRowHeight,
                           maxHeight: Self.listRowHeight, alignment: .leading)
                    .background(
                        RoundedRectangle(cornerRadius: selectionCornerRadius,
                                         style: glassBacked ? .continuous : .circular)
                            .fill(i == selectedIndex ? selectionFill : Color.clear)
                    )
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }

            HStack(spacing: 0) {
                Spacer()
                pageButton(
                    systemName: "chevron.up",
                    enabled: canGoPrevious,
                    action: onPrevious
                )
                pageButton(
                    systemName: "chevron.down",
                    enabled: canGoNext,
                    action: onNext
                )
            }
            .frame(height: Self.listFooterHeight)
        }
        .padding(Self.listInset)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(keyColor)
    }

    static let listRowHeight: CGFloat = 30
    static let listFooterHeight: CGFloat = 18
    static let listInset: CGFloat = 4
    static let listRowPadding: CGFloat = 8
    static let listIndexWidth: CGFloat = 14
    static let listIndexGap: CGFloat = 6

    private func pageButton(
        systemName: String,
        enabled: Bool,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            Image(systemName: systemName)
                .font(.system(size: 9, weight: .semibold))
                .foregroundColor(enabled ? hintColor
                    : glassBacked ? Color(nsColor: .tertiaryLabelColor)
                    : Color.gray.opacity(0.35))
                .frame(width: 30, height: 14)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
    }
}

// MARK: - Window controller

class CandidateWindowController: NSObject {
    private(set) var window: CandidatePanel?
    /// Liquid Glass (`NSGlassEffectView`, macOS 26+) when the running
    /// system provides it; looked up at runtime so older SDKs still build.
    static let systemGlassEffectClass: NSView.Type? = {
        guard let cls = NSClassFromString("NSGlassEffectView") as? NSView.Type,
              cls.instancesRespond(to: NSSelectorFromString("setContentView:")),
              cls.instancesRespond(to: NSSelectorFromString("setCornerRadius:"))
        else { return nil }
        return cls
    }()
    static let glassCornerRadius: CGFloat = 12
    /// Space after each glass candidate, in place of the fallback's 1pt divider.
    static let glassCandidateGap: CGFloat = 2

    private let glassEffectClass: NSView.Type?
    private(set) var backgroundView: NSView?
    var visualEffectView: ThemedEffectView? { backgroundView as? ThemedEffectView }
    private var hostingView: FirstMouseHostingView<CandidateView>?
    private var onSelect: ((Int) -> Void)?
    private let preferences: Preferences
    private var preferencesObserver: NSObjectProtocol?

    init(
        preferences: Preferences = .shared,
        glassEffectClass: NSView.Type? = CandidateWindowController.systemGlassEffectClass
    ) {
        self.preferences = preferences
        self.glassEffectClass = glassEffectClass
        super.init()
        preferencesObserver = NotificationCenter.default.addObserver(
            forName: Preferences.didChangeNotification,
            object: preferences, queue: nil
        ) { [weak self] _ in
            self?.applyTheme()
        }
    }

    deinit {
        if let preferencesObserver {
            NotificationCenter.default.removeObserver(preferencesObserver)
        }
    }

    /// A fixed theme pins the panel's appearance so the system-adaptive
    /// material and SwiftUI colors agree; `nil` follows the system.
    static func appearance(for mode: ThemeMode) -> NSAppearance? {
        switch mode {
        case .system: return nil
        case .light: return NSAppearance(named: .aqua)
        case .dark: return NSAppearance(named: .darkAqua)
        }
    }

    private func applyTheme() {
        let appearance = Self.appearance(for: preferences.themeMode)
        guard window?.appearance?.name != appearance?.name else { return }
        window?.appearance = appearance
        visualEffectView?.updateMaterial()
    }

    func update(
        candidates: [String],
        pinyin: PinyinReading,
        selectedIndex: Int = 0,
        selectedRemovable: Bool = false,
        canGoPrevious: Bool,
        canGoNext: Bool,
        onSelect: @escaping (Int) -> Void,
        onPrevious: @escaping () -> Void,
        onNext: @escaping () -> Void,
        onOpenSettings: (() -> Void)? = nil
    ) {
        self.onSelect = onSelect
        let layout = preferences.candidateLayout
        let view = CandidateView(
            candidates: candidates,
            pinyin: pinyin,
            selectedIndex: selectedIndex,
            selectedRemovable: selectedRemovable,
            canGoPrevious: canGoPrevious,
            canGoNext: canGoNext,
            onSelect: { [weak self] idx in self?.onSelect?(idx) },
            onPrevious: onPrevious,
            onNext: onNext,
            onOpenSettings: onOpenSettings,
            glassBacked: glassEffectClass != nil,
            layout: layout,
            preferences: preferences
        )
        let size = Self.windowSize(
            candidates: candidates, pinyin: pinyin.displayText,
            removeHint: !pinyin.isEmpty, layout: layout,
            glassBacked: glassEffectClass != nil)
        let width = size.width
        let height = size.height
        if window == nil {
            let w = CandidatePanel(
                contentRect: NSRect(x: 0, y: 0, width: width, height: height),
                styleMask: [.nonactivatingPanel, .borderless],
                backing: .buffered,
                defer: false
            )
            // isFloatingPanel would reset the level to .floating (3), below
            // high-level clients such as Spotlight.
            w.level = .popUpMenu
            w.isOpaque = false
            w.backgroundColor = .clear
            w.hasShadow = true
            w.becomesKeyOnlyIfNeeded = false

            let frame = NSRect(x: 0, y: 0, width: width, height: height)
            let hv = FirstMouseHostingView(rootView: view)
            hv.frame = frame
            hv.autoresizingMask = [.width, .height]
            // The glass view pins its content with constraints; keep SwiftUI's
            // ideal size from overriding the fixed panel size.
            hv.sizingOptions = []
            hv.wantsLayer = true
            hv.layer?.backgroundColor = NSColor.clear.cgColor

            let background: NSView
            if let glassEffectClass {
                background = glassEffectClass.init(frame: frame)
                background.setValue(Self.glassCornerRadius, forKey: "cornerRadius")
                background.setValue(hv, forKey: "contentView")
            } else {
                let vfx = ThemedEffectView(frame: frame)
                vfx.blendingMode = .behindWindow
                vfx.state = .active
                vfx.wantsLayer = true
                vfx.layer?.cornerRadius = 8
                vfx.layer?.masksToBounds = true
                vfx.addSubview(hv)
                background = vfx
            }
            w.contentView = background
            backgroundView = background

            window = w
            hostingView = hv
            w.appearance = Self.appearance(for: preferences.themeMode)
            visualEffectView?.updateMaterial()
        } else {
            hostingView?.rootView = view
            window?.setContentSize(NSSize(width: width, height: height))
            backgroundView?.frame = NSRect(x: 0, y: 0, width: width, height: height)
        }
    }

    /// Room the pinyin strip reserves for the remove hint, so moving the
    /// selection on or off a learned word never resizes the window: the
    /// Spacer minimum (8), two HStack gaps (12), the gear (16), and slack
    /// for the arrow glyphs, which draw wider than they measure.
    static let removeHintChrome: CGFloat = 52

    /// Height of the pinyin strip (16pt line + 12pt padding) and its
    /// divider. The horizontal row's 2pt slack absorbs the divider, so
    /// only the exactly sized vertical list adds it.
    static let pinyinStripHeight: CGFloat = 29

    static func windowSize(candidates: [String], pinyin: String,
                           removeHint: Bool = false,
                           layout: CandidateLayout = .horizontal,
                           glassBacked: Bool = false,
                           screenWidth: CGFloat? = NSScreen.main?.visibleFrame.width
    ) -> NSSize {
        let numberFont = NSFont.systemFont(ofSize: 11)
        let textFont = NSFont.systemFont(ofSize: 18)
        func width(_ s: String, _ font: NSFont) -> CGFloat {
            // SwiftUI lays each Text out at its pixel-rounded width; matching
            // that keeps the row from overflowing and scrolling by a few points.
            ceil((s as NSString).size(withAttributes: [.font: font]).width)
        }

        let candidatesWidth: CGFloat
        let candidatesHeight: CGFloat
        switch layout {
        case .horizontal:
            candidatesWidth = candidates.enumerated().map { index, candidate in
                max(36, width("\(index + 1).", numberFont) + 2
                    + width(candidate, textFont)) + 8
            }.reduce(0, +)
                + CGFloat(candidates.count) * (glassBacked ? glassCandidateGap : 1)
                + 38
            candidatesHeight = pinyin.isEmpty ? 40 : 68
        case .vertical:
            let widest = candidates.map { width($0, textFont) }.max() ?? 0
            candidatesWidth = widest + CandidateView.listIndexWidth
                + CandidateView.listIndexGap + CandidateView.listRowPadding * 2
                + CandidateView.listInset * 2
            candidatesHeight = CGFloat(candidates.count) * CandidateView.listRowHeight
                + CandidateView.listFooterHeight + CandidateView.listInset * 2
        }

        let pinyinWidth = (pinyin as NSString).size(withAttributes: [
            .font: PinyinReading.displayNSFont
        ]).width + 24
            + (removeHint
                ? width(CandidateView.removeHint, CandidateView.removeHintFont)
                    + removeHintChrome
                : 0)
        let contentWidth = ceil(max(120, max(candidatesWidth, pinyinWidth)))
        let height = layout == .horizontal || pinyin.isEmpty
            ? candidatesHeight : candidatesHeight + pinyinStripHeight
        return NSSize(width: min(contentWidth, (screenWidth ?? contentWidth) - 24),
                      height: height)
    }

    /// Predictions have no marked text, so they anchor at the caret; the
    /// marked-text rect is the fallback when the caret rect is unavailable.
    /// Some clients (e.g. iTerm2) report a caret far from the text; when the
    /// caret is on the same screen but several lines away from the previous
    /// window anchor, reuse that anchor. A caret on another screen is a real
    /// move and is trusted.
    static func anchorRect(isPrediction: Bool, caretRect: () -> NSRect?,
                           markedTextRect: () -> NSRect,
                           previousAnchor: NSRect? = nil,
                           screenFrames: [NSRect] = NSScreen.screens.map(\.frame)
    ) -> NSRect {
        guard isPrediction else { return markedTextRect() }
        if let caret = caretRect(), caret != .zero {
            guard let anchor = previousAnchor else { return caret }
            let caretScreen = screenIndex(for: caret, in: screenFrames)
            let anchorScreen = screenIndex(for: anchor, in: screenFrames)
            if caretScreen != anchorScreen { return caret }
            let line = max(anchor.height, caret.height, 16)
            return abs(caret.minY - anchor.minY) <= line * 3 ? caret : anchor
        }
        return previousAnchor ?? markedTextRect()
    }

    /// Index of the screen containing `rect`'s origin, else the nearest one.
    static func screenIndex(for rect: NSRect, in frames: [NSRect]) -> Int? {
        let p = NSPoint(x: rect.minX, y: rect.minY)
        if let i = frames.firstIndex(where: {
            p.x >= $0.minX && p.x < $0.maxX && p.y >= $0.minY && p.y < $0.maxY
        }) {
            return i
        }
        func distance(_ f: NSRect) -> CGFloat {
            let dx = max(f.minX - p.x, 0, p.x - f.maxX)
            let dy = max(f.minY - p.y, 0, p.y - f.maxY)
            return dx * dx + dy * dy
        }
        return frames.indices.min { distance(frames[$0]) < distance(frames[$1]) }
    }

    /// Places a window of `size` below `rect`, or above it when there is no
    /// room, clamped to `visible` (the anchor screen's visible frame).
    static func windowOrigin(size: NSSize, below rect: NSRect,
                             visible: NSRect?) -> NSPoint {
        var origin = NSPoint(x: rect.minX, y: rect.minY - size.height - 5)
        guard let sw = visible else { return origin }
        if origin.x + size.width > sw.maxX { origin.x = sw.maxX - size.width }
        if origin.x < sw.minX { origin.x = sw.minX }
        if origin.y < sw.minY { origin.y = rect.maxY + 4 }
        origin.y = max(sw.minY, min(origin.y, sw.maxY - size.height))
        return origin
    }

    /// IMKTextInput's documented convention: candidate windows sit one
    /// level above the client window, never below pop-up menus or above
    /// the window server's maximum level.
    static func windowLevel(clientLevel: Int) -> NSWindow.Level {
        let top = Int(CGWindowLevelForKey(.maximumWindow))
        return NSWindow.Level(rawValue: min(top, max(
            NSWindow.Level.popUpMenu.rawValue, clientLevel + 1)))
    }

    func show(near rect: NSRect, clientLevel: Int = 0) {
        guard let w = window else { return }
        w.level = Self.windowLevel(clientLevel: clientLevel)
        let screens = NSScreen.screens
        let screen = Self.screenIndex(for: rect, in: screens.map(\.frame))
            .map { screens[$0] } ?? NSScreen.main
        if let visible = screen?.visibleFrame,
           w.frame.width > visible.width - 24 {
            let size = NSSize(width: visible.width - 24,
                              height: w.frame.height)
            w.setContentSize(size)
            hostingView?.frame = NSRect(origin: .zero, size: size)
        }
        w.setFrameOrigin(Self.windowOrigin(size: w.frame.size, below: rect,
                                           visible: screen?.visibleFrame))
        w.orderFrontRegardless()
    }

    func close() { window?.orderOut(nil) }
}
