import Cocoa
import InputMethodKit

// MARK: - InputController

@objc(GboardInputController)
class GboardInputController: IMKInputController {

    private static var engineReady = false

    private var composition = ""          // accumulated pinyin (e.g. "nihao")
    private var candidates: [String] = []
    private var candidateWindow: CandidateWindowController?

    override init!(server: IMKServer!, delegate: Any!, client: Any!) {
        super.init(server: server, delegate: delegate, client: client)
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
        imeLog("handle: keyCode=\(event.keyCode) chars='\(event.characters ?? "")' composition='\(composition)'")
        guard event.type == .keyDown else { return false }

        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        let keyCode = event.keyCode
        let chars = event.characters ?? ""

        // Escape — cancel composition
        if keyCode == 53 {
            cancelComposition(client: sender)
            return !composition.isEmpty
        }

        // Return / Enter — commit first candidate or raw pinyin
        if keyCode == 36 || keyCode == 76 {
            return commitFirst(client: sender)
        }

        // Delete — remove last pinyin char
        if keyCode == 51 {
            return handleDelete(client: sender)
        }

        // Number keys 1-9 while candidate window is showing — select candidate
        if !composition.isEmpty, let n = Int(chars), n >= 1 && n <= 9 {
            return selectCandidate(index: n - 1, client: sender)
        }

        // Space — select first candidate if composing
        if keyCode == 49 && !composition.isEmpty {
            return commitFirst(client: sender)
        }

        // Ignore modifier-only combos
        guard flags.isEmpty || flags == .capsLock else { return false }

        // Accept a-z for Pinyin
        guard chars.count == 1,
              let scalar = chars.unicodeScalars.first,
              scalar.value >= 97 && scalar.value <= 122 else {
            // Non-letter while composing — commit first then pass through
            if !composition.isEmpty { _ = commitFirst(client: sender) }
            return false
        }

        return appendPinyin(chars, client: sender)
    }

    // ── Composition helpers ────────────────────────────────────────────────

    private func appendPinyin(_ ch: String, client sender: Any!) -> Bool {
        composition += ch
        updatePreedit(client: sender)
        fetchCandidates(client: sender)
        return true
    }

    private func handleDelete(client sender: Any!) -> Bool {
        guard !composition.isEmpty else { return false }
        composition.removeLast()
        if composition.isEmpty {
            cancelComposition(client: sender)
        } else {
            updatePreedit(client: sender)
            fetchCandidates(client: sender)
        }
        return true
    }

    private func updatePreedit(client sender: Any!) {
        guard let client = sender as? IMKTextInput else { return }
        let attrs = mark(forStyle: kTSMHiliteSelectedRawText,
                         at: NSRange(location: NSNotFound, length: 0))
        let str = NSAttributedString(string: composition,
                                     attributes: attrs as? [NSAttributedString.Key: Any])
        client.setMarkedText(str,
                             selectionRange: NSRange(location: composition.count, length: 0),
                             replacementRange: NSRange(location: NSNotFound, length: 0))
    }

    private func fetchCandidates(client sender: Any!) {
        guard Self.engineReady else {
            imeLog("fetchCandidates: engine not ready")
            return
        }

        // Reset engine state and feed current composition
        gboard_reset()
        imeLog("fetchCandidates: appending '\(composition)'")
        guard gboard_append(composition) else {
            imeLog("fetchCandidates: gboard_append returned false")
            candidates = []
            showCandidates([], client: sender)
            return
        }

        // Read candidates from engine
        let maxCount = 9
        var bufs = [UnsafeMutablePointer<CChar>?](repeating: nil, count: maxCount)
        let count = Int(gboard_get_candidates(&bufs, Int32(maxCount)))
        imeLog("fetchCandidates: got \(count) candidates")

        var results: [String] = []
        for i in 0..<count {
            if let ptr = bufs[i] {
                results.append(String(cString: ptr))
            }
        }

        candidates = results
        showCandidates(results, client: sender)
    }

    private func showCandidates(_ candidates: [String], client sender: Any!) {
        if candidates.isEmpty {
            candidateWindow?.close()
            candidateWindow = nil
            return
        }
        if candidateWindow == nil {
            candidateWindow = CandidateWindowController()
        }
        candidateWindow?.update(candidates: candidates, pinyin: composition) { [weak self] idx in
            self?.selectCandidate(index: idx, client: sender)
        }
        // Position near cursor
        if let client = sender as? IMKTextInput {
            var rect = NSRect.zero
            client.attributes(forCharacterIndex: 0, lineHeightRectangle: &rect)
            candidateWindow?.show(near: rect)
        }
    }

    @discardableResult
    private func commitFirst(client sender: Any!) -> Bool {
        guard !composition.isEmpty else { return false }
        if !candidates.isEmpty {
            return selectCandidate(index: 0, client: sender)
        }
        // No candidates — commit raw pinyin
        if let client = sender as? IMKTextInput {
            client.insertText(composition,
                              replacementRange: NSRange(location: NSNotFound, length: 0))
        }
        resetState()
        return true
    }

    @discardableResult
    private func selectCandidate(index: Int, client sender: Any!) -> Bool {
        guard index >= 0 && index < candidates.count else {
            return commitFirst(client: sender)
        }
        let text = candidates[index]
        gboard_select(Int32(index))
        if let client = sender as? IMKTextInput {
            client.insertText(text,
                              replacementRange: NSRange(location: NSNotFound, length: 0))
        }
        resetState()
        return true
    }

    private func cancelComposition(client sender: Any!) {
        if let client = sender as? IMKTextInput {
            client.setMarkedText("",
                                 selectionRange: NSRange(location: 0, length: 0),
                                 replacementRange: NSRange(location: NSNotFound, length: 0))
        }
        resetState()
    }

    private func resetState() {
        composition = ""
        candidates = []
        gboard_reset()
        candidateWindow?.close()
        candidateWindow = nil
    }

    // ── IMKit required ─────────────────────────────────────────────────────

    override func commitComposition(_ sender: Any!) {
        commitFirst(client: sender)
    }

    override func candidates(_ sender: Any!) -> [Any]! { return [] }
}
