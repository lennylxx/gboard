// PinyinSession — pure composition logic, no IMKit dependency.
// Used by GboardInputController and directly by tests.

import Foundation

/// Callback protocol for UI actions (insertText, setMarkedText, candidate display).
protocol PinyinSessionDelegate: AnyObject {
    func sessionInsertText(_ text: String)
    func sessionSetMarkedText(_ text: String)
    func sessionShowCandidates(_ candidates: [String], pinyin: String, selectedIndex: Int)
    func sessionHideCandidates()
}

/// Result of handling a key action.
enum KeyResult {
    case handled        // IME consumed the key
    case passThrough    // let the system handle it
    case commitAndPass  // committed text, then pass key through
}

class PinyinSession {
    private(set) var composition = ""
    private(set) var candidates: [String] = []
    private(set) var selectedIndex = 0

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
        composition += ch
        delegate?.sessionSetMarkedText(composition)
        fetchCandidates()
        return .handled
    }

    func deleteBack() -> KeyResult {
        guard !composition.isEmpty else { return .passThrough }
        composition.removeLast()
        if composition.isEmpty {
            cancel()
        } else {
            delegate?.sessionSetMarkedText(composition)
            fetchCandidates()
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
        let consumed = Int(gboard_get_candidate_consumed(Int32(index)))

        _ = gboard_select(Int32(index))
        delegate?.sessionInsertText(text)

        // Use engine-reported vertex count (each vertex = 1 pinyin char)
        let remaining: String
        if consumed > 0 && consumed < composition.count {
            remaining = String(composition.dropFirst(consumed))
        } else {
            remaining = ""
        }

        if !remaining.isEmpty {
            composition = remaining
            delegate?.sessionSetMarkedText(composition)
            fetchCandidates()
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
        }
        return .handled
    }

    func moveRight() -> KeyResult {
        guard !composition.isEmpty && !candidates.isEmpty else { return .passThrough }
        if selectedIndex < candidates.count - 1 {
            selectedIndex += 1
            notifyCandidates()
        }
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
    func handlePunctuation(_ ch: Character) -> KeyResult {
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

    // MARK: - Internal

    func reset() {
        composition = ""
        candidates = []
        selectedIndex = 0
        gboard_reset()
        delegate?.sessionHideCandidates()
    }

    private func fetchCandidates() {
        gboard_reset()
        guard gboard_append(composition) else {
            candidates = []
            delegate?.sessionHideCandidates()
            return
        }

        let maxCount = 9
        var bufs = [UnsafeMutablePointer<CChar>?](repeating: nil, count: maxCount)
        let count = Int(gboard_get_candidates(&bufs, Int32(maxCount)))

        var results: [String] = []
        for i in 0..<count {
            if let ptr = bufs[i] {
                results.append(String(cString: ptr))
            }
        }

        candidates = results
        selectedIndex = 0
        notifyCandidates()
    }

    private func notifyCandidates() {
        if candidates.isEmpty {
            delegate?.sessionHideCandidates()
        } else {
            delegate?.sessionShowCandidates(candidates, pinyin: composition, selectedIndex: selectedIndex)
        }
    }
}
