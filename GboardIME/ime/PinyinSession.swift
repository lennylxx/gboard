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
    private static let candidatePageSize = 9

    private(set) var composition = ""
    private(set) var candidates: [String] = []
    private(set) var selectedIndex = 0
    private(set) var candidatePage = 0
    private(set) var hasNextPage = false

    weak var delegate: PinyinSessionDelegate?

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

    // MARK: - Key actions (called by InputController or test harness)

    func appendLetter(_ ch: String) -> KeyResult {
        if composition.isEmpty {
            contextBeforeInput = delegate?.sessionContextBeforeInput() ?? ""
        }
        composition += ch
        fetchCandidates()
        delegate?.sessionSetMarkedText(segmentedPinyin)
        return .handled
    }

    func deleteBack() -> KeyResult {
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
        guard !composition.isEmpty else { return .passThrough }
        delegate?.sessionInsertText(composition)
        reset()
        return .handled
    }

    func selectCurrent() -> KeyResult {
        guard !composition.isEmpty else { return .passThrough }
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
        guard index >= 0 && index < candidates.count else {
            return selectCurrent()
        }
        let text = candidates[index]

        // Get the vertex range BEFORE selecting (like Android does)
        let engineIndex = candidatePage * Self.candidatePageSize + index
        let consumed = Int(gboard_get_candidate_consumed(Int32(engineIndex)))

        // Extract token info for learning BEFORE select mutates engine state
        let tokenCount = gboard_user_dict_extract_token_count(Int32(engineIndex))
        let letterCount = composition.filter { $0 != "'" }.count

        _ = gboard_select(Int32(engineIndex))

        // Learn the selected candidate
        if tokenCount > 0 && gboard_user_dict_is_ready() {
            learnCandidate(text: text,
                           engineIndex: engineIndex,
                           tokenCount: Int(tokenCount),
                           isFullMatch: consumed == letterCount)
        }

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
        }
        reset()
        return .handled
    }

    func selectNumber(_ n: Int) -> KeyResult {
        guard n >= 1 && n <= 9 && !composition.isEmpty else { return .passThrough }
        return selectCandidate(index: n - 1)
    }

    func moveLeft() -> KeyResult {
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
        guard !composition.isEmpty && candidatePage > 0 else { return .handled }
        candidatePage -= 1
        fillCandidatePage()
        return .handled
    }

    func nextPage() -> KeyResult {
        guard !composition.isEmpty && !candidates.isEmpty else { return .passThrough }
        guard hasNextPage else { return .handled }
        candidatePage += 1
        fillCandidatePage()
        return .handled
    }

    func cancel() {
        delegate?.sessionSetMarkedText("")
        reset()
    }

    var isComposing: Bool { !composition.isEmpty }

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

    /// Segmented pinyin from engine (e.g. "zhong'wen'shu'ru'fa").
    private(set) var segmentedPinyin: String = ""

    private func updateSegmentation() {
        guard !composition.isEmpty else { segmentedPinyin = ""; return }
        let letters = Array(composition.filter { $0 != "'" })
        guard !letters.isEmpty else { segmentedPinyin = composition; return }

        // Collect all break positions: engine breaks + user apostrophes
        var breakSet = Set<Int>()
        var breaks = [Int32](repeating: 0, count: 16)
        let n = Int(gboard_get_syllable_breaks(&breaks, Int32(breaks.count)))
        for i in 0..<n { breakSet.insert(Int(breaks[i])) }
        for pos in separatorPositions { breakSet.insert(Int(pos)) }

        if !breakSet.isEmpty {
            let sorted = breakSet.sorted()
            var parts: [String] = []
            var prev = 0
            for bp in sorted where bp > prev && bp < letters.count {
                parts.append(String(letters[prev..<bp]))
                prev = bp
            }
            parts.append(String(letters[prev...]))
            segmentedPinyin = parts.joined(separator: "'")
        } else {
            segmentedPinyin = String(letters)
        }
        // Preserve trailing apostrophe from user input
        if composition.hasSuffix("'") {
            segmentedPinyin += "'"
        }
    }

    // MARK: - User dictionary learning

    private func learnCandidate(text: String, engineIndex: Int, tokenCount: Int,
                                isFullMatch: Bool) {
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

        guard !tokenStrings.isEmpty else { return }

        // Call the C bridge
        tokenStrings.withCString2DArray { cTokens in
            types.withUnsafeMutableBufferPointer { cTypes in
                text.withCString { cValue in
                    _ = gboard_user_dict_learn(cTokens, cTypes.baseAddress!,
                                               Int32(tokenStrings.count), cValue,
                                               isFullMatch)
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
        separatorPositions = []
        contextBeforeInput = ""
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
        gboard_reset()
        // Append only letters (skip apostrophes)
        let letters = String(composition.filter { $0 != "'" })
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

    private func fillCandidatePage() {
        let maxCount = Self.candidatePageSize + 1
        var bufs = [UnsafeMutablePointer<CChar>?](repeating: nil, count: maxCount)
        let offset = candidatePage * Self.candidatePageSize
        let count = Int(gboard_get_candidates_page(&bufs, Int32(offset), Int32(maxCount)))

        var results: [String] = []
        for i in 0..<count {
            if let ptr = bufs[i] {
                results.append(String(cString: ptr))
            }
        }

        hasNextPage = results.count > Self.candidatePageSize
        candidates = Array(results.prefix(Self.candidatePageSize))
        selectedIndex = 0
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
