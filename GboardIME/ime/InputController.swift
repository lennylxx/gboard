import Cocoa
import InputMethodKit

// MARK: - InputController

@objc(GboardInputController)
class GboardInputController: IMKInputController {

    private static var engineReady = false

    private var composition = ""          // accumulated pinyin (e.g. "nihao")
    private var candidates: [String] = []
    private var selectedIndex = 0
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

        // Left/Right arrow — move highlighted candidate
        if !composition.isEmpty && !candidates.isEmpty {
            if keyCode == 123 { // Left arrow
                if selectedIndex > 0 {
                    selectedIndex -= 1
                    showCandidates(candidates, client: sender)
                }
                return true
            }
            if keyCode == 124 { // Right arrow
                if selectedIndex < candidates.count - 1 {
                    selectedIndex += 1
                    showCandidates(candidates, client: sender)
                }
                return true
            }
        }

        // Number keys 1-9 while candidate window is showing — select candidate
        if !composition.isEmpty, let n = Int(chars), n >= 1 && n <= 9 {
            return selectCandidate(index: n - 1, client: sender)
        }

        // Space — select highlighted candidate if composing
        if keyCode == 49 && !composition.isEmpty {
            return selectCandidate(index: selectedIndex, client: sender)
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
        setInvisibleMarkedText(client: sender)
        fetchCandidates(client: sender)
        return true
    }

    private func handleDelete(client sender: Any!) -> Bool {
        guard !composition.isEmpty else { return false }
        composition.removeLast()
        if composition.isEmpty {
            cancelComposition(client: sender)
        } else {
            setInvisibleMarkedText(client: sender)
            fetchCandidates(client: sender)
        }
        return true
    }

    private func setInvisibleMarkedText(client sender: Any!) {
        guard let client = sender as? IMKTextInput else { return }
        let attrs = mark(forStyle: kTSMHiliteSelectedRawText,
                         at: NSRange(location: NSNotFound, length: 0))
        let str = NSAttributedString(string: "\u{200B}",
                                     attributes: attrs as? [NSAttributedString.Key: Any])
        client.setMarkedText(str,
                             selectionRange: NSRange(location: 1, length: 0),
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
        selectedIndex = 0
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
        candidateWindow?.update(candidates: candidates, pinyin: composition, selectedIndex: selectedIndex) { [weak self] idx in
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
            return selectCandidate(index: selectedIndex, client: sender)
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

        // Figure out how many pinyin chars were consumed
        let chineseCount = text.unicodeScalars.filter { $0.value >= 0x4E00 && $0.value <= 0x9FFF }.count
        var consumed = pinyinConsumedLength(composition, syllableCount: max(chineseCount, 1))
        if consumed == 0 { consumed = composition.count } // incomplete syllable — consume all
        let remainingPinyin = String(composition.dropFirst(consumed))
        imeLog("selectCandidate: text='\(text)' chineseCount=\(chineseCount) consumed=\(consumed) remaining='\(remainingPinyin)'")

        if !remainingPinyin.isEmpty {
            composition = remainingPinyin
            setInvisibleMarkedText(client: sender)
            fetchCandidates(client: sender)
            if !candidates.isEmpty {
                return true
            }
        }
        resetState()
        return true
    }

    /// Split pinyin into syllables using DP to find a valid full segmentation.
    /// Returns the number of characters consumed for `syllableCount` syllables.
    private func pinyinConsumedLength(_ pinyin: String, syllableCount: Int) -> Int {
        let s = Array(pinyin)
        let n = s.count
        // DP: segmentAt[i] = array of syllable lengths forming a valid segmentation of s[0..<i]
        var segmentAt = [Int: [Int]]()
        segmentAt[0] = []
        for i in 0..<n {
            guard let prev = segmentAt[i] else { continue }
            for len in 1...min(6, n - i) {
                let slice = String(s[i..<i+len])
                if Self.pinyinSyllables.contains(slice) && segmentAt[i + len] == nil {
                    segmentAt[i + len] = prev + [len]
                }
            }
        }
        // Use the full segmentation if available, otherwise best partial
        let best = segmentAt[n] ?? segmentAt.filter { $0.key > 0 }.max(by: { $0.key < $1.key })?.value ?? []
        return best.prefix(syllableCount).reduce(0, +)
    }

    private static let pinyinSyllables: Set<String> = {
        let initials = ["b","p","m","f","d","t","n","l","g","k","h",
                        "j","q","x","zh","ch","sh","r","z","c","s","y","w"]
        let finals = ["a","o","e","ai","ei","ao","ou","an","en","ang","eng","ong","er",
                      "i","ia","ie","iao","iu","ian","in","iang","ing","iong",
                      "u","ua","uo","uai","ui","uan","un","uang",
                      "v","ve","ue","van","vn","yuan","yue","yun"]
        var set = Set<String>()
        // Standalone finals
        for f in finals { set.insert(f) }
        // initial + final combos (not all are valid but the greedy parser works fine)
        for i in initials {
            for f in finals {
                set.insert(i + f)
            }
        }
        // Common standalone syllables that might be missed
        for s in ["a","o","e","ai","ei","ao","ou","an","en","ang","eng","er",
                   "yi","ya","ye","yao","you","yan","yin","yang","ying","yong",
                   "wu","wa","wo","wai","wei","wan","wen","wang","weng",
                   "yu","yue","yuan","yun",
                   "zhi","chi","shi","ri","zi","ci","si",
                   "ju","qu","xu","lv","nv","lve","nve"] {
            set.insert(s)
        }
        return set
    }()

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
        selectedIndex = 0
        gboard_reset()
        candidateWindow?.close()
        candidateWindow = nil
    }

    // ── IMKit required ─────────────────────────────────────────────────────

    override func commitComposition(_ sender: Any!) {
        commitFirst(client: sender)
    }

    override func deactivateServer(_ sender: Any!) {
        resetState()
        super.deactivateServer(sender)
    }

    override func candidates(_ sender: Any!) -> [Any]! { return [] }
}
