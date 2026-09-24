// PinyinSession — pure composition logic, no IMKit dependency.
// Used by GboardInputController and directly by tests.

import Foundation

/// Callback protocol for UI actions (insertText, setMarkedText, candidate display).
protocol PinyinSessionDelegate: AnyObject {
    func sessionContextBeforeInput() -> String
    func sessionInsertText(_ text: String)
    func sessionSetMarkedText(_ text: String)
    func sessionShowCandidates(
        _ candidates: [String],
        pinyin: String,
        selectedIndex: Int,
        canGoPrevious: Bool,
        canGoNext: Bool
    )
    func sessionHideCandidates()
}

extension PinyinSessionDelegate {
    func sessionContextBeforeInput() -> String { "" }
}

/// Result of handling a key action.
enum KeyResult {
    case handled        // IME consumed the key
    case passThrough    // let the system handle it
    case commitAndPass  // committed text, then pass key through
}

class PinyinSession {
    var candidatePageSize: Int = 9
    var isTraditional: Bool {
        get { PreferencesManager.shared.isTraditional }
        set {
            guard PreferencesManager.shared.isTraditional != newValue else { return }
            PreferencesManager.shared.isTraditional = newValue
        }
    }

    private(set) var composition = ""
    private(set) var candidates: [String] = []
    private(set) var selectedIndex = 0
    private(set) var candidatePage = 0
    private(set) var hasNextPage = false

    // MARK: - ?123 Symbols Mode
    private static let symbolPages: [[String]] = [
        ["，", "。", "、", "？", "！", "；", "：", "“", "”"],
        ["《", "》", "【", "】", "（", "）", "——", "……", "～"],
        ["＋", "－", "×", "÷", "＝", "％", "·", "＆", "§"],
        ["￥", "＄", "€", "£", "¢", "©", "®", "™", "@"],
    ]

    private(set) var isSymbolsMode = false
    private var symbolPageIndex = 0
    private var prefsObserver: NSObjectProtocol?

    weak var delegate: PinyinSessionDelegate?

    init() {
        self.candidatePageSize = PreferencesManager.shared.candidatePageSize
        self.prefsObserver = NotificationCenter.default.addObserver(
            forName: PreferencesManager.didChangeNotification,
            object: nil,
            queue: nil
        ) { [weak self] _ in
            guard let self = self else { return }
            self.candidatePageSize = PreferencesManager.shared.candidatePageSize
            if self.isComposing && !self.isSymbolsMode {
                self.fillCandidatePage(keepSelection: true)
            }
        }
    }

    deinit {
        if let observer = prefsObserver {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    func toTraditionalIfEnabled(_ text: String) -> String {
        guard isTraditional else { return text }
        return (text as NSString).applyingTransform(StringTransform("Hans-Hant"), reverse: false) ?? text
    }

    func enterSymbolsMode() {
        if isComposing && !isSymbolsMode {
            _ = commitRawPinyin()
        }
        isSymbolsMode = true
        symbolPageIndex = 0
        // Set marked text so IMKit knows we are composing and routes all key events to handle()
        delegate?.sessionSetMarkedText("?123")
        fillSymbolsPage(resetSelection: true)
    }

    func exitSymbolsMode() {
        guard isSymbolsMode else { return }
        isSymbolsMode = false
        symbolPageIndex = 0
        delegate?.sessionSetMarkedText("")
        reset()
    }

    func toggleSymbolsMode() {
        if isSymbolsMode {
            exitSymbolsMode()
        } else {
            enterSymbolsMode()
        }
    }

    private func fillSymbolsPage(resetSelection: Bool = false) {
        guard isSymbolsMode else { return }
        candidates = Self.symbolPages[symbolPageIndex]
        if resetSelection {
            selectedIndex = 0
        }
        hasNextPage = symbolPageIndex < Self.symbolPages.count - 1
        let title = "?123 (\(symbolPageIndex + 1)/\(Self.symbolPages.count))"
        delegate?.sessionShowCandidates(
            candidates,
            pinyin: title,
            selectedIndex: selectedIndex,
            canGoPrevious: symbolPageIndex > 0,
            canGoNext: hasNextPage
        )
    }

    // MARK: - Chinese punctuation

    private static let punctuationMap: [Character: String] = [
        ",": "，", ".": "。", "?": "？", "!": "！",
        ":": "：", ";": "；", "\\": "、",
        "(": "（", ")": "）",
        "[": "【", "]": "】",
        "<": "《", ">": "》",
        "^": "……", "_": "——",
        "$": "￥", "~": "～",
    ]

    /// Paired punctuation: toggles between opening and closing forms.
    private static let pairedPunctuation: [Character: (String, String)] = [
        "\"": ("\u{201C}", "\u{201D}"),  // " "
        "'":  ("\u{2018}", "\u{2019}"),  // ' '
    ]

    private var doubleQuoteOpen = true
    private var singleQuoteOpen = true
    private var pendingLearningText = ""
    private var pendingLearningTokens: [String] = []
    private var pendingLearningTypes: [Int32] = []
    private var pendingLearningValid = true

    // MARK: - Key actions (called by InputController or test harness)

    func appendLetter(_ ch: String) -> KeyResult {
        if isSymbolsMode {
            exitSymbolsMode()
        }
        if composition.isEmpty {
            contextBeforeInput = delegate?.sessionContextBeforeInput() ?? ""
        }
        composition += ch
        fetchCandidates()
        delegate?.sessionSetMarkedText(segmentedPinyin)
        return .handled
    }

    func deleteBack() -> KeyResult {
        if isSymbolsMode {
            exitSymbolsMode()
            return .handled
        }
        guard !composition.isEmpty else { return .passThrough }
        let removed = composition.removeLast()
        if removed == "'" {
            // Recalculate separator positions from remaining composition
            separatorPositions = []
            var letterCount: Int32 = 0
            for c in composition {
                if c == "'" { separatorPositions.append(letterCount) }
                else { letterCount += 1 }
            }
        }
        if composition.isEmpty {
            cancel()
        } else {
            fetchCandidates()
            delegate?.sessionSetMarkedText(segmentedPinyin)
        }
        return .handled
    }

    func commitRawPinyin() -> KeyResult {
        if isSymbolsMode {
            exitSymbolsMode()
            return .handled
        }
        guard !composition.isEmpty else { return .passThrough }
        delegate?.sessionInsertText(composition)
        reset()
        return .handled
    }

    func selectCurrent() -> KeyResult {
        guard isComposing else { return .passThrough }
        guard !candidates.isEmpty else {
            // No candidates — commit raw pinyin
            delegate?.sessionInsertText(composition)
            reset()
            return .handled
        }
        return selectCandidate(index: selectedIndex)
    }

    @discardableResult
    func selectCandidate(index: Int) -> KeyResult {
        if isSymbolsMode {
            guard index >= 0 && index < candidates.count else { return .handled }
            let text = candidates[index]
            delegate?.sessionInsertText(text)
            exitSymbolsMode()
            return .handled
        }

        guard index >= 0 && index < candidates.count else {
            return selectCurrent()
        }
        let text = candidates[index]

        // Get the vertex range BEFORE selecting (like Android does)
        let engineIndex = candidatePage * candidatePageSize + index
        let consumed = Int(gboard_get_candidate_consumed(Int32(engineIndex)))

        // Capture this segment before selection mutates the engine state.
        let tokenCount = gboard_user_dict_extract_token_count(Int32(engineIndex))
        let letterCount = composition.filter { $0 != "'" }.count
        accumulateLearningSegment(text: text,
                                  engineIndex: engineIndex,
                                  tokenCount: Int(tokenCount))

        _ = gboard_select(Int32(engineIndex))

        delegate?.sessionInsertText(text)

        // Map vertex count to composition index (skipping apostrophes)
        let remaining: String
        if consumed > 0 && consumed < letterCount {
            // Find the position in composition corresponding to `consumed` letters
            var letters = 0
            var dropCount = 0
            for c in composition {
                if letters >= consumed { break }
                dropCount += 1
                if c != "'" { letters += 1 }
            }
            remaining = String(composition.dropFirst(dropCount))
        } else {
            remaining = ""
        }

        if !remaining.isEmpty {
            composition = remaining
            fetchCandidates()
            delegate?.sessionSetMarkedText(segmentedPinyin)
            if !candidates.isEmpty {
                return .handled
            }
        } else {
            commitPendingLearning()
        }
        reset()
        return .handled
    }

    func selectNumber(_ n: Int) -> KeyResult {
        guard n >= 1 && n <= 9 && isComposing else { return .passThrough }
        return selectCandidate(index: n - 1)
    }

    func moveLeft() -> KeyResult {
        if isSymbolsMode {
            if selectedIndex > 0 {
                selectedIndex -= 1
                fillSymbolsPage(resetSelection: false)
            } else if symbolPageIndex > 0 {
                symbolPageIndex -= 1
                selectedIndex = Self.symbolPages[symbolPageIndex].count - 1
                fillSymbolsPage(resetSelection: false)
            }
            return .handled
        }
        guard !composition.isEmpty && !candidates.isEmpty else { return .passThrough }
        if selectedIndex > 0 {
            selectedIndex -= 1
            notifyCandidates()
        } else if candidatePage > 0 {
            _ = previousPage()
            selectedIndex = candidates.count - 1
            notifyCandidates()
        }
        return .handled
    }

    func moveRight() -> KeyResult {
        if isSymbolsMode {
            if selectedIndex < candidates.count - 1 {
                selectedIndex += 1
                fillSymbolsPage(resetSelection: false)
            } else if symbolPageIndex < Self.symbolPages.count - 1 {
                symbolPageIndex += 1
                selectedIndex = 0
                fillSymbolsPage(resetSelection: false)
            }
            return .handled
        }
        guard !composition.isEmpty && !candidates.isEmpty else { return .passThrough }
        if selectedIndex < candidates.count - 1 {
            selectedIndex += 1
            notifyCandidates()
        } else {
            _ = nextPage()
        }
        return .handled
    }

    func previousPage() -> KeyResult {
        if isSymbolsMode {
            if symbolPageIndex > 0 {
                symbolPageIndex -= 1
                fillSymbolsPage(resetSelection: true)
            }
            return .handled
        }
        guard !composition.isEmpty && candidatePage > 0 else { return .handled }
        candidatePage -= 1
        fillCandidatePage()
        return .handled
    }

    func nextPage() -> KeyResult {
        if isSymbolsMode {
            if symbolPageIndex < Self.symbolPages.count - 1 {
                symbolPageIndex += 1
                fillSymbolsPage(resetSelection: true)
            }
            return .handled
        }
        guard !composition.isEmpty && !candidates.isEmpty else { return .passThrough }
        guard hasNextPage else { return .handled }
        candidatePage += 1
        fillCandidatePage()
        return .handled
    }

    func cancel() {
        if isSymbolsMode {
            exitSymbolsMode()
            return
        }
        delegate?.sessionSetMarkedText("")
        reset()
    }

    var isComposing: Bool { !composition.isEmpty || isSymbolsMode }

    // MARK: - Punctuation

    /// Returns true if the character has a Chinese punctuation mapping.
    static func isPunctuation(_ ch: Character) -> Bool {
        punctuationMap[ch] != nil || pairedPunctuation[ch] != nil
    }

    /// Returns the Chinese punctuation for the given character, or nil if not mapped.
    func chinesePunctuation(for ch: Character) -> String? {
        if let mapped = Self.punctuationMap[ch] {
            return mapped
        }
        if let pair = Self.pairedPunctuation[ch] {
            if ch == "\"" {
                let result = doubleQuoteOpen ? pair.0 : pair.1
                doubleQuoteOpen.toggle()
                return result
            } else {
                let result = singleQuoteOpen ? pair.0 : pair.1
                singleQuoteOpen.toggle()
                return result
            }
        }
        return nil
    }

    /// Handles a punctuation key: commits composition if needed, then inserts Chinese punctuation.
    /// Exception: apostrophe while composing sets a separator in the engine (e.g. xi'an).
    func handlePunctuation(_ ch: Character) -> KeyResult {
        if isSymbolsMode {
            exitSymbolsMode()
            if let punct = chinesePunctuation(for: ch) {
                delegate?.sessionInsertText(punct)
            } else {
                delegate?.sessionInsertText(String(ch))
            }
            return .handled
        }
        if (ch == "'" || ch == "\u{2019}" || ch == "\u{2018}") && isComposing {
            composition += "'"
            // Track separator at the vertex matching the letter count up to this apostrophe
            let vertexPos = Int32(composition.filter { $0 != "'" }.count)
            if !separatorPositions.contains(vertexPos) {
                separatorPositions.append(vertexPos)
            }
            _ = gboard_set_separator(vertexPos, 1)  // 1 = TOKEN_SEPARATOR
            refillCandidates()
            delegate?.sessionSetMarkedText(segmentedPinyin)
            return .handled
        }
        guard let punct = chinesePunctuation(for: ch) else { return .passThrough }
        if isComposing {
            _ = selectCurrent()
        }
        delegate?.sessionInsertText(punct)
        return .handled
    }

    // MARK: - Non-letter while composing

    func commitAndPassThrough() -> KeyResult {
        guard !composition.isEmpty else { return .passThrough }
        _ = selectCurrent()
        return .commitAndPass
    }

    // MARK: - Pinyin segmentation

    /// Reading text from the engine's current segment/token structure.
    private(set) var segmentedPinyin: String = ""

    private func updateSegmentation() {
        guard !composition.isEmpty else { segmentedPinyin = ""; return }
        var buffer = [CChar](repeating: 0, count: 256)
        if gboard_get_segmented_pinyin(&buffer, Int32(buffer.count)) > 0 {
            segmentedPinyin = String(cString: buffer)
        } else {
            segmentedPinyin = String(composition.filter { $0 != "'" })
        }

        if composition.hasSuffix("'") && !segmentedPinyin.hasSuffix("'") {
            segmentedPinyin += "'"
        }
    }

    // MARK: - User dictionary learning

    private func accumulateLearningSegment(text: String, engineIndex: Int,
                                           tokenCount: Int) {
        guard gboard_user_dict_is_ready() else {
            pendingLearningValid = false
            return
        }
        guard tokenCount > 0 else {
            pendingLearningValid = false
            return
        }

        var tokenStrings: [String] = []
        var types: [Int32] = []

        for i in 0..<tokenCount {
            var tokBuf = [CChar](repeating: 0, count: 16)
            var typeVal: Int32 = 0
            if gboard_user_dict_get_token(Int32(engineIndex), Int32(i), &tokBuf, &typeVal) {
                let s = String(cString: tokBuf)
                if !s.isEmpty {
                    tokenStrings.append(s)
                    types.append(typeVal)
                }
            }
        }

        guard tokenStrings.count == tokenCount, types.count == tokenCount else {
            pendingLearningValid = false
            return
        }

        pendingLearningText += text
        pendingLearningTokens.append(contentsOf: tokenStrings)
        pendingLearningTypes.append(contentsOf: types)
    }

    private func commitPendingLearning() {
        guard pendingLearningValid,
              !pendingLearningText.isEmpty,
              !pendingLearningTokens.isEmpty,
              pendingLearningTokens.count == pendingLearningTypes.count else {
            return
        }

        pendingLearningTokens.withCString2DArray { cTokens in
            pendingLearningTypes.withUnsafeMutableBufferPointer { cTypes in
                pendingLearningText.withCString { cValue in
                    _ = gboard_user_dict_learn(
                        cTokens, cTypes.baseAddress!,
                        Int32(pendingLearningTokens.count), cValue, true)
                }
            }
        }
    }

    // MARK: - Internal

    func reset() {
        composition = ""
        candidates = []
        selectedIndex = 0
        candidatePage = 0
        hasNextPage = false
        isSymbolsMode = false
        symbolPageIndex = 0
        separatorPositions = []
        contextBeforeInput = ""
        pendingLearningText = ""
        pendingLearningTokens = []
        pendingLearningTypes = []
        pendingLearningValid = true
        gboard_reset()
        delegate?.sessionHideCandidates()
    }

    /// Separator vertex positions set by user apostrophes.
    private var separatorPositions: [Int32] = []
    private var contextBeforeInput = ""

    private func fetchCandidates() {
        contextBeforeInput.withCString { context in
            _ = gboard_set_context(context)
        }
        // Append only letters (skip apostrophes)
        let letters = String(composition.filter { $0 != "'" })
        letters.withCString { gboard_prepare_input($0) }
        gboard_reset()
        guard gboard_append(letters) else {
            candidates = []
            delegate?.sessionHideCandidates()
            return
        }
        // Restore user-set separators after reset+append
        for pos in separatorPositions {
            gboard_set_separator(pos, 1)
        }
        candidatePage = 0
        fillCandidatePage()
    }

    /// Refill candidates on current engine state (no reset), used after setting a separator.
    private func refillCandidates() {
        candidatePage = 0
        fillCandidatePage()
    }

    func fillCandidatePage(keepSelection: Bool = false) {
        let oldSelected = selectedIndex
        let maxCount = candidatePageSize + 1
        var bufs = [UnsafeMutablePointer<CChar>?](repeating: nil, count: maxCount)
        let offset = candidatePage * candidatePageSize
        let count = Int(gboard_get_candidates_page(&bufs, Int32(offset), Int32(maxCount)))

        var results: [String] = []
        for i in 0..<count {
            if let ptr = bufs[i] {
                let text = String(cString: ptr)
                results.append(toTraditionalIfEnabled(text))
            }
        }

        hasNextPage = results.count > candidatePageSize
        candidates = Array(results.prefix(candidatePageSize))
        if keepSelection && oldSelected < candidates.count {
            selectedIndex = oldSelected
        } else {
            selectedIndex = 0
        }
        updateSegmentation()
        notifyCandidates()
    }

    private func notifyCandidates() {
        if candidates.isEmpty {
            delegate?.sessionHideCandidates()
        } else {
            delegate?.sessionShowCandidates(
                candidates,
                pinyin: segmentedPinyin,
                selectedIndex: selectedIndex,
                canGoPrevious: candidatePage > 0,
                canGoNext: hasNextPage
            )
        }
    }
}

// MARK: - Helper: convert [String] to C string pointer array

extension Array where Element == String {
    func withCString2DArray<R>(_ body: (UnsafeMutablePointer<UnsafePointer<CChar>?>) -> R) -> R {
        let cStrings = self.map { strdup($0) }
        defer { cStrings.forEach { free($0) } }
        var ptrs: [UnsafePointer<CChar>?] = cStrings.map { ptr in
            ptr.map { UnsafePointer($0) }
        }
        return ptrs.withUnsafeMutableBufferPointer { buf in
            body(buf.baseAddress!)
        }
    }
}
