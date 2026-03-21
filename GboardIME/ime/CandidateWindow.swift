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
    let onSelect: (Int) -> Void

    // Gboard light theme colors
    private let bgColor     = Color(red: 0.945, green: 0.953, blue: 0.961) // #F1F3F4
    private let keyColor    = Color.white
    private let labelColor  = Color(red: 0.259, green: 0.259, blue: 0.259) // #424242
    private let accentColor = Color(red: 0.098, green: 0.451, blue: 0.910) // #1873E8
    private let divColor    = Color(red: 0.878, green: 0.878, blue: 0.878) // #E0E0E0
    private let hintColor   = Color(red: 0.098, green: 0.451, blue: 0.910).opacity(0.7) // accent blue

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
                    }
                    .padding(.horizontal, 4)
                }
                .onAppear {
                    proxy.scrollTo(selectedIndex, anchor: .center)
                }
                .onChange(of: selectedIndex) { idx in
                    withAnimation(.easeOut(duration: 0.15)) {
                        proxy.scrollTo(idx, anchor: .center)
                    }
                }
            }
            .frame(height: 38)
            .background(keyColor)
        }
        .background(bgColor)
        .cornerRadius(10)
        .shadow(color: .black.opacity(0.18), radius: 8, x: 0, y: 4)
    }
}

// MARK: - Window controller

class CandidateWindowController: NSObject {
    private var window: NSWindow?
    private var hostingView: NSHostingView<CandidateView>?
    private var onSelect: ((Int) -> Void)?

    func update(candidates: [String], pinyin: String, selectedIndex: Int = 0, onSelect: @escaping (Int) -> Void) {
        self.onSelect = onSelect
        let view = CandidateView(candidates: candidates, pinyin: pinyin, selectedIndex: selectedIndex) { [weak self] idx in
            self?.onSelect?(idx)
        }
        let minWidth: CGFloat = max(300, CGFloat(candidates.count) * 56 + 24)
        if window == nil {
            let w = NSWindow(
                contentRect: NSRect(x: 0, y: 0, width: minWidth, height: 68),
                styleMask: [.borderless],
                backing: .buffered,
                defer: false
            )
            w.level = NSWindow.Level(rawValue: Int(CGWindowLevelKey.popUpMenuWindow.rawValue))
            w.isOpaque = false
            w.backgroundColor = .clear
            w.hasShadow = false  // we draw our own
            let hv = NSHostingView(rootView: view)
            hv.frame = NSRect(x: 0, y: 0, width: minWidth, height: 68)
            w.contentView = hv
            window = w
            hostingView = hv
        } else {
            hostingView?.rootView = view
            window?.setContentSize(NSSize(width: minWidth, height: 68))
        }
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
