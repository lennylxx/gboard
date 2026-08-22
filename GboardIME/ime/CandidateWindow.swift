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
            // Pinyin preedit strip
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
        if window == nil {
            let w = NSWindow(
                contentRect: NSRect(x: 0, y: 0, width: width, height: 68),
                styleMask: [.borderless],
                backing: .buffered,
                defer: false
            )
            w.level = NSWindow.Level(rawValue: Int(CGWindowLevelKey.popUpMenuWindow.rawValue))
            w.isOpaque = false
            w.backgroundColor = .clear
            w.hasShadow = false
            let hv = NSHostingView(rootView: view)
            hv.frame = NSRect(x: 0, y: 0, width: width, height: 68)
            hv.wantsLayer = true
            hv.layer?.backgroundColor = NSColor.clear.cgColor
            w.contentView = hv
            window = w
            hostingView = hv
        } else {
            hostingView?.rootView = view
            window?.setContentSize(NSSize(width: width, height: 68))
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

    func show(near rect: NSRect) {
        guard let w = window else { return }
        // Position just below the cursor rect
        var origin = NSPoint(x: rect.minX, y: rect.minY - 73)
        // Keep on screen
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
