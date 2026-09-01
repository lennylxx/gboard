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
        // no-op for testing
    }
    func sessionHideCandidates() {
        // no-op for testing
    }

    func clear() {
        committedText = ""
        markedText = nil
    }
}

// ── Helper: create a fresh session with mock delegate ────────────────────────

func makeSession() -> (PinyinSession, MockDelegate) {
    let session = PinyinSession()
    let mock = MockDelegate()
    session.delegate = mock
    return (session, mock)
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
        testBruteForceTypingSessions()
        testDeleteAndRetype()
        testMixedSession()
        testCommitAndPassThrough()
        testRandomKeystrokes()
        testBruteForceInitials()
        testChinesePunctuation()
        testPunctuationWhileComposing()
        testContextLanguageScoring()

        print("\n══════════════════════════════════")
        print("Results: \(gPass) passed, \(gFail) failed")
        print("══════════════════════════════════")
        exit(gFail > 0 ? 1 : 0)
    }
}
