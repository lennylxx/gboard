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
        pinyin: PinyinReading,
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

/// Pinyin shown above the candidates. Spans the engine's pinyin corrector
/// rewrote (e.g. typed "hoa" shown as "hao") are flagged for highlighting.
struct PinyinReading: Equatable {
    struct Span: Equatable {
        let text: String
        let isCorrected: Bool
        /// A typed letter the correction drops; shown struck out, not read.
        var isTypo: Bool = false
    }

    var spans: [Span]

    static let empty = PinyinReading(spans: [])

    static func plain(_ text: String) -> PinyinReading {
        PinyinReading(spans: text.isEmpty ? [] : [Span(text: text, isCorrected: false)])
    }

    /// The corrected reading, without struck-out letters.
    var text: String { spans.filter { !$0.isTypo }.map(\.text).joined() }
    /// Text as rendered, including struck-out letters.
    var displayText: String { spans.map(\.text).joined() }
    var isEmpty: Bool { spans.allSatisfy { $0.text.isEmpty } }
    var hasCorrection: Bool { spans.contains { $0.isCorrected || $0.isTypo } }
}

class PinyinSession {
    private static let candidatePageSize = 9

    private(set) var composition = ""
    private(set) var candidates: [String] = []
    private(set) var selectedIndex = 0
    private(set) var candidatePage = 0
    private(set) var hasNextPage = false

    /// Next-word predictions shown after a commit while nothing is composing.
    private(set) var predictions: [String] = []
    private(set) var predictionPage = 0
    private var predictionContext = ""
    var isPredicting: Bool { !predictions.isEmpty }
    private static let maxPredictions = 50

    /// Predictions on the current page, selectable with 1-9.
    var visiblePredictions: [String] {
        let start = predictionPage * Self.candidatePageSize
        guard start < predictions.count else { return [] }
        let end = min(start + Self.candidatePageSize, predictions.count)
        return Array(predictions[start..<end])
    }

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
    private var pendingLearningText = ""
    private var pendingLearningTokens: [String] = []
    private var pendingLearningTypes: [Int32] = []
    private var pendingLearningValid = true

    // MARK: - Key actions (called by InputController or test harness)

    func appendLetter(_ ch: String) -> KeyResult {
        if composition.isEmpty {
            dismissPredictions()
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
        if composition.isEmpty && isPredicting {
            return selectPrediction(index: 0)
        }
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

        // Capture this segment before selection mutates the engine state.
        let tokenCount = gboard_user_dict_extract_token_count(Int32(engineIndex))
        let letterCount = composition.filter { $0 != "'" }.count
        if committedInComposition.isEmpty {
            predictionContext = contextBeforeInput
        }
        committedInComposition += text
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
            // Undecodable remainder: commit its letters rather than drop them.
            let raw = remaining.filter { $0 != "'" }
            if !raw.isEmpty {
                delegate?.sessionInsertText(raw)
                committedInComposition += raw
            }
        } else {
            commitPendingLearning()
        }
        let committedContext = predictionContext + committedInComposition
        reset()
        showPredictions(context: committedContext)
        return .handled
    }

    func selectNumber(_ n: Int) -> KeyResult {
        guard n >= 1 && n <= 9 else { return .passThrough }
        if composition.isEmpty && isPredicting {
            let result = selectPrediction(index: n - 1)
            if result == .passThrough { dismissPredictions() }
            return result
        }
        guard !composition.isEmpty else { return .passThrough }
        return selectCandidate(index: n - 1)
    }

    // MARK: - Next-word prediction

    /// Commits a prediction and chains the next one, like Gboard's
    /// AbstractHmmChineseDecodeProcessor.Z(): plain commit, no learning.
    @discardableResult
    func selectPrediction(index: Int) -> KeyResult {
        let visible = visiblePredictions
        guard composition.isEmpty, index >= 0, index < visible.count else {
            return .passThrough
        }
        let text = visible[index]
        delegate?.sessionInsertText(text)
        showPredictions(context: predictionContext + text)
        return .handled
    }

    func dismissPredictions() {
        guard isPredicting else { return }
        predictions = []
        predictionPage = 0
        predictionContext = ""
        delegate?.sessionHideCandidates()
    }

    func previousPredictionPage() {
        guard isPredicting, predictionPage > 0 else { return }
        predictionPage -= 1
        notifyPredictions()
    }

    func nextPredictionPage() {
        guard isPredicting,
              (predictionPage + 1) * Self.candidatePageSize < predictions.count
        else { return }
        predictionPage += 1
        notifyPredictions()
    }

    func showPredictions(context: String) {
        predictions = []
        predictionPage = 0
        predictionContext = context
        defer {
            if !isPredicting {
                predictionContext = ""
                delegate?.sessionHideCandidates()
            }
        }
        guard !context.isEmpty else { return }

        context.withCString { _ = gboard_set_context($0) }
        let maxCount = Self.maxPredictions
        var bufs = [UnsafeMutablePointer<CChar>?](repeating: nil, count: maxCount)
        let count = Int(gboard_get_predictions(&bufs, Int32(maxCount)))
        for i in 0..<count {
            if let ptr = bufs[i] {
                predictions.append(String(cString: ptr))
                free(ptr)
            }
        }
        if isPredicting { notifyPredictions() }
    }

    private func notifyPredictions() {
        delegate?.sessionShowCandidates(
            visiblePredictions,
            pinyin: .empty,
            selectedIndex: 0,
            canGoPrevious: predictionPage > 0,
            canGoNext: (predictionPage + 1) * Self.candidatePageSize
                < predictions.count
        )
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
        finishComposition()
        dismissPredictions()
        delegate?.sessionInsertText(punct)
        return .handled
    }

    // MARK: - Non-letter while composing

    /// Commits every remaining segment so a boundary key never leaves an
    /// active composition behind after a partial candidate pick.
    private func finishComposition() {
        var rounds = 0
        while isComposing && rounds < 32 {
            let before = composition
            _ = selectCurrent()
            if composition == before { break }
            rounds += 1
        }
        if isComposing {
            _ = commitRawPinyin()
        }
    }

    /// Drops all state owned by a client that IMKit no longer routes to,
    /// clearing its marked text first. Call while the old client is current.
    func handleClientSwitch() {
        if isComposing {
            cancel()
        } else {
            dismissPredictions()
        }
    }

    func commitAndPassThrough() -> KeyResult {
        guard !composition.isEmpty else { return .passThrough }
        finishComposition()
        dismissPredictions()
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
        separatorPositions = []
        contextBeforeInput = ""
        pendingLearningText = ""
        pendingLearningTokens = []
        pendingLearningTypes = []
        pendingLearningValid = true
        committedInComposition = ""
        predictions = []
        predictionPage = 0
        predictionContext = ""
        gboard_reset()
        delegate?.sessionHideCandidates()
    }

    /// Separator vertex positions set by user apostrophes.
    private var separatorPositions: [Int32] = []
    private var contextBeforeInput = ""
    /// Text committed from the current composition across segmented picks.
    private var committedInComposition = ""

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
            pinyinReading = .empty
            delegate?.sessionHideCandidates()
        } else {
            pinyinReading = readingForSelectedCandidate()
            delegate?.sessionShowCandidates(
                candidates,
                pinyin: pinyinReading,
                selectedIndex: selectedIndex,
                canGoPrevious: candidatePage > 0,
                canGoNext: hasNextPage
            )
        }
    }

    /// Reading shown in the candidate window for the selected candidate.
    private(set) var pinyinReading = PinyinReading.empty

    /// Uses the corrected token spellings when the engine's pinyin corrector
    /// produced the selected candidate; otherwise the plain segmentation.
    private func readingForSelectedCandidate() -> PinyinReading {
        let fallback = PinyinReading.plain(segmentedPinyin)
        guard selectedIndex < candidates.count else { return fallback }
        let engineIndex = Int32(candidatePage * Self.candidatePageSize + selectedIndex)
        let maxTokens = 64
        var tokens = [HmmTokenReading](repeating: HmmTokenReading(), count: maxTokens)
        let tokenCount = Int(gboard_get_corrected_candidate_reading(
            engineIndex, &tokens, Int32(maxTokens)))
        guard tokenCount > 0 else { return fallback }

        var spans: [PinyinReading.Span] = []
        var consumedLetters = 0
        var previousLatin = false
        for token in tokens.prefix(tokenCount) {
            let typed = Self.tokenText(token.raw)
            let corrected = Self.tokenText(token.normalized)
            let isLatin = token.language == Int32(HMM_TOKEN_LANGUAGE_LATIN)
            // English words arrive as single-letter tokens; keep them joined,
            // matching hmm_engine_get_segmented_pinyin().
            if !spans.isEmpty && !(isLatin && previousLatin) {
                spans.append(.init(text: "'", isCorrected: false))
            }
            if isLatin || corrected.lowercased() == typed.lowercased() {
                spans.append(.init(text: isLatin ? typed : corrected, isCorrected: false))
            } else {
                spans += Self.correctionSpans(typed: typed, corrected: corrected)
            }
            consumedLetters += typed.count
            previousLatin = isLatin
        }

        // Keep the engine's segmentation for letters after this candidate.
        var letters = 0
        var rest = Substring(segmentedPinyin)
        while letters < consumedLetters, let first = rest.first {
            if first != "'" { letters += 1 }
            rest = rest.dropFirst()
        }
        let remainder = rest.drop { $0 == "'" }
        if !remainder.isEmpty {
            spans.append(.init(text: "'" + remainder, isCorrected: false))
        } else if rest.hasSuffix("'") {
            spans.append(.init(text: "'", isCorrected: false))
        }
        return PinyinReading(spans: spans)
    }

    /// Marks the fewest letter edits from typed to corrected, like Sogou's
    /// "cuu̸o": extra typed letters are struck out in place and missing
    /// letters are inserted as corrections, e.g. "hoa" -> h o̶ a o.
    static func correctionSpans(typed: String, corrected: String) -> [PinyinReading.Span] {
        let a = Array(typed), b = Array(corrected)
        let lower = { (c: Character) in c.lowercased() }
        var lcs = [[Int]](repeating: [Int](repeating: 0, count: b.count + 1),
                          count: a.count + 1)
        for i in stride(from: a.count - 1, through: 0, by: -1) {
            for j in stride(from: b.count - 1, through: 0, by: -1) {
                lcs[i][j] = lower(a[i]) == lower(b[j])
                    ? lcs[i + 1][j + 1] + 1
                    : max(lcs[i + 1][j], lcs[i][j + 1])
            }
        }

        var spans: [PinyinReading.Span] = []
        func append(_ c: Character, corrected: Bool, typo: Bool) {
            if let last = spans.last, last.isCorrected == corrected, last.isTypo == typo {
                spans[spans.count - 1] = .init(text: last.text + String(c),
                                               isCorrected: corrected, isTypo: typo)
            } else {
                spans.append(.init(text: String(c), isCorrected: corrected, isTypo: typo))
            }
        }
        var i = 0, j = 0
        while i < a.count || j < b.count {
            if i < a.count, j < b.count, lower(a[i]) == lower(b[j]) {
                append(b[j], corrected: false, typo: false); i += 1; j += 1
            } else if i < a.count, j == b.count || lcs[i + 1][j] >= lcs[i][j + 1] {
                append(a[i], corrected: false, typo: true); i += 1
            } else {
                append(b[j], corrected: true, typo: false); j += 1
            }
        }
        return spans
    }

    private static func tokenText<T>(_ tuple: T) -> String {
        withUnsafeBytes(of: tuple) { bytes in
            let chars = bytes.bindMemory(to: CChar.self)
            let length = chars.firstIndex(of: 0) ?? chars.count
            return String(decoding: bytes.prefix(length), as: UTF8.self)
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
