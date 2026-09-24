import Cocoa
import InputMethodKit

private let userDictionaryPersistInterval: TimeInterval = 4 * 60 * 60

// MARK: - InputController

@objc(GboardInputController)
class GboardInputController: IMKInputController, PinyinSessionDelegate {

    private static var engineReady = false
    private let session = PinyinSession()
    private var candidateWindow: CandidateWindowController?

    /// true = Chinese input, false = English passthrough
    private var chineseMode = true
    private var shiftTracker = ShiftToggleTracker()

    /// Periodic user dictionary persistence (every 4 hours)
    private static var persistTimer: Timer?
    private static let persistQueue = DispatchQueue(
        label: "com.lennylxx.inputmethod.GboardIME.user-dictionary-persist"
    )

    override init!(server: IMKServer!, delegate: Any!, client: Any!) {
        super.init(server: server, delegate: delegate, client: client)
        session.delegate = self
        Self.initEngineOnce()
        imeLog("InputController init — server=\(String(describing: server))")
    }

    private static func initEngineOnce() {
        guard !engineReady else { return }
        let bundle = Bundle.main
        let soPath = bundle.path(forResource: "libintegrated_shared_object", ofType: "so") ?? ""
        let packDir = bundle.resourcePath.map { $0 + "/hmmoemdata/current" } ?? ""

        // User data directory for persistent user dictionary
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory,
                                                   in: .userDomainMask).first
        let userDataDir = appSupport?.appendingPathComponent("GboardIME").path ?? ""

        imeLog("Engine init: so=\(soPath) pack=\(packDir) userData=\(userDataDir)")
        engineReady = gboard_init_with_user_data(soPath, packDir, userDataDir)
        imeLog("Engine ready: \(engineReady)")

        // Schedule periodic persistence every 4 hours
        if engineReady && gboard_user_dict_is_ready() {
            persistTimer = Timer.scheduledTimer(
                withTimeInterval: userDictionaryPersistInterval,
                repeats: true
            ) { _ in
                imeLog("Periodic user dict persist (size=\(gboard_user_dict_get_size()))")
                persistUserDictionary()
            }
        }
    }

    static func persistUserDictionary(wait: Bool = false) {
        if !wait {
            persistQueue.async {
                _ = gboard_user_dict_persist()
            }
            return
        }

        let completed = DispatchSemaphore(value: 0)
        persistQueue.async {
            _ = gboard_user_dict_persist()
            completed.signal()
        }
        _ = completed.wait(timeout: .now() + 5)
    }

    // ── Key handling ───────────────────────────────────────────────────────

    override func handle(_ event: NSEvent!, client sender: Any!) -> Bool {
        self.currentClient = sender

        // ── Shift / CapsLock toggle detection ───────────────────────────
        let switchMode = PreferencesManager.shared.switchMode
        if event.type == .flagsChanged {
            if switchMode == .shift {
                if shiftTracker.handleFlagsChanged(keyCode: event.keyCode, modifierFlags: event.modifierFlags) == .shouldToggle {
                    toggleChineseMode()
                    return true
                }
            } else if switchMode == .capsLock && event.keyCode == 57 {
                toggleChineseMode()
                return true
            }
            return false
        }

        if event.type == .keyUp {
            if switchMode == .shift {
                if shiftTracker.handleKeyUp(keyCode: event.keyCode) == .shouldToggle {
                    toggleChineseMode()
                    return true
                }
            }
            return false
        }

        guard event.type == .keyDown else { return false }
        imeLog("handle: keyCode=\(event.keyCode) chars='\(event.characters ?? "")' composition='\(session.composition)' chinese=\(chineseMode)")

        shiftTracker.handleKeyDown(keyCode: event.keyCode, modifierFlags: event.modifierFlags)
        // Shift key itself as keyDown — ignore
        if event.keyCode == 56 || event.keyCode == 60 { return false }

        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        let keyCode = event.keyCode
        let chars = event.characters ?? ""
        let charsIgnoring = (event.charactersIgnoringModifiers ?? "").lowercased()

        // ── Global shortcuts (active in both Chinese and English mode) ──
        // 1. Command + Shift + F -> Toggle Simplified / Traditional
        let isFKey = (keyCode == 3 || charsIgnoring == "f")
        let isTradShortcut = isFKey && flags.contains(.command) && flags.contains(.shift) && !flags.contains(.control)
        if isTradShortcut {
            toggleTraditional()
            return true
        }

        // 2. Ctrl + 1 or Option + ? -> Toggle ?123 Symbols Mode
        let isOneKey = (keyCode == 18 || charsIgnoring == "1")
        let isQuestionOrSlashKey = (keyCode == 44 || charsIgnoring == "?" || charsIgnoring == "/")
        if (flags.contains(.control) && !flags.contains(.command) && isOneKey) ||
           (flags.contains(.option) && !flags.contains(.command) && isQuestionOrSlashKey) {
            if !chineseMode {
                chineseMode = true
            }
            session.toggleSymbolsMode()
            return true
        }

        // ── English mode: pass everything through ────────────────────────
        if !chineseMode {
            contextTracker.invalidate()
            return false
        }

        // ── Chinese mode ─────────────────────────────────────────────────
        let result: KeyResult

        switch keyCode {
        case 53: // Escape
            let wasComposing = session.isComposing
            session.cancel()
            return wasComposing
        case 36, 76: // Return / Enter — commit raw pinyin
            result = session.commitRawPinyin()
        case 51: // Delete
            result = session.deleteBack()
        case 123: // Left arrow
            result = session.moveLeft()
        case 124: // Right arrow
            result = session.moveRight()
        case 27 where session.isComposing && (flags.isEmpty || flags == .capsLock): // -
            result = session.previousPage()
        case 24 where session.isComposing && (flags.isEmpty || flags == .capsLock): // =
            result = session.nextPage()
        case 49 where session.isComposing: // Space
            result = session.selectCurrent()
        default:
            // Number keys 1-9 while composing
            if session.isComposing, let n = Int(chars), n >= 1 && n <= 9 {
                result = session.selectNumber(n)
            }
            // Chinese punctuation mapping (before modifier check — Shift produces ? " ^ $ etc.)
            else if chars.count == 1,
                    let ch = chars.first,
                    (flags.isEmpty || flags == .capsLock || flags == .shift || flags == [.shift, .capsLock]),
                    PinyinSession.isPunctuation(ch) {
                result = session.handlePunctuation(ch)
            }
            // Ignore modifier-only combos
            else if !(flags.isEmpty || flags == .capsLock) {
                contextTracker.invalidate()
                return false
            }
            // Accept a-z for Pinyin
            else if chars.count == 1,
                    let scalar = chars.unicodeScalars.first,
                    scalar.value >= 97 && scalar.value <= 122 {
                result = session.appendLetter(chars)
            }
            // Non-letter while composing — commit first then pass through
            else if session.isComposing {
                result = session.commitAndPassThrough()
            } else {
                contextTracker.invalidate()
                return false
            }
        }

        if case .passThrough = result {
            contextTracker.invalidate()
        }
        return result != .passThrough
    }

    private func toggleChineseMode() {
        if session.isComposing {
            _ = session.commitRawPinyin()
        }
        chineseMode.toggle()
        imeLog("Mode switched to \(chineseMode ? "Chinese" : "English")")
    }

    // ── PinyinSessionDelegate ────────────────────────────────────────────

    private var currentClient: Any?
    private var contextTracker = SessionContextTracker()

    func sessionContextBeforeInput() -> String {
        let clientContext: String?
        var inputContext: SessionInputContext?
        var selection = NSRange(location: NSNotFound, length: 0)
        if let client = currentClient as? IMKTextInput {
            selection = client.selectedRange()
            inputContext = SessionContextRetriever.retrieveInputContext(
                selection: selection
            ) {
                client.attributedSubstring(from: $0)
            }
            clientContext = inputContext.map {
                SessionContextRetriever.trailingContext(
                    in: $0.beforeSelection
                )
            } ?? SessionContextRetriever.retrieve(selection: selection) {
                client.attributedSubstring(from: $0)
            }
        } else {
            clientContext = nil
        }
        let context = contextTracker.resolve(clientContext: clientContext)
        updateNativeInputContext(
            inputContext ?? SessionInputContext(
                beforeSelection: context,
                selectedText: "",
                afterSelection: ""
            )
        )
        let source = clientContext == nil ? "fallback" : "client"
        imeLog(
            "Context: selection=(\(selection.location),\(selection.length)) " +
            "source=\(source) utf16=\((context as NSString).length)"
        )
        return context
    }

    func sessionInsertText(_ text: String) {
        if let client = currentClient as? IMKTextInput {
            client.insertText(text,
                              replacementRange: NSRange(location: NSNotFound, length: 0))
            contextTracker.recordInsertedText(text)
            updateNativeInputContext(from: client)
        }
    }

    private func updateNativeInputContext(from client: IMKTextInput) {
        let selection = client.selectedRange()
        guard let context = SessionContextRetriever.retrieveInputContext(
            selection: selection,
            attributedSubstring: { client.attributedSubstring(from: $0) }
        ) else {
            return
        }
        updateNativeInputContext(context)
    }

    private func updateNativeInputContext(_ context: SessionInputContext) {
        context.beforeSelection.withCString { before in
            context.selectedText.withCString { selected in
                context.afterSelection.withCString { after in
                    _ = gboard_update_input_context(before, selected, after)
                }
            }
        }
    }

    func sessionSetMarkedText(_ text: String) {
        guard let client = currentClient as? IMKTextInput else { return }
        if text.isEmpty {
            client.setMarkedText("",
                                 selectionRange: NSRange(location: 0, length: 0),
                                 replacementRange: NSRange(location: NSNotFound, length: 0))
        } else {
            let attrs = mark(forStyle: kTSMHiliteSelectedRawText,
                             at: NSRange(location: NSNotFound, length: 0))
            let str = NSAttributedString(string: "\u{200B}",
                                         attributes: attrs as? [NSAttributedString.Key: Any])
            client.setMarkedText(str,
                                 selectionRange: NSRange(location: 1, length: 0),
                                 replacementRange: NSRange(location: NSNotFound, length: 0))
        }
    }

    func sessionShowCandidates(
        _ candidates: [String],
        pinyin: String,
        selectedIndex: Int,
        canGoPrevious: Bool,
        canGoNext: Bool
    ) {
        if candidateWindow == nil {
            candidateWindow = CandidateWindowController()
        }
        candidateWindow?.update(
            candidates: candidates,
            pinyin: pinyin,
            selectedIndex: selectedIndex,
            canGoPrevious: canGoPrevious,
            canGoNext: canGoNext,
            onSelect: { [weak self] idx in
                self?.session.selectCandidate(index: idx)
            },
            onPrevious: { [weak self] in
                _ = self?.session.previousPage()
            },
            onNext: { [weak self] in
                _ = self?.session.nextPage()
            }
        )
        var rect = NSRect.zero
        if let client = currentClient as? IMKTextInput {
            client.attributes(forCharacterIndex: 0, lineHeightRectangle: &rect)
        }
        if rect.isEmpty || rect == .zero {
            let mouseLoc = NSEvent.mouseLocation
            rect = NSRect(x: mouseLoc.x, y: mouseLoc.y, width: 1, height: 1)
        }
        candidateWindow?.show(near: rect)
    }

    func sessionHideCandidates() {
        candidateWindow?.close()
        candidateWindow = nil
    }

    // ── IMKit required ─────────────────────────────────────────────────────

    override func menu() -> NSMenu! {
        let menu = NSMenu(title: "Gboard")

        let titleItem = NSMenuItem(title: "Gboard 拼音输入法", action: nil, keyEquivalent: "")
        titleItem.isEnabled = false
        menu.addItem(titleItem)

        menu.addItem(NSMenuItem.separator())

        // 简繁切换
        let isTrad = PreferencesManager.shared.isTraditional
        let tradItem = NSMenuItem(
            title: isTrad ? "切换为简体中文" : "切换为繁体中文",
            action: #selector(toggleTraditionalMenuAction),
            keyEquivalent: "F"
        )
        tradItem.keyEquivalentModifierMask = [.command, .shift]
        tradItem.target = self
        tradItem.state = .off
        menu.addItem(tradItem)

        // ?123 标点与数字
        let symItem = NSMenuItem(
            title: "标点与符号",
            action: #selector(toggleSymbolsMenuAction),
            keyEquivalent: ""
        )
        symItem.target = self
        symItem.state = session.isSymbolsMode ? .on : .off
        menu.addItem(symItem)

        menu.addItem(NSMenuItem.separator())

        // 中英文切换模式子菜单
        let switchSubmenu = NSMenu(title: "中英文切换按键")
        for mode in ChineseEnglishSwitchMode.allCases {
            let item = NSMenuItem(
                title: mode.displayName,
                action: #selector(changeSwitchModeAction(_:)),
                keyEquivalent: ""
            )
            item.target = self
            item.tag = mode.rawValue
            item.state = (PreferencesManager.shared.switchMode == mode) ? .on : .off
            switchSubmenu.addItem(item)
        }
        let switchItem = NSMenuItem(title: "中英文切换按键", action: nil, keyEquivalent: "")
        switchItem.submenu = switchSubmenu
        menu.addItem(switchItem)

        menu.addItem(NSMenuItem.separator())

        // 偏好设置
        let prefItem = NSMenuItem(
            title: "偏好设置...",
            action: #selector(openPreferencesAction),
            keyEquivalent: ","
        )
        prefItem.target = self
        menu.addItem(prefItem)

        return menu
    }

    private func toggleTraditional() {
        shiftTracker.cancelTracking()
        PreferencesManager.shared.toggleTraditional()
        if session.isComposing && !session.isSymbolsMode {
            session.fillCandidatePage(keepSelection: true)
        }
        imeLog("Traditional toggled to: \(PreferencesManager.shared.isTraditional)")
    }

    @objc private func toggleTraditionalMenuAction() {
        shiftTracker.cancelTracking()
        toggleTraditional()
    }

    @objc private func toggleSymbolsMenuAction() {
        shiftTracker.cancelTracking()
        if !chineseMode {
            chineseMode = true
        }
        session.toggleSymbolsMode()
    }

    @objc private func changeSwitchModeAction(_ sender: NSMenuItem) {
        if let mode = ChineseEnglishSwitchMode(rawValue: sender.tag) {
            PreferencesManager.shared.switchMode = mode
        }
    }

    @objc private func openPreferencesAction() {
        DispatchQueue.main.async {
            SettingsWindowController.shared.show()
        }
    }

    override func commitComposition(_ sender: Any!) {
        currentClient = sender
        _ = session.commitRawPinyin()
    }

    override func deactivateServer(_ sender: Any!) {
        _ = session.commitRawPinyin()
        contextTracker.invalidate()
        candidateWindow?.close()
        candidateWindow = nil
        super.deactivateServer(sender)
    }

    override func recognizedEvents(_ sender: Any!) -> Int {
        let mask: NSEvent.EventTypeMask = [.keyDown, .keyUp, .flagsChanged]
        return Int(mask.rawValue)
    }

    override func candidates(_ sender: Any!) -> [Any]! { return [] }
}
