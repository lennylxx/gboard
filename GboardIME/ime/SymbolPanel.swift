// SymbolPanel — the ?123 symbol picker, layered over PinyinSession.
//
// Gboard implements ?123 as a separate keyboard beside the Pinyin one
// rather than a mode inside the decoder. The panel mirrors that: it sits
// between the session and the UI as its delegate, leaves the composition
// untouched, and while open holds back the session's candidate updates so
// they can be restored when the panel closes.

import Foundation

enum SymbolPanelKey: Equatable {
    case escape
    case returnKey
    case delete
    case left
    case right
    case previousPage
    case nextPage
    case space
    case number(Int)
    case other

    /// Maps a key-down. `plain` means no modifiers other than Caps Lock.
    init(keyCode: UInt16, characters: String, plain: Bool) {
        switch keyCode {
        case 53: self = .escape
        case 36, 76: self = .returnKey
        case 51: self = .delete
        case 123 where plain: self = .left
        case 124 where plain: self = .right
        case 49: self = .space
        case 27 where plain: self = .previousPage
        case 24 where plain: self = .nextPage
        default:
            if plain, let n = Int(characters), (1...9).contains(n) {
                self = .number(n)
            } else {
                self = .other
            }
        }
    }
}

final class SymbolPanel: PinyinSessionDelegate {
    /// The original hand-picked pages merged with the tap symbols of
    /// Gboard's Chinese ?123 keyboard (resources/res/sla.xml) and some of
    /// its long-press alternates. —— and …… follow this IME's punctuation
    /// mapping rather than Gboard's single … key.
    static let pages: [[String]] = [
        ["，", "。", "、", "？", "！", "；", "：", "\u{201C}", "\u{201D}"],
        ["《", "》", "【", "】", "（", "）", "「", "」"],
        ["\u{2018}", "\u{2019}", "『", "』", "——", "……", "～", "·"],
        ["＋", "－", "×", "÷", "＝", "％", "＆", "＊"],
        ["／", "＃", "＠", "＿", "｀", "｛", "｜", "｝"],
        ["￥", "＄", "€", "£", "¢", "§", "©", "®", "™"],
    ]

    /// Marked text that keeps the client composing while the panel is open
    /// without pinyin; the controller renders it as a zero-width space.
    static let placeholder = "?123"

    weak var delegate: PinyinSessionDelegate?
    private let session: PinyinSession

    private(set) var isOpen = false
    private(set) var pageIndex = 0
    private(set) var selectedIndex = 0

    private struct CandidateUpdate {
        let candidates: [String]
        let pinyin: PinyinReading
        let selectedIndex: Int
        let canGoPrevious: Bool
        let canGoNext: Bool
    }

    /// The session's latest candidate update; nil when hidden.
    private var sessionCandidates: CandidateUpdate?

    init(session: PinyinSession) {
        self.session = session
        session.delegate = self
    }

    var title: String { "\(Self.placeholder) (\(pageIndex + 1)/\(Self.pages.count))" }

    // MARK: - Opening and closing

    func toggle() {
        if isOpen {
            close()
        } else {
            open()
        }
    }

    func open() {
        guard !isOpen else { return }
        session.dismissPredictions()
        isOpen = true
        pageIndex = 0
        selectedIndex = 0
        if !session.isComposing {
            delegate?.sessionSetMarkedText(Self.placeholder)
        }
        render()
    }

    /// Returns to Pinyin, restoring the composition's candidates.
    func close() {
        guard isOpen else { return }
        isOpen = false
        guard session.isComposing, let update = sessionCandidates else {
            if !session.isComposing {
                delegate?.sessionSetMarkedText("")
            }
            delegate?.sessionHideCandidates()
            return
        }
        delegate?.sessionShowCandidates(
            update.candidates,
            pinyin: update.pinyin,
            selectedIndex: update.selectedIndex,
            canGoPrevious: update.canGoPrevious,
            canGoNext: update.canGoNext
        )
    }

    /// Closes without restoring candidates, before the caller ends or
    /// discards the composition at a boundary such as a client switch.
    func dismiss() {
        guard isOpen else { return }
        isOpen = false
        if !session.isComposing {
            delegate?.sessionSetMarkedText("")
            delegate?.sessionHideCandidates()
        }
    }

    // MARK: - Keys

    /// Returns false when the key closed the panel and still needs normal
    /// handling, such as a letter continuing the pinyin.
    func handle(_ key: SymbolPanelKey) -> Bool {
        guard isOpen else { return false }
        switch key {
        case .escape, .returnKey, .delete:
            close()
        case .left:
            moveSelection(by: -1)
        case .right:
            moveSelection(by: 1)
        case .previousPage:
            previousPage()
        case .nextPage:
            nextPage()
        case .space:
            select(index: selectedIndex)
        case .number(let n):
            select(index: n - 1)
        case .other:
            close()
            return false
        }
        return true
    }

    /// Commits the pending composition through its candidates, then the
    /// symbol, and closes the panel.
    func select(index: Int) {
        guard isOpen, Self.pages[pageIndex].indices.contains(index) else { return }
        let symbol = Self.pages[pageIndex][index]
        // Still open, so the session's interim candidate updates stay hidden.
        _ = session.commitAndPassThrough()
        isOpen = false
        delegate?.sessionInsertText(symbol)
        delegate?.sessionSetMarkedText("")
        delegate?.sessionHideCandidates()
    }

    func previousPage() {
        guard isOpen, pageIndex > 0 else { return }
        pageIndex -= 1
        selectedIndex = 0
        render()
    }

    func nextPage() {
        guard isOpen, pageIndex < Self.pages.count - 1 else { return }
        pageIndex += 1
        selectedIndex = 0
        render()
    }

    private func moveSelection(by offset: Int) {
        let target = selectedIndex + offset
        if Self.pages[pageIndex].indices.contains(target) {
            selectedIndex = target
        } else if offset < 0 && pageIndex > 0 {
            pageIndex -= 1
            selectedIndex = Self.pages[pageIndex].count - 1
        } else if offset > 0 && pageIndex < Self.pages.count - 1 {
            pageIndex += 1
            selectedIndex = 0
        } else {
            return
        }
        render()
    }

    private func render() {
        delegate?.sessionShowCandidates(
            Self.pages[pageIndex],
            pinyin: .plain(title),
            selectedIndex: selectedIndex,
            canGoPrevious: pageIndex > 0,
            canGoNext: pageIndex < Self.pages.count - 1
        )
    }

    // MARK: - PinyinSessionDelegate

    func sessionContextBeforeInput() -> String {
        delegate?.sessionContextBeforeInput() ?? ""
    }

    func sessionInsertText(_ text: String) {
        delegate?.sessionInsertText(text)
    }

    func sessionSetMarkedText(_ text: String) {
        delegate?.sessionSetMarkedText(text)
    }

    func sessionShowCandidates(
        _ candidates: [String],
        pinyin: PinyinReading,
        selectedIndex: Int,
        canGoPrevious: Bool,
        canGoNext: Bool
    ) {
        sessionCandidates = CandidateUpdate(
            candidates: candidates,
            pinyin: pinyin,
            selectedIndex: selectedIndex,
            canGoPrevious: canGoPrevious,
            canGoNext: canGoNext
        )
        guard !isOpen else { return }
        delegate?.sessionShowCandidates(
            candidates,
            pinyin: pinyin,
            selectedIndex: selectedIndex,
            canGoPrevious: canGoPrevious,
            canGoNext: canGoNext
        )
    }

    func sessionHideCandidates() {
        sessionCandidates = nil
        guard !isOpen else { return }
        delegate?.sessionHideCandidates()
    }
}
