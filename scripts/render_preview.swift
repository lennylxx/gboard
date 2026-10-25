import Cocoa

// Renders the README preview image: the real candidate window in light and
// dark appearances over one shared colorful backdrop, captured with
// screencapture.

private let candidates = ["中文输入法", "中文", "中", "种", "重", "众", "钟", "终", "肿"]
// Typed `zhongwnshuurufa`: the engine's correction inserts `e` (orange)
// and drops the extra `u` (struck out).
private let pinyin = PinyinReading(spans: [
    .init(text: "zhong'w", isCorrected: false),
    .init(text: "e", isCorrected: true),
    .init(text: "n'shu", isCorrected: false),
    .init(text: "u", isCorrected: false, isTypo: true),
    .init(text: "'ru'fa", isCorrected: false),
])
private let sectionSize = NSSize(width: 640, height: 150)

private final class BackdropView: NSView {
    override func draw(_ dirtyRect: NSRect) {
        NSColor(red: 0.96, green: 0.95, blue: 0.98, alpha: 1).setFill()
        bounds.fill()

        // One continuous wash behind both themes, so only the theme differs.
        let blobs: [(CGFloat, CGFloat, CGFloat, NSColor)] = [
            (0.10, 0.80, 170, .systemBlue), (0.40, 0.92, 150, .systemYellow),
            (0.72, 0.78, 160, .systemPink), (0.95, 0.90, 150, .systemGreen),
            (0.05, 0.25, 160, .systemPurple), (0.35, 0.40, 150, .systemTeal),
            (0.65, 0.15, 170, .systemOrange), (0.92, 0.35, 160, .systemIndigo),
        ]
        for (fx, fy, radius, color) in blobs {
            let center = NSPoint(x: bounds.width * fx, y: bounds.height * fy)
            let gradient = NSGradient(colors: [
                color.withAlphaComponent(0.8), color.withAlphaComponent(0),
            ])
            gradient?.draw(fromCenter: center, radius: 0,
                           toCenter: center, radius: radius, options: [])
        }
    }
}

@main
struct RenderPreview {
    static func main() {
        let output = CommandLine.arguments.dropFirst().first ?? "preview.png"
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)

        guard let screen = NSScreen.main else {
            FileHandle.standardError.write("No screen available\n".data(using: .utf8)!)
            exit(1)
        }
        let visible = screen.visibleFrame
        let canvas = NSRect(
            x: visible.midX - sectionSize.width / 2,
            y: visible.midY - sectionSize.height,
            width: sectionSize.width, height: sectionSize.height * 2
        )

        let backdrop = NSWindow(contentRect: canvas, styleMask: .borderless,
                                backing: .buffered, defer: false)
        backdrop.level = .floating
        backdrop.hasShadow = false
        backdrop.contentView = BackdropView(frame: NSRect(origin: .zero, size: canvas.size))
        backdrop.orderFrontRegardless()

        var keepAlive: [AnyObject] = [backdrop]
        for (index, dark) in [false, true].enumerated() {
            let frame = NSRect(
                x: canvas.minX,
                y: canvas.maxY - sectionSize.height * CGFloat(index + 1),
                width: sectionSize.width, height: sectionSize.height
            )
            let suite = "GboardIME.render-preview.\(index)"
            let defaults = UserDefaults(suiteName: suite)!
            defaults.removePersistentDomain(forName: suite)
            let preferences = Preferences(defaults: defaults)
            preferences.themeMode = dark ? .dark : .light

            let controller = CandidateWindowController(preferences: preferences)
            controller.update(
                candidates: candidates, pinyin: pinyin,
                canGoPrevious: false, canGoNext: true,
                onSelect: { _ in }, onPrevious: {}, onNext: {},
                onOpenSettings: {}
            )
            guard let panel = controller.window else { exit(1) }
            panel.setFrameOrigin(NSPoint(
                x: frame.midX - panel.frame.width / 2,
                y: frame.midY - panel.frame.height / 2
            ))
            panel.orderFrontRegardless()
            keepAlive += [controller, preferences]
        }

        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
            // screencapture -R takes top-left-origin points on the main display.
            let top = screen.frame.maxY - canvas.maxY
            let region = "\(Int(canvas.minX)),\(Int(top)),\(Int(canvas.width)),\(Int(canvas.height))"
            let capture = Process()
            capture.executableURL = URL(fileURLWithPath: "/usr/sbin/screencapture")
            capture.arguments = ["-x", "-R", region, output]
            do {
                try capture.run()
                capture.waitUntilExit()
            } catch {
                FileHandle.standardError.write("screencapture failed: \(error)\n".data(using: .utf8)!)
                exit(1)
            }
            _ = keepAlive
            exit(capture.terminationStatus)
        }
        app.run()
    }
}
