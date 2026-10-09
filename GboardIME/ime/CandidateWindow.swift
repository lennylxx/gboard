import Cocoa
import SwiftUI

// MARK: - Helper to load bundled PNGs as NSImage

private func bundledImage(_ name: String) -> NSImage? {
    guard let path = Bundle.main.path(forResource: name, ofType: "png") else { return nil }
    return NSImage(contentsOfFile: path)
}

// MARK: - SwiftUI candidate panel (Gboard-inspired design)

struct CandidateView: View {
    let candidates: [String]
    let pinyin: String
    let selectedIndex: Int
    let canGoPrevious: Bool
    let canGoNext: Bool
    let onSelect: (Int) -> Void
    let onPrevious: () -> Void
    let onNext: () -> Void

    // Gboard light theme colors
    private let bgColor     = Color(red: 0.945, green: 0.953, blue: 0.961) // #F1F3F4
    private let keyColor    = Color.white
    private let labelColor  = Color(red: 0.259, green: 0.259, blue: 0.259) // #424242
    private let accentColor = Color(red: 0.098, green: 0.451, blue: 0.910) // #1873E8
    private let divColor    = Color(red: 0.878, green: 0.878, blue: 0.878) // #E0E0E0
    private let hintColor   = Color(red: 0.098, green: 0.451, blue: 0.910).opacity(0.7) // accent blue
    private let cornerRadius: CGFloat = 8

    var body: some View {
        VStack(spacing: 0) {
            // Pinyin preedit strip (absent for next-word predictions)
            if !pinyin.isEmpty {
                HStack(spacing: 6) {
                    Text(pinyin)
                        .font(.system(size: 13, weight: .medium))
                        .foregroundColor(accentColor)
                    Spacer()
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .background(bgColor)

                Divider().background(divColor)
            }

            // Candidate row
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
                                        .foregroundColor(hintColor)
                                    Text(cand)
                                        .font(.system(size: 18, weight: .regular))
                                        .foregroundColor(labelColor)
                                }
                                .frame(minWidth: 36, minHeight: 36)
                                .padding(.horizontal, 4)
                                .padding(.vertical, 2)
                            }
                            .buttonStyle(.plain)
                            .background(
                                RoundedRectangle(cornerRadius: 6)
                                    .fill(i == selectedIndex ? accentColor.opacity(0.08) : Color.clear)
                            )
                            .id(i)

                            if i < candidates.count - 1 {
                                Divider()
                                    .frame(height: 28)
                                    .background(divColor)
                            }
                        }

                        Divider()
                            .frame(height: 28)
                            .background(divColor)

                        VStack(spacing: 0) {
                            pageButton(
                                systemName: "chevron.up",
                                enabled: canGoPrevious,
                                help: "Previous candidate page (-)",
                                action: onPrevious
                            )
                            pageButton(
                                systemName: "chevron.down",
                                enabled: canGoNext,
                                help: "Next candidate page (=)",
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
        .background(bgColor)
        .clipShape(RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                .strokeBorder(divColor, lineWidth: 1)
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
                .foregroundColor(enabled ? hintColor : Color.gray.opacity(0.35))
                .frame(width: 30, height: 14)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .help(help)
    }
}

// MARK: - Window controller

class CandidateWindowController: NSObject {
    private var window: NSWindow?
    private var hostingView: NSHostingView<CandidateView>?
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
        let height: CGFloat = pinyin.isEmpty ? 40 : 68
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
            w.hasShadow = false
            let hv = NSHostingView(rootView: view)
            hv.frame = NSRect(x: 0, y: 0, width: width, height: height)
            hv.wantsLayer = true
            hv.layer?.backgroundColor = NSColor.clear.cgColor
            w.contentView = hv
            window = w
            hostingView = hv
        } else {
            hostingView?.rootView = view
            window?.setContentSize(NSSize(width: width, height: height))
        }
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
            return max(36, numberWidth + 2 + textWidth) + 8
        }

        let rowWidth = candidateWidths.reduce(0, +)
            + CGFloat(candidates.count)
            + 38
        let pinyinWidth = (pinyin as NSString).size(withAttributes: [
            .font: NSFont.systemFont(ofSize: 13, weight: .medium)
        ]).width + 24
        let contentWidth = ceil(max(120, max(rowWidth, pinyinWidth)))
        let screenWidth = NSScreen.main?.visibleFrame.width ?? contentWidth
        return min(contentWidth, screenWidth - 24)
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

    func show(near rect: NSRect) {
        guard let w = window else { return }
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
        w.orderFront(nil)
    }

    func close() { window?.orderOut(nil) }
}
