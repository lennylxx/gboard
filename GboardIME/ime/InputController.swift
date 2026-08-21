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
        let packDir = bundle.resourcePath.map { $0 + "/hmmoemdata/zh_cn_2025090307" } ?? ""
        imeLog("Engine init: so=\(soPath) pack=\(packDir)")
        engineReady = gboard_init(soPath, packDir)
        imeLog("Engine ready: \(engineReady)")
    }

    // ── Key handling ───────────────────────────────────────────────────────

    override func handle(_ event: NSEvent!, client sender: Any!) -> Bool {
        self.currentClient = sender

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
            return false
        }

        // ── Chinese mode ─────────────────────────────────────────────────
        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        let keyCode = event.keyCode
        let chars = event.characters ?? ""

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
                return false
            }
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

    func sessionInsertText(_ text: String) {
        if let client = currentClient as? IMKTextInput {
            client.insertText(text,
                              replacementRange: NSRange(location: NSNotFound, length: 0))
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

    func sessionShowCandidates(_ candidates: [String], pinyin: String, selectedIndex: Int) {
        if candidateWindow == nil {
            candidateWindow = CandidateWindowController()
        }
        candidateWindow?.update(candidates: candidates, pinyin: pinyin, selectedIndex: selectedIndex) { [weak self] idx in
            guard let self = self else { return }
            self.session.selectCandidate(index: idx)
        }
        if let client = currentClient as? IMKTextInput {
            var rect = NSRect.zero
            client.attributes(forCharacterIndex: 0, lineHeightRectangle: &rect)
            candidateWindow?.show(near: rect)
        }
    }

    func sessionHideCandidates() {
        candidateWindow?.close()
        candidateWindow = nil
    }

    // ── IMKit required ─────────────────────────────────────────────────────

    override func commitComposition(_ sender: Any!) {
        currentClient = sender
        _ = session.commitRawPinyin()
    }

    override func deactivateServer(_ sender: Any!) {
        _ = session.commitRawPinyin()
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
