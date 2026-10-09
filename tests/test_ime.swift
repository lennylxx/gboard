// IME-level integration test.
// Tests the real PinyinSession class (same code InputController uses)
// with a mock delegate that records insertText / setMarkedText calls.

import Foundation

// ── Bridge to C engine ───────────────────────────────────────────────────────

@_silgen_name("gboard_init")
func gboard_init(_ so: UnsafePointer<CChar>, _ pack: UnsafePointer<CChar>) -> Bool
@_silgen_name("gboard_append")
func gboard_append(_ pinyin: UnsafePointer<CChar>) -> Bool
@_silgen_name("gboard_get_candidates")
func gboard_get_candidates(_ out: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>, _ max: Int32) -> Int32
@_silgen_name("gboard_select")
@discardableResult func gboard_select(_ index: Int32) -> Bool
@_silgen_name("gboard_get_candidate_consumed")
func gboard_get_candidate_consumed(_ index: Int32) -> Int32
@_silgen_name("gboard_reset")
func gboard_reset()

// ── Mock delegate: captures what InputController would send to the text field ─

class MockDelegate: PinyinSessionDelegate {
    var committedText = ""
    var markedText: String? = nil
    var candidatePinyin: String? = nil
    var shownCandidates: [String] = []
    var selectedIndex = -1
    var contextBeforeInput = ""
    var contextRequestCount = 0

    func sessionContextBeforeInput() -> String {
        contextRequestCount += 1
        return contextBeforeInput
    }

    func sessionInsertText(_ text: String) {
        committedText += text
    }
    func sessionSetMarkedText(_ text: String) {
        markedText = text.isEmpty ? nil : text
    }
    func sessionShowCandidates(
        _ candidates: [String],
        pinyin: String,
        selectedIndex: Int,
        canGoPrevious: Bool,
        canGoNext: Bool
    ) {
        candidatePinyin = pinyin
        shownCandidates = candidates
        self.selectedIndex = selectedIndex
    }
    func sessionHideCandidates() {
        shownCandidates = []
    }

    func clear() {
        committedText = ""
        markedText = nil
        candidatePinyin = nil
    }
}

// ── Helper: create a fresh session with mock delegate ────────────────────────

func makeSession() -> (PinyinSession, MockDelegate) {
    let session = PinyinSession()
    let mock = MockDelegate()
    session.delegate = mock
    return (session, mock)
}

func substringProvider(
    _ source: NSString,
    maximumLength: Int? = nil
) -> (NSRange) -> NSAttributedString? {
    { range in
        guard range.location >= 0,
              range.length >= 0,
              NSMaxRange(range) <= source.length,
              maximumLength.map({ range.length <= $0 }) ?? true else {
            return nil
        }
        return NSAttributedString(string: source.substring(with: range))
    }
}

func testSessionContextRetriever() {
    print("[test_session_context_retriever]")

    let chinese = "我对这里很" as NSString
    let chineseContext = SessionContextRetriever.retrieve(
        selection: NSRange(location: chinese.length, length: 0),
        attributedSubstring: substringProvider(chinese)
    )
    check("returns all available Chinese context",
          chineseContext == "我对这里很")

    let limited = "abcdefghijklmnopqrstuvw" as NSString
    var requestedLengths: [Int] = []
    let limitedProvider = substringProvider(limited, maximumLength: 7)
    let limitedContext = SessionContextRetriever.retrieve(
        selection: NSRange(location: limited.length, length: 0)
    ) { range in
        requestedLengths.append(range.length)
        return limitedProvider(range)
    }
    check("returns longest client-permitted suffix",
          limitedContext == "qrstuvw")
    check("retries from 21 down to permitted length",
          requestedLengths == Array(stride(from: 21, through: 7, by: -1)))

    let surrogateSource = "😀abcdefghijklmnopqrs" as NSString
    let surrogateContext = SessionContextRetriever.retrieve(
        selection: NSRange(location: surrogateSource.length, length: 0),
        attributedSubstring: substringProvider(surrogateSource)
    )
    check("drops a leading split surrogate",
          surrogateContext == "abcdefghijklmnopqrs")
    check("returned context stays within 20 UTF-16 units",
          surrogateContext.map { ($0 as NSString).length <= 20 } == true)
    check("returned context has no replacement character",
          surrogateContext.map { !$0.contains("\u{fffd}") } == true)

    var requestCount = 0
    let unavailable: (NSRange) -> NSAttributedString? = { _ in
        requestCount += 1
        return nil
    }
    let notFound = SessionContextRetriever.retrieve(
        selection: NSRange(location: NSNotFound, length: 0),
        attributedSubstring: unavailable
    )
    let cursorStart = SessionContextRetriever.retrieve(
        selection: NSRange(location: 0, length: 0),
        attributedSubstring: unavailable
    )
    check("NSNotFound reports unavailable context", notFound == nil)
    check("cursor zero returns known empty context", cursorStart == "")
    check("invalid selections do not request substrings", requestCount == 0)

    let refused = SessionContextRetriever.retrieve(
        selection: NSRange(location: 4, length: 0),
        attributedSubstring: unavailable
    )
    check("unavailable substring reports unavailable context", refused == nil)
    check("all shorter ranges are attempted", requestCount == 4)

    let snapshotSource = "beforeSELECTafter" as NSString
    let snapshot = SessionContextRetriever.retrieveInputContext(
        selection: NSRange(location: 6, length: 6),
        attributedSubstring: substringProvider(snapshotSource)
    )
    check("snapshot includes text before selection",
          snapshot?.beforeSelection == "before")
    check("snapshot includes selected text",
          snapshot?.selectedText == "SELECT")
    check("snapshot includes text after selection",
          snapshot?.afterSelection == "after")

    let longSnapshotSource =
        ("0123456789" + String(repeating: "a", count: 50) +
         String(repeating: "b", count: 50) + "0123456789") as NSString
    let longSnapshot = SessionContextRetriever.retrieveInputContext(
        selection: NSRange(location: 60, length: 0),
        attributedSubstring: substringProvider(longSnapshotSource)
    )
    check("snapshot limits text before selection to 50 UTF-16 units",
          longSnapshot?.beforeSelection == String(repeating: "a", count: 50))
    check("snapshot limits text after selection to 50 UTF-16 units",
          longSnapshot?.afterSelection == String(repeating: "b", count: 50))
}

func testSessionContextFallback() {
    print("[test_session_context_fallback]")
    var tracker = SessionContextTracker()

    tracker.recordInsertedText("我对这里很")
    check("uses tracked commits when client context is unavailable",
          tracker.resolve(clientContext: nil) == "我对这里很")
    check("client context overrides tracked commits",
          tracker.resolve(clientContext: "别的前文") == "别的前文")
    check("updated client context becomes the next fallback",
          tracker.resolve(clientContext: nil) == "别的前文")

    _ = tracker.resolve(clientContext: "")
    check("known cursor start clears stale fallback",
          tracker.resolve(clientContext: nil).isEmpty)

    tracker.recordInsertedText("😀abcdefghijklmnopqrs")
    check("tracked context does not split surrogate pairs",
          tracker.fallbackContext == "abcdefghijklmnopqrs")

    tracker.invalidate()
    check("invalidation clears tracked context", tracker.fallbackContext.isEmpty)
}

// ── Test harness ─────────────────────────────────────────────────────────────

var gPass = 0
var gFail = 0

func check(_ name: String, _ cond: Bool) {
    if cond {
        gPass += 1
        print("  PASS: \(name)")
    } else {
        gFail += 1
        print("  FAIL: \(name)")
    }
}

// ── Tests ────────────────────────────────────────────────────────────────────

func testBasicTyping() {
    print("[test_basic_typing]")
    let (s, m) = makeSession()

    for ch in "nihao" { _ = s.appendLetter(String(ch)) }
    check("'nihao' has candidates", !s.candidates.isEmpty)
    check("'nihao' first candidate is 你好", s.candidates.first == "你好")
    _ = s.selectCurrent()
    check("space commits 你好", m.committedText == "你好")
    check("composition cleared", s.composition.isEmpty)
    check("candidates cleared", s.candidates.isEmpty)
}

func testNumberSelection() {
    print("[test_number_selection]")
    let (s, m) = makeSession()

    for ch in "nihao" { _ = s.appendLetter(String(ch)) }
    let cands = s.candidates
    check("has candidates", cands.count > 1)

    let second = cands[1]
    _ = s.selectNumber(2)
    check("number 2 commits second candidate", m.committedText == second)
}

func testDelete() {
    print("[test_delete]")
    let (s, _) = makeSession()

    for ch in "nihao" { _ = s.appendLetter(String(ch)) }
    check("composition is 'nihao'", s.composition == "nihao")

    _ = s.deleteBack()
    check("after delete: 'niha'", s.composition == "niha")
    check("still has candidates", !s.candidates.isEmpty)

    _ = s.deleteBack(); _ = s.deleteBack(); _ = s.deleteBack(); _ = s.deleteBack()
    check("empty after all deletes", s.composition.isEmpty)
    check("candidates empty", s.candidates.isEmpty)
}

func testEscape() {
    print("[test_escape]")
    let (s, m) = makeSession()

    for ch in "zhongwen" { _ = s.appendLetter(String(ch)) }
    check("composing", s.isComposing)
    s.cancel()
    check("cancel clears composition", s.composition.isEmpty)
    check("cancel clears candidates", s.candidates.isEmpty)
    check("cancel commits nothing", m.committedText.isEmpty)
}

func testArrowKeys() {
    print("[test_arrow_keys]")
    let (s, m) = makeSession()

    for ch in "nihao" { _ = s.appendLetter(String(ch)) }
    check("selectedIndex starts at 0", s.selectedIndex == 0)

    _ = s.moveRight()
    check("right → 1", s.selectedIndex == 1)
    _ = s.moveRight()
    check("right → 2", s.selectedIndex == 2)
    _ = s.moveLeft()
    check("left → 1", s.selectedIndex == 1)

    _ = s.moveLeft()
    check("at 0", s.selectedIndex == 0)
    _ = s.moveLeft()
    check("left at 0 stays 0", s.selectedIndex == 0)

    // Select at non-zero index
    _ = s.moveRight()
    let cand1 = s.candidates[1]
    _ = s.selectCurrent()
    check("space selects highlighted", m.committedText == cand1)
}

func testCandidatePaging() {
    print("[test_candidate_paging]")
    let (s, m) = makeSession()

    for ch in "shi" { _ = s.appendLetter(String(ch)) }
    check("first page has candidates", !s.candidates.isEmpty)
    let firstPage = s.candidates

    _ = s.nextPage()
    check("= advances candidate page", s.candidatePage == 1)
    check("next page has candidates", !s.candidates.isEmpty)
    check("next page differs", s.candidates != firstPage)
    let secondPageFirst = s.candidates[0]

    _ = s.previousPage()
    check("- returns to first page", s.candidatePage == 0)
    check("first page restored", s.candidates == firstPage)

    _ = s.nextPage()
    _ = s.selectNumber(1)
    check("number selects candidate from current page", m.committedText == secondPageFirst)
}

func testEnterCommits() {
    print("[test_enter_commits]")
    let (s, m) = makeSession()

    for ch in "nihao" { _ = s.appendLetter(String(ch)) }
    _ = s.commitRawPinyin()
    check("enter commits raw pinyin", m.committedText == "nihao")
    check("composition cleared", !s.isComposing)
}

func testRemainingComposition() {
    print("[test_remaining_composition]")
    let (s, m) = makeSession()

    for ch in "nihaoma" { _ = s.appendLetter(String(ch)) }
    check("composition is 'nihaoma'", s.composition == "nihaoma")
    check("has candidates", !s.candidates.isEmpty)

    let firstCand = s.candidates[0]
    print("    first candidate: '\(firstCand)'")
    _ = s.selectCurrent()

    let committed = m.committedText
    print("    committed: '\(committed)' remaining: '\(s.composition)'")

    if s.composition.isEmpty {
        check("all consumed — committed non-empty", !committed.isEmpty)
    } else {
        check("remaining has candidates", !s.candidates.isEmpty)
        _ = s.selectCurrent()
        check("second select commits more", m.committedText.count > committed.count)
    }
}

func testSelectAndContinue(_ label: String, _ input: String) {
    print("[test_select_continue_\(label)]")
    let (s, m) = makeSession()

    for ch in input { _ = s.appendLetter(String(ch)) }
    check("composition is '\(input)'", s.composition == input)

    var rounds = 0
    while s.isComposing && !s.candidates.isEmpty && rounds < 10 {
        rounds += 1
        let cand = s.candidates[0]
        let comp = s.composition
        _ = s.selectCurrent()
        print("    round \(rounds): '\(comp)' → '\(cand)' → remaining='\(s.composition)'")
    }
    check("fully committed", s.composition.isEmpty)
    check("non-empty output", !m.committedText.isEmpty)
    print("    output: '\(m.committedText)'")
}

func selectCandidate(_ text: String, in session: PinyinSession) -> Bool {
    while true {
        if let index = session.candidates.firstIndex(of: text) {
            _ = session.selectCandidate(index: index)
            return true
        }
        guard session.hasNextPage else { return false }
        _ = session.nextPage()
    }
}

func candidatePosition(_ text: String, in session: PinyinSession) -> Int? {
    while true {
        if let index = session.candidates.firstIndex(of: text) {
            return session.candidatePage * 9 + index
        }
        guard session.hasNextPage else { return nil }
        _ = session.nextPage()
    }
}

func testSegmentedUserDictionaryLearning() {
    print("[test_segmented_user_dictionary_learning]")

    let (beforeSession, _) = makeSession()
    for ch in "ceshi" { _ = beforeSession.appendLetter(String(ch)) }
    let positionBefore = candidatePosition("侧试", in: beforeSession)
    beforeSession.cancel()

    var allSelectionsSucceeded = true
    for _ in 0..<20 {
        let (session, delegate) = makeSession()
        for ch in "ceshi" { _ = session.appendLetter(String(ch)) }

        if !selectCandidate("侧", in: session) ||
           session.composition != "shi" ||
           !selectCandidate("试", in: session) ||
           delegate.committedText != "侧试" {
            allSelectionsSucceeded = false
            break
        }
    }
    check("segmented phrase selections complete", allSelectionsSucceeded)

    let (session, _) = makeSession()
    for ch in "ceshi" { _ = session.appendLetter(String(ch)) }
    let positionAfter = candidatePosition("侧试", in: session)
    print("    '侧试' position before: \(positionBefore.map(String.init) ?? "none"), after: \(positionAfter.map(String.init) ?? "none")")
    check("segmented phrase is learned as a full candidate",
          positionAfter != nil &&
          (positionBefore == nil || positionAfter! < positionBefore!))
    session.cancel()
}

// ── Brute force: 50 phrases typed and committed ──────────────────────────────

func testBruteForceTypingSessions() {
    print("[test_brute_force_typing_sessions]")

    let phrases = [
        "nihao", "xiexie", "zaijian", "duibuqi", "meiguanxi",
        "zhongguo", "meiguo", "beijing", "shanghai", "guangzhou",
        "dianhua", "diannao", "shouji", "yinyue", "dianying",
        "xuesheng", "laoshi", "tongxue", "pengyou", "jiaren",
        "chifan", "shuijiao", "shangban", "xiaban", "huijia",
        "zuotian", "jintian", "mingtian", "xianzai", "yihou",
        "feichang", "feichanghao", "taihaole", "meiwenti",
        "qingwen", "xingming", "dizhi", "dianhuahaoma",
        "womenshipengyou", "zhongwenshurufa", "rengongzhineng",
        "jiqixuexi", "shenduxuexi", "ziranyuyan",
        "woxihuanzhongguo", "jintiantianqihenhao",
        "woshizhongguoren", "nihaoshijie",
        "jintianwomenquchifan", "mingtianyouyigehuiyi",
    ]

    var ok = 0
    for phrase in phrases {
        let (s, m) = makeSession()
        for ch in phrase { _ = s.appendLetter(String(ch)) }

        var rounds = 0
        while s.isComposing && !s.candidates.isEmpty && rounds < 20 {
            rounds += 1
            _ = s.selectCurrent()
        }

        if s.isComposing {
            print("    STUCK: '\(phrase)' → remaining='\(s.composition)' output='\(m.committedText)'")
        } else {
            ok += 1
            print("    OK: '\(phrase)' → '\(m.committedText)' (\(rounds) rounds)")
        }
    }
    print("    \(ok)/\(phrases.count) phrases fully committed")
    check("all phrases complete", ok == phrases.count)
}

// ── Delete + retype cycles ───────────────────────────────────────────────────

func testDeleteAndRetype() {
    print("[test_delete_and_retype]")
    let (s, m) = makeSession()

    for ch in "nihao" { _ = s.appendLetter(String(ch)) }
    check("composition is nihao", s.composition == "nihao")

    _ = s.deleteBack() // niha
    _ = s.deleteBack() // nih
    check("after 2 deletes: 'nih'", s.composition == "nih")
    check("still has candidates", !s.candidates.isEmpty)

    for ch in "eng" { _ = s.appendLetter(String(ch)) }
    check("retyped to 'niheng'", s.composition == "niheng")
    check("has candidates", !s.candidates.isEmpty)

    _ = s.selectCurrent()
    check("committed something", !m.committedText.isEmpty)
    print("    'niheng' → '\(m.committedText)'")
}

// ── Mixed sessions ───────────────────────────────────────────────────────────

func testMixedSession() {
    print("[test_mixed_session]")
    let (s, m) = makeSession()

    // Session 1
    for ch in "nihao" { _ = s.appendLetter(String(ch)) }
    _ = s.selectCurrent()
    let first = m.committedText
    check("first commit non-empty", !first.isEmpty)

    // Session 2
    for ch in "shijie" { _ = s.appendLetter(String(ch)) }
    _ = s.selectCurrent()
    check("second commit appended", m.committedText.count > first.count)
    let second = m.committedText

    // Session 3: escape then retype
    for ch in "cuowu" { _ = s.appendLetter(String(ch)) }
    s.cancel()
    check("escape didn't add to committed", m.committedText == second)

    for ch in "zhengque" { _ = s.appendLetter(String(ch)) }
    _ = s.selectCurrent()
    check("third commit appended", m.committedText.count > second.count)
    print("    final: '\(m.committedText)'")
}

// ── commitAndPassThrough (non-letter while composing) ────────────────────────

func testCommitAndPassThrough() {
    print("[test_commit_and_pass_through]")
    let (s, m) = makeSession()

    for ch in "nihao" { _ = s.appendLetter(String(ch)) }
    check("composing", s.isComposing)

    let result = s.commitAndPassThrough()
    check("returns commitAndPass", result == .commitAndPass)
    check("committed text", !m.committedText.isEmpty)
    check("composition cleared", s.composition.isEmpty)
}

// ── Random keystroke sequences ───────────────────────────────────────────────

func testRandomKeystrokes() {
    print("[test_random_keystrokes]")

    var seed: UInt32 = 54321
    func nextRand() -> UInt32 {
        seed = seed &* 1103515245 &+ 12345
        return (seed >> 16) & 0x7FFF
    }

    let letters = Array("abcdefghijklmnopqrstuvwxyz")
    var issues = 0
    let sessions = 100

    for _ in 0..<sessions {
        let (s, _) = makeSession()
        let numKeys = 5 + Int(nextRand() % 30)

        for _ in 0..<numKeys {
            let action = nextRand() % 10
            switch action {
            case 0...6:
                let ch = letters[Int(nextRand()) % letters.count]
                _ = s.appendLetter(String(ch))
            case 7:
                _ = s.selectCurrent()
            case 8:
                _ = s.deleteBack()
            case 9:
                _ = s.selectNumber(1 + Int(nextRand() % 3))
            default:
                break
            }
        }

        if s.isComposing { s.cancel() }
        if s.isComposing { issues += 1 }
    }
    print("    \(sessions) random sessions, \(issues) issues")
    check("no issues", issues == 0)
}

// ── Brute force: every initial + vowel combo ─────────────────────────────────

func testBruteForceInitials() {
    print("[test_brute_force_initials]")
    let inputs = [
        "a","o","e","ai","an","ba","bo","bi","bu","ca","ce","ci","cu",
        "da","de","di","du","fa","fo","fu","ga","ge","gu","ha","he","hu",
        "ji","ju","ka","ke","ku","la","le","li","lu","lv","ma","me","mi","mu",
        "na","ne","ni","nu","nv","pa","po","pi","pu","qi","qu","re","ri","ru",
        "sa","se","si","su","sha","she","shi","shu","ta","te","ti","tu",
        "wa","wo","wu","xi","xu","ya","ye","yi","yu","za","ze","zi","zu",
        "zha","zhe","zhi","zhu","cha","che","chi","chu",
    ]

    var ok = 0
    for input in inputs {
        let (s, _) = makeSession()
        for ch in input { _ = s.appendLetter(String(ch)) }
        if !s.candidates.isEmpty { ok += 1 }
        else { print("    WARN: '\(input)' no candidates") }
    }
    print("    \(ok)/\(inputs.count) returned candidates")
    check("all initials return candidates", ok == inputs.count)
}

// ── Chinese punctuation ─────────────────────────────────────────────────────

func testChinesePunctuation() {
    print("[test_chinese_punctuation]")

    // Basic punctuation mapping (not composing)
    let (s, d) = makeSession()
    _ = s.handlePunctuation(",")
    check("comma → ，", d.committedText == "，")
    d.clear()

    _ = s.handlePunctuation(".")
    check("period → 。", d.committedText == "。")
    d.clear()

    _ = s.handlePunctuation("?")
    check("question → ？", d.committedText == "？")
    d.clear()

    _ = s.handlePunctuation("!")
    check("exclamation → ！", d.committedText == "！")
    d.clear()

    _ = s.handlePunctuation(";")
    check("semicolon → ；", d.committedText == "；")
    d.clear()

    _ = s.handlePunctuation(":")
    check("colon → ：", d.committedText == "：")
    d.clear()

    // Paired quotes toggle
    _ = s.handlePunctuation("\"")
    check("first double-quote → \u{201C}", d.committedText == "\u{201C}")
    d.clear()
    _ = s.handlePunctuation("\"")
    check("second double-quote → \u{201D}", d.committedText == "\u{201D}")
    d.clear()

    // isPunctuation static check
    check("comma is punctuation", PinyinSession.isPunctuation(","))
    check("a is not punctuation", !PinyinSession.isPunctuation("a"))

    // Unmapped character returns passThrough
    let r = s.handlePunctuation("@")
    check("@ is passThrough", r == .passThrough)
}

func testPunctuationWhileComposing() {
    print("[test_punctuation_while_composing]")
    let (s, d) = makeSession()

    // Type some pinyin first
    for ch in "ni" { _ = s.appendLetter(String(ch)) }
    check("composing 'ni'", s.isComposing)
    check("has candidates", !s.candidates.isEmpty)

    // Punctuation should commit first candidate then insert Chinese punct
    let firstCandidate = s.candidates[0]
    _ = s.handlePunctuation(",")
    check("committed candidate + comma", d.committedText == firstCandidate + "，")
    check("composition cleared", !s.isComposing)
}

func testVisualPinyinSegmentation() {
    print("[test_visual_pinyin_segmentation]")
    let (s, d) = makeSession()

    for ch in "fangan" { _ = s.appendLetter(String(ch)) }
    check("engine split fangan as fang'an",
          s.segmentedPinyin == "fang'an")
    check("candidate window receives fang'an",
          d.candidatePinyin == "fang'an")

    s.cancel()
    for ch in "xi" { _ = s.appendLetter(String(ch)) }
    _ = s.handlePunctuation("'")
    for ch in "an" { _ = s.appendLetter(String(ch)) }
    check("explicit separator displays xi'an",
          s.segmentedPinyin == "xi'an")
    check("candidate window receives xi'an",
          d.candidatePinyin == "xi'an")
}

func testContextLanguageScoring() {
    print("[test_context_language_scoring]")
    let (s, d) = makeSession()
    d.contextBeforeInput = "我对这里很"

    _ = s.appendLetter("b")
    d.contextBeforeInput = "准备开始"
    for ch in "ushu" { _ = s.appendLetter(String(ch)) }

    check("session context reranks bushu to 不熟",
          s.candidates.first == "不熟")
    check("session captures context once per composition",
          d.contextRequestCount == 1)

    s.cancel()
    d.contextBeforeInput = "准备开始"
    for ch in "bushu" { _ = s.appendLetter(String(ch)) }
    check("new composition refreshes context",
          d.contextRequestCount == 2)
    check("deployment context keeps 部署 first",
          s.candidates.first == "部署")
    s.cancel()
}

func testContinuousPhraseLanguageScoring() {
    print("[test_continuous_phrase_language_scoring]")
    let (s, _) = makeSession()

    for ch in "woduizhelihenbushu" {
        _ = s.appendLetter(String(ch))
    }

    check("continuous phrase prefers 我对这里很不熟",
          s.candidates.first == "我对这里很不熟")
    if let first = s.candidates.first, first != "我对这里很不熟" {
        print("    actual first candidate: \(first)")
    }
    s.cancel()
}

func testNextWordPrediction() {
    print("[test_next_word_prediction]")
    let (s, d) = makeSession()

    for ch in "nihao" { _ = s.appendLetter(String(ch)) }
    _ = s.selectCurrent()
    check("commit shows predictions", s.isPredicting)
    check("predictions reach the window",
          !d.shownCandidates.isEmpty && d.shownCandidates == s.visiblePredictions)
    check("prediction window has no pinyin strip", d.candidatePinyin == "")
    check("composition candidates stay empty", s.candidates.isEmpty)

    let first = s.predictions.first ?? ""
    let result = s.selectNumber(1)
    check("number selects prediction", result == .handled)
    check("prediction is committed", d.committedText == "你好" + first)
    print("    你好 → \(first) → \(s.predictions.prefix(5))")

    _ = s.appendLetter("w")
    check("typing dismisses predictions", !s.isPredicting)
    check("typing starts a composition", s.composition == "w")
    s.cancel()

    d.clear()
    d.contextBeforeInput = ""
    for ch in "kuai" { _ = s.appendLetter(String(ch)) }
    _ = s.selectCurrent()
    let bare = s.predictions
    s.dismissPredictions()
    d.clear()
    d.contextBeforeInput = "生日"
    for ch in "kuai" { _ = s.appendLetter(String(ch)) }
    let picked = s.candidates.first ?? ""
    _ = s.selectCurrent()
    print("    \(picked) → \(bare.prefix(5)); 生日 + \(picked) → \(s.predictions.prefix(5))")
    check("prediction commits 快", picked == "快")
    check("prediction uses text before input",
          s.predictions.first == "乐" && s.predictions != bare)

    s.dismissPredictions()
    check("dismiss clears predictions", !s.isPredicting)
    check("dismiss hides window", d.shownCandidates.isEmpty)
    check("number passes through without predictions",
          s.selectNumber(1) == .passThrough)

    d.contextBeforeInput = ""
    for ch in "ni" { _ = s.appendLetter(String(ch)) }
    _ = s.handlePunctuation(",")
    check("punctuation leaves no predictions", !s.isPredicting)

    d.contextBeforeInput = "生日"
    for ch in "kuai" { _ = s.appendLetter(String(ch)) }
    _ = s.selectCurrent()
    check("fetches more than one page of predictions",
          s.predictions.count > 9)
    check("window shows one page", d.shownCandidates.count == 9)
    let page2 = Array(s.predictions[9..<min(18, s.predictions.count)])
    s.nextPredictionPage()
    check("next prediction page", d.shownCandidates == page2)
    let committedBefore = d.committedText
    _ = s.selectNumber(1)
    check("number selects on current page",
          d.committedText == committedBefore + page2[0])
    check("first prediction is highlighted", d.selectedIndex == 0)
    let spaceBefore = d.committedText
    let spaceExpected = s.visiblePredictions.first ?? ""
    check("space selects first prediction",
          s.selectCurrent() == .handled &&
          d.committedText == spaceBefore + spaceExpected)
    s.dismissPredictions()
    check("space passes through without predictions",
          s.selectCurrent() == .passThrough)

    for ch in "kuai" { _ = s.appendLetter(String(ch)) }
    _ = s.selectCurrent()
    for _ in 0..<10 { s.nextPredictionPage() }
    let shown = s.visiblePredictions.count
    print("    last prediction page has \(shown) of \(s.predictions.count)")
    check("last page is partial", shown > 0 && shown < 9)
    check("unavailable number passes through",
          s.selectNumber(shown + 1) == .passThrough)
    check("unavailable number dismisses",
          !s.isPredicting && d.shownCandidates.isEmpty)
    s.dismissPredictions()

    d.contextBeforeInput = ""
    for ch in "github" { _ = s.appendLetter(String(ch)) }
    _ = s.selectCurrent()
    print("    GitHub → \(s.predictions.prefix(5))")
    check("Latin commit has no predictions",
          !s.isPredicting && d.shownCandidates.isEmpty)
    s.dismissPredictions()
}

func testPredictionEdgeCases() {
    print("[test_prediction_edge_cases]")
    let (s, d) = makeSession()

    // Chaining into a context with no predictions hides the stale window.
    s.showPredictions(context: "生日快")
    check("chain: predictions shown", s.isPredicting && !d.shownCandidates.isEmpty)
    s.showPredictions(context: "生日快。")
    check("chain: empty result clears predictions", !s.isPredicting)
    check("chain: empty result hides window", d.shownCandidates.isEmpty)

    // Client switch: the tracker flags a new client and reset drops state.
    let first = NSObject(), second = NSObject()
    var tracker = SessionClientTracker()
    check("client: first client is not a switch", !tracker.update(to: first))
    check("client: same client is not a switch", !tracker.update(to: first))
    check("client: different client is a switch", tracker.update(to: second))
    tracker.clear()
    check("client: after clear is not a switch", !tracker.update(to: first))
    s.showPredictions(context: "生日快")
    if tracker.update(to: second) { s.handleClientSwitch() }
    check("client: switch drops predictions",
          !s.isPredicting && d.shownCandidates.isEmpty)
    check("client: number no longer selects",
          s.selectNumber(1) == .passThrough && d.committedText.isEmpty)
    for ch in "ni" { _ = s.appendLetter(String(ch)) }
    check("client: composing has marked text", d.markedText != nil)
    if tracker.update(to: first) { s.handleClientSwitch() }
    check("client: switch clears old marked text", d.markedText == nil)
    check("client: switch drops composition",
          !s.isComposing && d.committedText.isEmpty)

    // Boundary keys finish a composition left active by a partial pick.
    for ch in "nihao" { _ = s.appendLetter(String(ch)) }
    check("partial: picks 你", selectCandidate("你", in: s))
    check("partial: remainder composing", s.composition == "hao")
    let rest = s.candidates.first ?? ""
    _ = s.handlePunctuation(",")
    check("partial: punctuation finishes composition",
          !s.isComposing && d.committedText == "你" + rest + "，")
    check("partial: punctuation leaves no predictions", !s.isPredicting)
    d.clear()
    for ch in "nihao" { _ = s.appendLetter(String(ch)) }
    _ = selectCandidate("你", in: s)
    let rest2 = s.candidates.first ?? ""
    check("partial: pass-through key commits all",
          s.commitAndPassThrough() == .commitAndPass &&
          !s.isComposing && d.committedText == "你" + rest2)
    check("partial: pass-through leaves no predictions", !s.isPredicting)
    d.clear()

    // A separator-only remainder is not committed as a stray apostrophe.
    for ch in "woaini" { _ = s.appendLetter(String(ch)) }
    _ = s.handlePunctuation("'")
    let expected = s.candidates.first ?? ""
    _ = s.selectCurrent()
    check("remainder: commits candidate without apostrophe",
          d.committedText == expected && !d.committedText.contains("'"))
    check("remainder: composition finished", !s.isComposing)
    check("remainder: predictions follow the commit", s.isPredicting)
    s.dismissPredictions()

    // Prediction windows anchor at the caret, compositions at marked text.
    let caret = NSRect(x: 10, y: 20, width: 1, height: 16)
    let marked = NSRect(x: 30, y: 40, width: 1, height: 16)
    check("anchor: prediction uses caret",
          CandidateWindowController.anchorRect(
              isPrediction: true, caretRect: { caret },
              markedTextRect: { marked }) == caret)
    check("anchor: missing caret falls back",
          CandidateWindowController.anchorRect(
              isPrediction: true, caretRect: { nil },
              markedTextRect: { marked }) == marked)
    check("anchor: zero caret falls back",
          CandidateWindowController.anchorRect(
              isPrediction: true, caretRect: { .zero },
              markedTextRect: { marked }) == marked)
    check("anchor: composition uses marked text",
          CandidateWindowController.anchorRect(
              isPrediction: false, caretRect: { caret },
              markedTextRect: { marked }) == marked)
    let near = NSRect(x: 60, y: 40, width: 1, height: 16)
    let far = NSRect(x: 0, y: 900, width: 1, height: 16)
    check("anchor: caret near composition is used",
          CandidateWindowController.anchorRect(
              isPrediction: true, caretRect: { caret },
              markedTextRect: { marked }, previousAnchor: near) == caret)
    check("anchor: far caret falls back to composition",
          CandidateWindowController.anchorRect(
              isPrediction: true, caretRect: { far },
              markedTextRect: { marked }, previousAnchor: near) == near)
    check("anchor: missing caret prefers composition",
          CandidateWindowController.anchorRect(
              isPrediction: true, caretRect: { nil },
              markedTextRect: { marked }, previousAnchor: near) == near)
    let screens = [NSRect(x: 0, y: 0, width: 1440, height: 900),
                   NSRect(x: 1440, y: 0, width: 2560, height: 1440)]
    let other = NSRect(x: 2000, y: 900, width: 1, height: 16)
    check("anchor: caret on another screen is trusted",
          CandidateWindowController.anchorRect(
              isPrediction: true, caretRect: { other },
              markedTextRect: { marked }, previousAnchor: near,
              screenFrames: screens) == other)
    let wide = NSRect(x: 1400, y: 40, width: 1, height: 16)
    check("anchor: far caret on the same line is trusted",
          CandidateWindowController.anchorRect(
              isPrediction: true, caretRect: { wide },
              markedTextRect: { marked }, previousAnchor: near,
              screenFrames: screens) == wide)
    check("screen: point inside second screen",
          CandidateWindowController.screenIndex(for: other, in: screens) == 1)
    check("screen: offscreen point picks nearest",
          CandidateWindowController.screenIndex(
              for: NSRect(x: 5000, y: 100, width: 1, height: 1),
              in: screens) == 1)
    let size = NSSize(width: 300, height: 60)
    let o = CandidateWindowController.windowOrigin(
        size: size, below: NSRect(x: 3900, y: 300, width: 1, height: 16),
        visible: screens[1])
    check("origin: clamps to the anchor screen",
          o.x == screens[1].maxX - size.width && o.y == 300 - 60 - 5)
    let low = CandidateWindowController.windowOrigin(
        size: size, below: NSRect(x: 100, y: 10, width: 1, height: 16),
        visible: screens[0])
    check("origin: flips above near the bottom", low.y == 10 + 16 + 4)
    let below = CandidateWindowController.windowOrigin(
        size: size, below: NSRect(x: 100, y: -200, width: 1, height: 16),
        visible: screens[0])
    check("origin: flipped window stays on screen", below.y == screens[0].minY)
    let stacked = [NSRect(x: 0, y: 0, width: 1440, height: 900),
                   NSRect(x: 0, y: 900, width: 1440, height: 900)]
    check("screen: shared edge belongs to the upper screen",
          CandidateWindowController.screenIndex(
              for: NSRect(x: 100, y: 900, width: 1, height: 16),
              in: stacked) == 1)
    // A prediction chain advances the anchor, so a caret that drifts
    // line by line is never treated as bogus.
    var anchor = near
    var drifted = true
    for step in 1...6 {
        let next = NSRect(x: 60, y: 40 - CGFloat(step) * 40, width: 1, height: 16)
        let r = CandidateWindowController.anchorRect(
            isPrediction: true, caretRect: { next },
            markedTextRect: { marked }, previousAnchor: anchor,
            screenFrames: [NSRect(x: 0, y: -1000, width: 1440, height: 2000)])
        drifted = drifted && r == next
        anchor = r
    }
    check("anchor: chain follows a drifting caret", drifted)
}

// ── Main ─────────────────────────────────────────────────────────────────────

// ── Entry point ──────────────────────────────────────────────────────────────

@main struct TestIME {
    static func main() {
        let args = CommandLine.arguments
        guard args.count > 2 else {
            print("Usage: test_ime <so_path> <pack_dir>")
            exit(1)
        }

        print("[init]")
        let initOk = gboard_init(args[1], args[2])
        check("engine initializes", initOk)
        guard initOk else { print("Engine init failed."); exit(1) }

        testBasicTyping()
        testNumberSelection()
        testDelete()
        testEscape()
        testArrowKeys()
        testCandidatePaging()
        testEnterCommits()
        testRemainingComposition()
        testSelectAndContinue("nihao_shijie", "nihaoshijie")
        testSelectAndContinue("woqunijiaya", "woqunijiaya")
        testSelectAndContinue("zhongwenshuruf", "zhongwenshuruf")
        testSegmentedUserDictionaryLearning()
        testBruteForceTypingSessions()
        testDeleteAndRetype()
        testMixedSession()
        testCommitAndPassThrough()
        testRandomKeystrokes()
        testBruteForceInitials()
        testChinesePunctuation()
        testPunctuationWhileComposing()
        testVisualPinyinSegmentation()
        testSessionContextRetriever()
        testSessionContextFallback()
        testContextLanguageScoring()
        testContinuousPhraseLanguageScoring()
        testNextWordPrediction()
        testPredictionEdgeCases()

        print("\n══════════════════════════════════")
        print("Results: \(gPass) passed, \(gFail) failed")
        print("══════════════════════════════════")
        exit(gFail > 0 ? 1 : 0)
    }
}
