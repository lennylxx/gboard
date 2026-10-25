import Cocoa
import InputMethodKit

// MARK: - InputController

@objc(GboardInputController)
class GboardInputController: IMKInputController, PinyinSessionDelegate {

    private static var engineReady = false
    private let session = PinyinSession()
    private var candidateWindow: CandidateWindowController?

    /// true = Chinese input, false = English passthrough
    private var chineseMode = true
    private var shiftTracker = ShiftToggleTracker()
    private var preferencesObserver: NSObjectProtocol?

    override init!(server: IMKServer!, delegate: Any!, client: Any!) {
        super.init(server: server, delegate: delegate, client: client)
        session.delegate = self
        Self.initEngineOnce()
        shiftTracker.isEnabled = Preferences.shared.shiftToggleEnabled
        preferencesObserver = NotificationCenter.default.addObserver(
            forName: Preferences.didChangeNotification,
            object: Preferences.shared, queue: nil
        ) { [weak self] _ in
            guard let self = self else { return }
            self.shiftTracker.isEnabled = Preferences.shared.shiftToggleEnabled
            if !Preferences.shared.shiftToggleEnabled {
                self.chineseMode = true
            }
            if !Preferences.shared.contextEnabled {
                self.contextTracker.invalidate()
            }
        }
        imeLog("InputController init — server=\(String(describing: server))")
    }

    deinit {
        if let observer = preferencesObserver {
            NotificationCenter.default.removeObserver(observer)
        }
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
        if engineReady {
            Preferences.shared.applyPinyinScheme()
            Preferences.shared.applyFuzzyPinyin()
        }

        if engineReady && gboard_user_dict_is_ready() {
            UserDictionary.shared.startAutomaticSaving()
        }
    }

    // ── Key handling ───────────────────────────────────────────────────────

    override func handle(_ event: NSEvent!, client sender: Any!) -> Bool {
        switchClient(to: sender)

        // ── Shift toggle detection ───────────────────────────────────────
        if event.type == .flagsChanged {
            if shiftTracker.handleFlagsChanged(keyCode: event.keyCode, modifierFlags: event.modifierFlags) == .shouldToggle {
                toggleChineseMode()
                return true
            }
            return false
        }

        if event.type == .keyUp {
            if shiftTracker.handleKeyUp(keyCode: event.keyCode) == .shouldToggle {
                toggleChineseMode()
                return true
            }
            return false
        }

        guard event.type == .keyDown else { return false }
        imeLog("handle: keyCode=\(event.keyCode) chars='\(event.characters ?? "")' composition='\(session.composition)' chinese=\(chineseMode)")

        shiftTracker.handleKeyDown(keyCode: event.keyCode, modifierFlags: event.modifierFlags)
        // Shift key itself as keyDown — ignore
        if event.keyCode == 56 || event.keyCode == 60 { return false }

        // ── English mode: pass everything through ────────────────────────
        if !chineseMode {
            contextTracker.invalidate()
            return false
        }

        // ── Chinese mode ─────────────────────────────────────────────────
        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        let keyCode = event.keyCode
        let chars = event.characters ?? ""
        let charsIgnoring = (event.charactersIgnoringModifiers ?? "").lowercased()

        // Chinese-mode shortcuts:
        // 1. Command + Shift + F -> Toggle Simplified / Traditional
        if flags == [.command, .shift], keyCode == 3 || charsIgnoring == "f" {
            toggleTraditional()
            return true
        }

        // 2. Ctrl + 1 or Option + ?/ (with or without Shift) -> Toggle ?123 Symbols Mode
        let isOptionSlashOrQuestion = (flags == .option || flags == [.option, .shift]) &&
            (keyCode == 44 || charsIgnoring == "?" || charsIgnoring == "/")
        if (flags == .control && (keyCode == 18 || charsIgnoring == "1")) ||
           isOptionSlashOrQuestion {
            shiftTracker.cancelTracking()
            session.toggleSymbolsMode()
            return true
        }

        let result: KeyResult

        // Predictions take Space, 1-9 and -/= paging; any other key
        // dismisses them and is handled normally.
        if session.isPredicting {
            if keyCode == 53 {
                session.dismissPredictions()
                return true
            }
            if flags.isEmpty || flags == .capsLock {
                switch keyCode {
                case 49:
                    return session.selectCurrent() == .handled
                case 27:
                    session.previousPredictionPage()
                    return true
                case 24:
                    session.nextPredictionPage()
                    return true
                default:
                    break
                }
                if let n = Int(chars), n >= 1 && n <= 9 {
                    if session.selectNumber(n) == .handled { return true }
                    contextTracker.invalidate()
                    return false
                }
            }
            session.dismissPredictions()
        }

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

    /// Drops composition and prediction state that belongs to another client
    /// so it can never be committed into the wrong document.
    private func switchClient(to sender: Any?) {
        if clientTracker.update(to: sender as AnyObject?) {
            session.handleClientSwitch()
            contextTracker.invalidate()
            windowAnchor = nil
        }
        currentClient = sender
    }

    private func toggleChineseMode() {
        session.dismissPredictions()
        if session.isComposing {
            _ = session.commitRawPinyin()
        }
        chineseMode.toggle()
        imeLog("Mode switched to \(chineseMode ? "Chinese" : "English")")
    }

    // ── PinyinSessionDelegate ────────────────────────────────────────────

    private var currentClient: Any?
    /// Where the candidate window was last anchored in this client; a
    /// trusted prediction caret advances it along a prediction chain.
    private var windowAnchor: NSRect?
    private var contextTracker = SessionContextTracker()
    private var clientTracker = SessionClientTracker()

    func sessionContextBeforeInput() -> String {
        guard Preferences.shared.contextEnabled else { return "" }
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
            if Preferences.shared.contextEnabled {
                contextTracker.recordInsertedText(text)
                updateNativeInputContext(from: client)
            }
        }
    }

    private func updateNativeInputContext(from client: IMKTextInput) {
        guard Preferences.shared.contextEnabled else { return }
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
        let beforeText = PinyinSession.toSimplified(context.beforeSelection)
        let selectedText = PinyinSession.toSimplified(context.selectedText)
        let afterText = PinyinSession.toSimplified(context.afterSelection)
        beforeText.withCString { before in
            selectedText.withCString { selected in
                afterText.withCString { after in
                    _ = gboard_update_input_context(before, selected, after)
                }
            }
        }
    }

    func sessionSetMarkedText(_ text: String) {
        guard let client = currentClient as? IMKTextInput else { return }
        let markedText = text == "?123"
            ? "\u{200B}"
            : Preferences.shared.pinyinDisplayMode.markedText(for: text)
        if markedText.isEmpty {
            client.setMarkedText("",
                                 selectionRange: NSRange(location: 0, length: 0),
                                 replacementRange: NSRange(location: NSNotFound, length: 0))
        } else {
            let attrs = mark(forStyle: kTSMHiliteSelectedRawText,
                             at: NSRange(location: NSNotFound, length: 0))
            let str = NSAttributedString(string: markedText,
                                         attributes: attrs as? [NSAttributedString.Key: Any])
            client.setMarkedText(str,
                                 selectionRange: NSRange(location: markedText.utf16.count, length: 0),
                                 replacementRange: NSRange(location: NSNotFound, length: 0))
        }
    }

    func sessionShowCandidates(
        _ candidates: [String],
        pinyin: PinyinReading,
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
                guard let session = self?.session else { return }
                if session.isPredicting {
                    session.selectPrediction(index: idx)
                } else {
                    session.selectCandidate(index: idx)
                }
            },
            onPrevious: { [weak self] in
                guard let session = self?.session else { return }
                if session.isPredicting {
                    session.previousPredictionPage()
                } else {
                    _ = session.previousPage()
                }
            },
            onNext: { [weak self] in
                guard let session = self?.session else { return }
                if session.isPredicting {
                    session.nextPredictionPage()
                } else {
                    _ = session.nextPage()
                }
            },
            onOpenSettings: { [weak self] in
                self?.session.dismissPredictions()
                self?.session.cancel()
                SettingsWindowController.shared.show()
            }
        )
        if let client = currentClient as? IMKTextInput {
            let rect = CandidateWindowController.anchorRect(
                isPrediction: session.isPredicting,
                caretRect: {
                    let location = client.selectedRange().location
                    guard location != NSNotFound else { return nil }
                    return client.firstRect(
                        forCharacterRange: NSRange(location: location, length: 0),
                        actualRange: nil)
                },
                markedTextRect: {
                    var rect = NSRect.zero
                    client.attributes(forCharacterIndex: 0,
                                      lineHeightRectangle: &rect)
                    return rect
                },
                previousAnchor: windowAnchor)
            if rect != .zero { windowAnchor = rect }
            candidateWindow?.show(near: rect)
        }
    }

    func sessionHideCandidates() {
        candidateWindow?.close()
        candidateWindow = nil
    }

    // ── IMKit required ─────────────────────────────────────────────────────

    override func menu() -> NSMenu! {
        let menu = NSMenu(title: "GboardIME")
        menu.autoenablesItems = false

        let isTrad = Preferences.shared.isTraditional
        let tradItem = NSMenuItem(
            title: isTrad ? "切换为简体中文" : "切换为繁体中文",
            action: #selector(toggleTraditionalMenuAction(_:)),
            keyEquivalent: ""
        )
        tradItem.target = self
        menu.addItem(tradItem)

        let symItem = NSMenuItem(
            title: "标点与符号 (?123)",
            action: #selector(toggleSymbolsMenuAction(_:)),
            keyEquivalent: ""
        )
        symItem.target = self
        symItem.state = session.isSymbolsMode ? .on : .off
        menu.addItem(symItem)

        menu.addItem(NSMenuItem.separator())

        let settingsItem = NSMenuItem(
            title: "设置…",
            action: #selector(showPreferences(_:)),
            keyEquivalent: ""
        )
        settingsItem.target = self
        menu.addItem(settingsItem)
        imeLog("Settings menu requested: \(menu.numberOfItems) item(s)")
        return menu
    }

    private func toggleTraditional() {
        shiftTracker.cancelTracking()
        Preferences.shared.toggleTraditional()
        imeLog("Traditional toggled to: \(Preferences.shared.isTraditional)")
    }

    @objc private func toggleTraditionalMenuAction(_ sender: Any!) {
        toggleTraditional()
    }

    @objc private func toggleSymbolsMenuAction(_ sender: Any!) {
        shiftTracker.cancelTracking()
        if !chineseMode {
            chineseMode = true
        }
        session.toggleSymbolsMode()
    }

    override func showPreferences(_ sender: Any!) {
        SettingsWindowController.shared.show()
    }

    override func commitComposition(_ sender: Any!) {
        switchClient(to: sender)
        session.dismissPredictions()
        _ = session.commitRawPinyin()
    }

    override func deactivateServer(_ sender: Any!) {
        session.dismissPredictions()
        _ = session.commitRawPinyin()
        contextTracker.invalidate()
        candidateWindow?.close()
        candidateWindow = nil
        currentClient = nil
        windowAnchor = nil
        clientTracker.clear()
        super.deactivateServer(sender)
    }

    override func recognizedEvents(_ sender: Any!) -> Int {
        let mask: NSEvent.EventTypeMask = [.keyDown, .keyUp, .flagsChanged]
        return Int(mask.rawValue)
    }

    override func candidates(_ sender: Any!) -> [Any]! { return [] }
}
