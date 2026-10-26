import Cocoa
import SwiftUI

func withTestPreferences(_ body: (Preferences, UserDefaults) -> Void) {
    let name = "com.lennylxx.GboardIME.tests.preferences.\(UUID().uuidString)"
    guard let defaults = UserDefaults(suiteName: name) else {
        fatalError("Unable to create isolated test preferences")
    }
    defer { defaults.removePersistentDomain(forName: name) }
    body(Preferences(defaults: defaults), defaults)
}

func testPreferencesStorage() {
    print("[test_preferences_storage]")
    withTestPreferences { preferences, defaults in
        check("preferences preserve enabled defaults",
              preferences.predictionsEnabled && preferences.contextEnabled &&
              preferences.shiftToggleEnabled && preferences.pinyinDisplayMode == .candidateWindow &&
              preferences.learningEnabled)
        check("preferences preserve page and correction defaults",
              preferences.candidatePageSize == .nine && preferences.correctionStyle == .edits &&
              preferences.correctionAlignment == .lcs)
        check("user dictionary keeps the four-hour default",
              preferences.userDictionarySaveInterval == .fourHours)
        check("fuzzy pinyin defaults to disabled",
              preferences.fuzzyPinyinGroups.isEmpty && preferences.fuzzyPinyinMask == 0)

        var notifications = 0
        let observer = NotificationCenter.default.addObserver(
            forName: Preferences.didChangeNotification, object: preferences, queue: nil
        ) { _ in notifications += 1 }
        defer { NotificationCenter.default.removeObserver(observer) }
        preferences.predictionsEnabled = false
        preferences.contextEnabled = false
        preferences.shiftToggleEnabled = false
        preferences.correctionStyle = .color
        preferences.correctionAlignment = .dl
        preferences.candidatePageSize = .five
        preferences.pinyinDisplayMode = .hidden
        preferences.learningEnabled = false
        preferences.userDictionarySaveInterval = .fifteenMinutes
        preferences.fuzzyPinyinGroups = [.lN, .anAng]
        check("each preference change notifies synchronously", notifications == 10)

        let restored = Preferences(defaults: defaults)
        check("disabled switches persist",
              !restored.predictionsEnabled && !restored.contextEnabled &&
              !restored.shiftToggleEnabled && restored.pinyinDisplayMode == .hidden &&
              !restored.learningEnabled)
        check("user dictionary interval persists",
              restored.userDictionarySaveInterval == .fifteenMinutes)
        check("fuzzy groups persist independently",
              restored.fuzzyPinyinGroups == [.lN, .anAng] &&
              restored.fuzzyPinyinMask == UInt32(HMM_FUZZY_L_N | HMM_FUZZY_AN_ANG))
        for group in FuzzyPinyinGroup.allCases {
            restored.fuzzyPinyinGroups = [group]
            check("\(group.title) persists with its native mask",
                  Preferences(defaults: defaults).fuzzyPinyinGroups == [group] &&
                  restored.fuzzyPinyinMask == group.mask)
        }
        restored.fuzzyPinyinGroups = Set(FuzzyPinyinGroup.allCases)
        check("all fuzzy masks are distinct and cover native options",
              restored.fuzzyPinyinMask == UInt32(HMM_FUZZY_ALL) &&
              Set(FuzzyPinyinGroup.allCases.map(\.mask)).count == 12)
        restored.fuzzyPinyinGroups = []
        check("disabled fuzzy groups persist",
              Preferences(defaults: defaults).fuzzyPinyinGroups.isEmpty)
        defaults.set(["l_n", "unknown", "l_n"], forKey: "fuzzyPinyinGroups")
        check("unknown fuzzy groups are ignored and duplicates removed",
              Preferences(defaults: defaults).fuzzyPinyinGroups == [.lN])
        defaults.set(42, forKey: "fuzzyPinyinGroups")
        check("malformed fuzzy preferences remain disabled",
              Preferences(defaults: defaults).fuzzyPinyinGroups.isEmpty)
        for mode in PinyinDisplayMode.allCases {
            restored.pinyinDisplayMode = mode
            check("\(mode.rawValue) display mode persists",
                  Preferences(defaults: defaults).pinyinDisplayMode == mode)
        }
        for interval in UserDictionarySaveInterval.allCases {
            restored.userDictionarySaveInterval = interval
            check("\(interval.title) interval persists",
                  Preferences(defaults: defaults).userDictionarySaveInterval == interval)
        }
        check("page size and correction style persist",
              restored.candidatePageSize == .five && restored.correctionStyle == .color)
        check("DL correction alignment persists", restored.correctionAlignment == .dl)
        restored.correctionAlignment = .lcs
        check("LCS correction alignment persists",
              Preferences(defaults: defaults).correctionAlignment == .lcs)
        restored.correctionStyle = .hidden
        check("hidden correction style persists",
              Preferences(defaults: defaults).correctionStyle == .hidden)

        defaults.set(100, forKey: "candidatePageSize")
        defaults.set("invalid", forKey: "correctionStyle")
        defaults.set("invalid", forKey: "correctionAlignment")
        defaults.set(-1, forKey: "userDictionarySaveInterval")
        defaults.set("invalid", forKey: "pinyinDisplayMode")
        let recovered = Preferences(defaults: defaults)
        check("unrecognized stored options use supported defaults",
              recovered.candidatePageSize == .nine && recovered.correctionStyle == .edits &&
              recovered.correctionAlignment == .lcs &&
              recovered.userDictionarySaveInterval == .fourHours &&
              recovered.pinyinDisplayMode == .candidateWindow)
    }
}

func testFuzzyPinyinPreference() {
    print("[test_fuzzy_pinyin_preference]")
    withTestPreferences { preferences, defaults in
        preferences.contextEnabled = false
        preferences.predictionsEnabled = false
        preferences.learningEnabled = false
        let (session, delegate) = makeSession(preferences: preferences)
        for ch in "hulan" { _ = session.appendLetter(String(ch)) }
        let baseline = session.candidates
        preferences.fuzzyPinyinGroups = [.lN]
        check("fuzzy preference immediately redecodes the active composition",
              session.composition == "hulan" && session.candidates.first == "湖南" &&
              delegate.shownCandidates == session.candidates &&
              delegate.committedText.isEmpty &&
              delegate.markedText == session.segmentedPinyin)
        preferences.fuzzyPinyinGroups = []
        check("disabling fuzzy pinyin restores current candidates",
              session.candidates == baseline && session.composition == "hulan")
        preferences.fuzzyPinyinGroups = [.lN]
        _ = session.selectCurrent()
        check("fuzzy candidate consumes its original typed pinyin",
              delegate.committedText == "湖南" && !session.isComposing)

        session.cancel()
        delegate.clear()
        for ch in "xi" { _ = session.appendLetter(String(ch)) }
        _ = session.handlePunctuation("'")
        for ch in "an" { _ = session.appendLetter(String(ch)) }
        preferences.fuzzyPinyinGroups = [.lN, .zZh]
        check("fuzzy switch preserves manual separators",
              session.composition == "xi'an" &&
              session.segmentedPinyin == "xi'an" && session.candidates.first == "西安")
        _ = session.selectCurrent()
        check("manual separator selection still commits correctly",
              delegate.committedText == "西安" && !session.isComposing)

        session.cancel()
        delegate.clear()
        for ch in "nihaohulan" { _ = session.appendLetter(String(ch)) }
        check("partial selection before fuzzy switch succeeds",
              selectCandidate("你好", in: session) && session.composition == "hulan")
        preferences.fuzzyPinyinGroups = [.lN]
        _ = session.selectCurrent()
        check("fuzzy switch retains partial commits and remaining consumption",
              delegate.committedText == "你好湖南" && !session.isComposing)
        session.cancel()
        preferences.fuzzyPinyinGroups = []
        preferences.predictionsEnabled = true
        session.showPredictions(context: "生日快")
        check("fuzzy test has predictions", session.isPredicting)
        preferences.fuzzyPinyinGroups = [.lN]
        check("fuzzy switch dismisses stale predictions without inserting text",
              !session.isPredicting && !session.isComposing &&
              delegate.committedText == "你好湖南")
        let restored = Preferences(defaults: defaults)
        check("saved fuzzy configuration can be restored to the engine",
              gboard_set_fuzzy_pinyin(restored.fuzzyPinyinMask) &&
              gboard_get_fuzzy_pinyin() == restored.fuzzyPinyinMask)
        session.cancel()
        delegate.clear()
        preferences.contextEnabled = true
        delegate.contextBeforeInput = "我对这里很"
        for ch in "bushu" { _ = session.appendLetter(String(ch)) }
        preferences.fuzzyPinyinGroups = [.lN, .fH]
        check("fuzzy updates preserve context-aware ranking",
              session.candidates.first == "不熟" && session.composition == "bushu" &&
              delegate.contextRequestCount == 1)
        session.cancel()
        delegate.clear()
        preferences.contextEnabled = false
        preferences.learningEnabled = true
        check("fuzzy learning test clears existing data", gboard_user_dict_clear())
        for ch in "ceshi" { _ = session.appendLetter(String(ch)) }
        check("fuzzy learning test selects its first segment",
              selectCandidate("侧", in: session) && session.composition == "shi")
        preferences.fuzzyPinyinGroups = [.lN]
        check("fuzzy switch retains pending segmented learning",
              selectCandidate("试", in: session) && delegate.committedText == "侧试" &&
              !session.isComposing && gboard_user_dict_get_size() > 0)
        let learnedSize = gboard_user_dict_get_size()
        preferences.fuzzyPinyinGroups = [.zZh, .anAng]
        check("fuzzy switch keeps learned dictionary entries",
              gboard_user_dict_get_size() == learnedSize)
        check("fuzzy learning test clears its learned entries", gboard_user_dict_clear())
        preferences.fuzzyPinyinGroups = []
        session.cancel()
    }
}

func testPinyinSchemePreference() {
    print("[test_pinyin_scheme_preference]")
    withTestPreferences { preferences, defaults in
        check("pinyin defaults to full", preferences.pinyinScheme == .full)
        check("all seven schemes have distinct native values",
              Set(PinyinScheme.allCases.map { $0.nativeValue.rawValue }).count == 7)
        for scheme in PinyinScheme.allCases {
            var applied: HmmPinyinScheme?
            check("scheme preference applies its native value",
                  preferences.applyPinyinScheme(scheme, using: {
                      applied = $0
                      return true
                  }) && applied == scheme.nativeValue)
            check("each scheme persists",
                  Preferences(defaults: defaults).pinyinScheme == scheme)
        }
        defaults.set("unsupported", forKey: "pinyinScheme")
        check("unknown stored scheme falls back to full",
              Preferences(defaults: defaults).pinyinScheme == .full)
        defaults.set(42, forKey: "pinyinScheme")
        check("malformed stored scheme falls back to full",
              Preferences(defaults: defaults).pinyinScheme == .full)
    }

    withTestPreferences { preferences, defaults in
        preferences.contextEnabled = false
        preferences.predictionsEnabled = false
        preferences.learningEnabled = false
        check("flypy scheme applies without a restart", preferences.applyPinyinScheme(.flypy))
        let (session, delegate) = makeSession(preferences: preferences)
        let (idleSession, _) = makeSession(preferences: preferences)
        for ch in "nihc" { _ = session.appendLetter(String(ch)) }
        check("flypy uses native keys and reading",
              session.candidates.first == "你好" &&
              session.segmentedPinyin == "ni'hc" &&
              session.pinyinReading.text == "ni'hc" &&
              !session.pinyinReading.spans.contains { $0.isCorrected || $0.isTypo })
        let before = session.candidates
        check("scheme switch is accepted but deferred during composition",
              preferences.applyPinyinScheme(.ms) && preferences.pinyinSchemePending &&
              preferences.pinyinScheme == .ms &&
              gboard_get_pinyin_scheme() == HMM_PINYIN_SHUANGPIN_FLYPY)
        check("idle sessions do not apply a pending switch early",
              !idleSession.isComposing &&
              session.candidates == before && delegate.committedText.isEmpty)
        _ = session.deleteBack()
        _ = session.appendLetter("c")
        check("replaying input still uses the original scheme",
              session.candidates.first == "你好" && session.composition == "nihc")
        preferences.fuzzyPinyinGroups = [.lN]
        check("fuzzy updates do not apply the deferred scheme",
              session.candidates.first == "你好" &&
              gboard_get_pinyin_scheme() == HMM_PINYIN_SHUANGPIN_FLYPY)
        preferences.fuzzyPinyinGroups = []
        check("partial shuangpin selection preserves remaining original keys",
              selectCandidate("你", in: session) &&
              session.composition == "hc" && session.candidates.first == "好" &&
              gboard_get_pinyin_scheme() == HMM_PINYIN_SHUANGPIN_FLYPY)
        _ = session.selectCurrent()
        check("scheme applies only after the entire composition ends",
              delegate.committedText == "你好" && !session.isComposing &&
              !preferences.pinyinSchemePending &&
              gboard_get_pinyin_scheme() == HMM_PINYIN_SHUANGPIN_MS)
        check("deferred selection is persisted",
              Preferences(defaults: defaults).pinyinScheme == .ms)

        for scheme: PinyinScheme in [.ms, .ziguang] {
            check("semicolon scheme applies", preferences.applyPinyinScheme(scheme))
            delegate.clear()
            _ = session.handlePunctuation(";")
            check("semicolon outside composition remains punctuation",
                  delegate.committedText == "；" && !session.isComposing)
            delegate.clear()
            _ = session.appendLetter("b")
            _ = session.handlePunctuation(";")
            check("semicolon reaches the native decoder",
                  session.composition == "b;" && session.candidates.contains("冰") &&
                  delegate.committedText.isEmpty)
            _ = session.deleteBack()
            check("semicolon deletes as one raw key", session.composition == "b")
            session.cancel()
        }
        check("switch to full pinyin", preferences.applyPinyinScheme(.full))
        delegate.clear()
        for ch in "nihao" { _ = session.appendLetter(String(ch)) }
        check("request flypy while full pinyin is active",
              preferences.applyPinyinScheme(.flypy))
        _ = session.commitRawPinyin()
        check("Return preserves old raw input then switches",
              delegate.committedText == "nihao" &&
              gboard_get_pinyin_scheme() == HMM_PINYIN_SHUANGPIN_FLYPY)

        delegate.clear()
        for ch in "nihc" { _ = session.appendLetter(String(ch)) }
        check("request full pinyin during flypy input", preferences.applyPinyinScheme(.full))
        session.cancel()
        check("Escape applies the pending scheme without inserting text",
              delegate.committedText.isEmpty && !session.isComposing &&
              gboard_get_pinyin_scheme() == HMM_PINYIN_FULL)
        check("enable flypy for explicit separator", preferences.applyPinyinScheme(.flypy))
        for ch in "ni" { _ = session.appendLetter(String(ch)) }
        _ = session.handlePunctuation("'")
        for ch in "hc" { _ = session.appendLetter(String(ch)) }
        check("explicit separators remain raw-key based",
              session.composition == "ni'hc" && session.segmentedPinyin == "ni'hc" &&
              session.candidates.first == "你好")
        _ = session.selectCurrent()
        check("explicitly separated shuangpin commits correctly",
              delegate.committedText == "你好" && !session.isComposing)

        preferences.learningEnabled = true
        delegate.clear()
        check("shuangpin learning starts empty", gboard_user_dict_clear())
        for ch in "ceui" { _ = session.appendLetter(String(ch)) }
        check("shuangpin learning selects its first segment",
              selectCandidate("侧", in: session) && session.composition == "ui")
        check("request full pinyin before finishing learned phrase",
              preferences.applyPinyinScheme(.full))
        check("pending learning finishes before the accessor switches",
              selectCandidate("试", in: session) &&
              delegate.committedText == "侧试" && !session.isComposing &&
              gboard_user_dict_get_size() > 0 &&
              gboard_get_pinyin_scheme() == HMM_PINYIN_FULL)
        for ch in "ceshi" { _ = session.appendLetter(String(ch)) }
        check("shuangpin learning remains usable in full pinyin",
              candidatePosition("侧试", in: session) != nil)
        session.cancel()
        check("clear shuangpin test learning", gboard_user_dict_clear())

        preferences.predictionsEnabled = true
        session.showPredictions(context: "生日快")
        check("scheme test has predictions", session.isPredicting)
        check("switch to flypy after prediction", preferences.applyPinyinScheme(.flypy))
        check("scheme change dismisses stale predictions", !session.isPredicting)
        _ = session.appendLetter("n")
        check("request full pinyin before deleting last key", preferences.applyPinyinScheme(.full))
        _ = session.deleteBack()
        check("deleting the last key applies pending scheme",
              !session.isComposing && gboard_get_pinyin_scheme() == HMM_PINYIN_FULL)
        session.cancel()
    }
}

func testPinyinSchemeRestoreFailure() {
    print("[test_pinyin_scheme_restore_failure]")
    withTestPreferences { _, defaults in
        check("scheme restore starts from full", gboard_set_pinyin_scheme(HMM_PINYIN_FULL))
        defaults.set("flypy", forKey: "pinyinScheme")
        let preferences = Preferences(defaults: defaults)
        preferences.contextEnabled = false
        preferences.predictionsEnabled = false
        preferences.learningEnabled = false
        check("failed startup scheme restore is reported",
              !preferences.applyPinyinScheme(using: { _ in false }) &&
              preferences.pinyinSchemeApplyFailed)
        check("failed restore retains the saved selection and native scheme",
              preferences.pinyinScheme == .flypy &&
              Preferences(defaults: defaults).pinyinScheme == .flypy &&
              gboard_get_pinyin_scheme() == HMM_PINYIN_FULL)
        let (session, delegate) = makeSession(preferences: preferences)
        for ch in "nihao" { _ = session.appendLetter(String(ch)) }
        preferences.correctionStyle = .color
        session.cancel()
        check("unrelated settings and composition boundaries do not retry a failed restore",
              gboard_get_pinyin_scheme() == HMM_PINYIN_FULL &&
              preferences.pinyinSchemeApplyFailed)
        check("explicit retry restores the saved scheme",
              preferences.applyPinyinScheme() && !preferences.pinyinSchemeApplyFailed &&
              gboard_get_pinyin_scheme() == HMM_PINYIN_SHUANGPIN_FLYPY)
        delegate.clear()
        for ch in "nihc" { _ = session.appendLetter(String(ch)) }
        check("restored scheme decodes correctly", session.candidates.first == "你好")
        session.cancel()
        check("failed new selection retains the previous selection",
              !preferences.applyPinyinScheme(.ms, using: { _ in false }) &&
              preferences.pinyinSchemeApplyFailed &&
              preferences.pinyinScheme == .flypy &&
              Preferences(defaults: defaults).pinyinScheme == .flypy &&
              gboard_get_pinyin_scheme() == HMM_PINYIN_SHUANGPIN_FLYPY)
        check("restore full after failure tests", preferences.applyPinyinScheme(.full))
    }
    withTestPreferences { preferences, _ in
        preferences.contextEnabled = false
        preferences.predictionsEnabled = false
        preferences.learningEnabled = false
        check("deferred recovery starts with a visible native failure",
              !preferences.applyPinyinScheme(using: { _ in false }) &&
              preferences.pinyinSchemeApplyFailed)
        let (session, _) = makeSession(preferences: preferences)
        _ = session.appendLetter("n")
        var prematureApplies = 0
        let unexpectedApply: (HmmPinyinScheme) -> Bool = { _ in
            prematureApplies += 1
            return true
        }
        check("queued selection does not clear an existing native failure",
              preferences.applyPinyinScheme(.flypy, using: unexpectedApply) &&
              preferences.pinyinSchemeApplyFailed)
        check("selecting the old scheme still retains failure and queues a retry",
              preferences.applyPinyinScheme(.full, using: unexpectedApply) &&
              preferences.pinyinSchemeApplyFailed &&
              preferences.pinyinScheme.nativeValue == gboard_get_pinyin_scheme())
        check("queued recovery never applies before composition ends",
              prematureApplies == 0)
        session.cancel()
        check("composition boundary retries even when the scheme value is unchanged",
              !preferences.pinyinSchemeApplyFailed &&
              preferences.pinyinScheme == .full && !preferences.pinyinSchemePending)
    }
}

func testFuzzyPinyinRestoreFailure() {
    print("[test_fuzzy_pinyin_restore_failure]")
    withTestPreferences { _, defaults in
        check("restore test starts with fuzzy pinyin disabled", gboard_set_fuzzy_pinyin(0))
        defaults.set(["l_n"], forKey: "fuzzyPinyinGroups")
        let preferences = Preferences(defaults: defaults)
        preferences.contextEnabled = false
        preferences.predictionsEnabled = false
        preferences.learningEnabled = false
        var requestedMasks: [UInt32] = []
        let failApply: (UInt32) -> Bool = { mask in
            requestedMasks.append(mask)
            return false
        }
        check("startup restore failure is reported",
              !preferences.applyFuzzyPinyin(using: failApply) &&
              preferences.fuzzyPinyinApplyFailed)
        check("startup failure keeps saved selections and native configuration",
              preferences.fuzzyPinyinGroups == [.lN] &&
              Preferences(defaults: defaults).fuzzyPinyinGroups == [.lN] &&
              gboard_get_fuzzy_pinyin() == 0)
        check("failure status is transient, not a saved preference",
              !Preferences(defaults: defaults).fuzzyPinyinApplyFailed)

        let (session, delegate) = makeSession(preferences: preferences)
        for ch in "hulan" { _ = session.appendLetter(String(ch)) }
        let originalCandidates = session.candidates
        let originalMarkedText = delegate.markedText
        check("composition still uses original configuration after failed restore",
              originalCandidates.first == "护栏" && session.composition == "hulan")
        check("failed retry continues to report unapplied settings",
              !preferences.applyFuzzyPinyin(using: failApply) &&
              preferences.fuzzyPinyinApplyFailed)
        check("failed retry preserves composition, candidates and saved choices",
              session.composition == "hulan" && session.candidates == originalCandidates &&
              delegate.markedText == originalMarkedText && delegate.committedText.isEmpty &&
              Preferences(defaults: defaults).fuzzyPinyinGroups == [.lN])
        preferences.correctionStyle = .color
        check("unrelated preferences do not silently retry failed fuzzy settings",
              preferences.fuzzyPinyinApplyFailed && gboard_get_fuzzy_pinyin() == 0 &&
              session.candidates == originalCandidates)
        check("retry always uses the saved mask",
              requestedMasks == [UInt32(HMM_FUZZY_L_N), UInt32(HMM_FUZZY_L_N)])

        check("successful retry clears failure status",
              preferences.applyFuzzyPinyin() && !preferences.fuzzyPinyinApplyFailed)
        check("retry refreshes a session created after failed startup",
              session.composition == "hulan" && session.candidates.first == "湖南" &&
              delegate.shownCandidates == session.candidates &&
              delegate.committedText.isEmpty &&
              delegate.markedText == session.segmentedPinyin)
        check("retry preserves persisted selections",
              Preferences(defaults: defaults).fuzzyPinyinGroups == [.lN])
        let appliedCandidates = session.candidates
        check("failed new selection leaves existing applied state intact",
              !preferences.applyFuzzyPinyin([.lN, .zZh], using: failApply) &&
              !preferences.fuzzyPinyinApplyFailed &&
              preferences.fuzzyPinyinGroups == [.lN] &&
              gboard_get_fuzzy_pinyin() == UInt32(HMM_FUZZY_L_N) &&
              session.candidates == appliedCandidates)
        check("failed new selection does not overwrite stored choices",
              Preferences(defaults: defaults).fuzzyPinyinGroups == [.lN])
        check("successful selection updates settings and runtime together",
              preferences.applyFuzzyPinyin([.lN, .zZh]) &&
              preferences.fuzzyPinyinGroups == [.lN, .zZh] &&
              Preferences(defaults: defaults).fuzzyPinyinGroups == [.lN, .zZh] &&
              !preferences.fuzzyPinyinApplyFailed)
        check("restore test resets fuzzy pinyin", preferences.applyFuzzyPinyin([]))
        session.cancel()
    }
}

func testPinyinDisplayModePreference() {
    print("[test_pinyin_display_mode_preference]")
    withTestPreferences { preferences, _ in
        let (session, delegate) = makeSession(preferences: preferences)
        for ch in "nihoa" { _ = session.appendLetter(String(ch)) }
        let candidates = session.candidates
        let markedText = delegate.markedText
        let reading = delegate.candidateReading
        check("pinyin bar is visible by default", !reading.isEmpty)
        let originalMarkedUpdates = delegate.markedTextUpdateCount
        preferences.pinyinDisplayMode = .hidden
        check("hiding pinyin immediately removes only the strip",
              delegate.candidateReading.isEmpty &&
              delegate.shownCandidates == candidates && delegate.markedText == markedText)
        check("switching display mode immediately updates the client's marked text",
              delegate.markedTextUpdateCount == originalMarkedUpdates + 1)
        let markedRect = NSRect(x: 520, y: 420, width: 1, height: 20)
        let staleAnchor = NSRect(x: 20, y: 20, width: 1, height: 20)
        var caretRequested = false
        let anchor = CandidateWindowController.anchorRect(
            isPrediction: session.isPredicting,
            caretRect: { caretRequested = true; return .zero },
            markedTextRect: { markedRect },
            previousAnchor: staleAnchor)
        check("hidden pinyin still anchors composition at marked text, not a stale corner",
              anchor == markedRect && !caretRequested)
        preferences.correctionStyle = .color
        check("changing correction style does not reveal a hidden strip",
              delegate.candidateReading.isEmpty)
        preferences.pinyinDisplayMode = .inline
        check("native inline mode hides the duplicate candidate strip",
              delegate.candidateReading.isEmpty && delegate.shownCandidates == candidates &&
              delegate.markedText == session.segmentedPinyin && !session.isPredicting)
        check("native inline mode supplies visible text to the actual client",
              preferences.pinyinDisplayMode.markedText(for: session.segmentedPinyin)
                  == session.segmentedPinyin)
        preferences.pinyinDisplayMode = .candidateWindow
        check("showing pinyin restores the current correction style",
              !delegate.candidateReading.isEmpty &&
              !delegate.candidateReading.spans.contains(where: \.isTypo))
        preferences.pinyinDisplayMode = .inline
        _ = session.selectCurrent()
        check("inline pinyin does not duplicate committed text", delegate.committedText == "你好")
        check("prediction windows still have no pinyin bar", delegate.candidateReading.isEmpty)
        session.dismissPredictions()
        session.cancel()
        check("cancelling clears native marked text", delegate.markedText == nil)
        let idleMarkedUpdates = delegate.markedTextUpdateCount
        for mode in PinyinDisplayMode.allCases {
            preferences.pinyinDisplayMode = mode
            check("\(mode.rawValue) clears rather than inserts empty marked text",
                  mode.markedText(for: "").isEmpty)
            check("\(mode.rawValue) preserves the composition anchor when text is invisible",
                  mode == .inline || mode.markedText(for: "ni'hao") == "\u{200B}")
            session.showPredictions(context: "生日快")
            check("\(mode.rawValue) predictions never show a pinyin strip",
                  session.isPredicting && delegate.candidateReading.isEmpty)
            session.dismissPredictions()
        }
        check("changing display mode while idle does not insert marked text",
              delegate.markedTextUpdateCount == idleMarkedUpdates)
    }
}

func testPinyinDisplayModeMigration() {
    print("[test_pinyin_display_mode_migration]")
    withTestPreferences { _, defaults in
        defaults.set(false, forKey: "pinyinBarEnabled")
        let hidden = Preferences(defaults: defaults)
        check("legacy disabled pinyin remains hidden", hidden.pinyinDisplayMode == .hidden)
        hidden.pinyinDisplayMode = .inline
        check("new inline selection overrides the old disabled switch",
              Preferences(defaults: defaults).pinyinDisplayMode == .inline)
        defaults.removeObject(forKey: "pinyinDisplayMode")
        defaults.set(true, forKey: "pinyinBarEnabled")
        check("legacy enabled pinyin remains in the candidate window",
              Preferences(defaults: defaults).pinyinDisplayMode == .candidateWindow)
    }
}

func testNativeMarkedTextPresentation() {
    print("[test_native_marked_text_presentation]")
    let editor = NSTextView(frame: NSRect(x: 0, y: 0, width: 400, height: 100))
    let prefix = "前文🧩 "
    editor.string = prefix
    editor.setSelectedRange(NSRange(location: prefix.utf16.count, length: 0))

    func update(_ mode: PinyinDisplayMode, _ pinyin: String) {
        let text = mode.markedText(for: pinyin)
        editor.setMarkedText(
            NSAttributedString(string: text),
            selectedRange: NSRange(location: text.utf16.count, length: 0),
            replacementRange: NSRange(location: NSNotFound, length: 0))
    }

    update(.inline, "ni'hao")
    check("native marked text is visible inside the editor",
          editor.string == prefix + "ni'hao" && editor.hasMarkedText())
    update(.inline, "ni'hao'ma")
    check("updating native marked text replaces rather than appends",
          editor.string == prefix + "ni'hao'ma")
    update(.hidden, "ni'hao'ma")
    check("switching to hidden removes visible text without committing",
          editor.string == prefix + "\u{200B}" && editor.hasMarkedText())
    update(.candidateWindow, "ni'hao'ma")
    check("candidate-window mode keeps only an invisible native anchor",
          editor.string == prefix + "\u{200B}")
    update(.inline, "ni'hao")
    editor.insertText("你好", replacementRange: NSRange(location: NSNotFound, length: 0))
    check("candidate selection replaces native pinyin exactly once",
          editor.string == prefix + "你好" && !editor.hasMarkedText())
    update(.inline, "shi")
    update(.inline, "")
    check("cancelling native marked text preserves previously committed text",
          editor.string == prefix + "你好" && !editor.hasMarkedText())
}

func testLearningPreference() {
    print("[test_learning_preference]")
    withTestPreferences { preferences, _ in
        preferences.predictionsEnabled = false
        preferences.contextEnabled = false
        check("learning preference test starts with an empty dictionary",
              gboard_user_dict_clear() && gboard_user_dict_get_size() == 0)

        func selectPhrase(_ afterFirstSegment: () -> Void = {}) {
            let (session, delegate) = makeSession(preferences: preferences)
            for ch in "ceshi" { _ = session.appendLetter(String(ch)) }
            check("learning test selects its first segment",
                  selectCandidate("侧", in: session) && session.composition == "shi")
            afterFirstSegment()
            check("learning test completes its phrase",
                  selectCandidate("试", in: session) && delegate.committedText == "侧试")
            session.cancel()
        }

        preferences.learningEnabled = false
        for _ in 0..<3 { selectPhrase() }
        check("disabled learning does not add selected phrases",
              gboard_user_dict_get_size() == 0)
        preferences.learningEnabled = true
        selectPhrase()
        check("reenabled learning records new phrases", gboard_user_dict_get_size() > 0)
        let learnedSize = gboard_user_dict_get_size()
        preferences.learningEnabled = false
        check("disabling learning retains existing learned data",
              gboard_user_dict_get_size() == learnedSize)

        check("mid-composition test resets learning data", gboard_user_dict_clear())
        preferences.learningEnabled = true
        selectPhrase {
            preferences.learningEnabled = false
            preferences.learningEnabled = true
        }
        check("reenabling mid-phrase does not learn earlier disabled input",
              gboard_user_dict_get_size() == 0)
        selectPhrase()
        check("the next full phrase can be learned normally",
              gboard_user_dict_get_size() > 0)
        check("learning test cleans up its dictionary", gboard_user_dict_clear())
    }
}

func testUserDictionarySettings() {
    print("[test_user_dictionary_settings]")
    withTestPreferences { preferences, _ in
        var ready = false
        var saves = 0
        var clears = 0
        var savesSucceed = true
        var clearsSucceed = true
        let dictionary = UserDictionary(
            preferences: preferences, isReady: { ready },
            persist: {
                check("dictionary saves run on the engine thread", Thread.isMainThread)
                saves += 1
                return savesSucceed
            },
            clear: {
                check("dictionary clears run on the engine thread", Thread.isMainThread)
                clears += 1
                return clearsSucceed
            }
        )
        dictionary.startAutomaticSaving()
        check("automatic saving waits for initialization", dictionary.saveTimer == nil)
        if case .failure(.notReady) = dictionary.clearLearningData() {
            check("clear reports an unavailable dictionary without calling the engine", clears == 0)
        } else { check("clear must report an unavailable dictionary", false) }
        ready = true
        dictionary.startAutomaticSaving()
        let initialTimer = dictionary.saveTimer
        dictionary.startAutomaticSaving()
        check("starting automatic saving is idempotent",
              dictionary.saveTimer === initialTimer)
        for interval in UserDictionarySaveInterval.allCases {
            let oldTimer = dictionary.saveTimer
            let oldInterval = oldTimer?.timeInterval
            preferences.userDictionarySaveInterval = interval
            check("\(interval.title) reschedules the actual timer",
                  dictionary.saveTimer?.timeInterval == TimeInterval(interval.rawValue) &&
                  dictionary.saveTimer?.isValid == true)
            if oldInterval != TimeInterval(interval.rawValue) {
                check("rescheduling invalidates the previous timer", oldTimer?.isValid == false)
            }
            dictionary.saveTimer?.fire()
        }
        check("each scheduled timer calls persistence", saves == 4)
        let timer = dictionary.saveTimer
        preferences.learningEnabled = false
        check("unrelated preferences do not restart the save timer",
              dictionary.saveTimer === timer)
        dictionary.saveTimer?.fire()
        check("disabling learning does not discard pending saves", saves == 5)
        savesSucceed = false
        check("save failures are reported", !dictionary.persist())

        var notifications = 0
        let observer = NotificationCenter.default.addObserver(
            forName: UserDictionary.didClearNotification, object: dictionary, queue: nil
        ) { _ in notifications += 1 }
        defer { NotificationCenter.default.removeObserver(observer) }
        if case .success = dictionary.clearLearningData() {
            check("clear success is published after the engine call", clears == 1 && notifications == 1)
        } else { check("successful clear is reported", false) }
        clearsSucceed = false
        if case .failure(.clearFailed) = dictionary.clearLearningData() {
            check("partial clear failure also refreshes sessions", clears == 2 && notifications == 2)
        } else { check("failed clear must not report success", false) }
    }
    weak var releasedDictionary: UserDictionary?
    var timer: Timer?
    withTestPreferences { preferences, _ in
        let dictionary = UserDictionary(preferences: preferences, isReady: { true })
        dictionary.startAutomaticSaving()
        releasedDictionary = dictionary
        timer = dictionary.saveTimer
    }
    check("observation and timers do not retain dictionary services",
          releasedDictionary == nil && timer?.isValid == false)
}

func testClearLearningSessionUpdates() {
    print("[test_clear_learning_session_updates]")
    withTestPreferences { preferences, _ in
        let dictionary = UserDictionary(preferences: preferences)
        let (session, delegate) = makeSession(preferences: preferences)
        for ch in "ceshi" { _ = session.appendLetter(String(ch)) }
        check("clear test starts a segmented selection", selectCandidate("侧", in: session))
        let composition = session.composition
        if case .failure = dictionary.clearLearningData() {
            check("isolated test dictionary can be cleared", false)
        }
        check("clear refreshes candidates without committing the remaining composition",
              session.composition == composition && !delegate.shownCandidates.isEmpty &&
              delegate.committedText == "侧")
        check("remaining segment still commits normally", selectCandidate("试", in: session))
        check("clear discards previously accumulated learning",
              gboard_user_dict_get_size() == 0)
        session.showPredictions(context: "生日快")
        check("clear test has visible predictions", session.isPredicting)
        _ = dictionary.clearLearningData()
        check("clear dismisses stale predictions",
              !session.isPredicting && delegate.shownCandidates.isEmpty)
        session.cancel()
    }
}

func testRemoveAndUndoLearning() {
    print("[test_remove_and_undo_learning]")
    withTestPreferences { preferences, _ in
        preferences.predictionsEnabled = false
        preferences.contextEnabled = false
        check("remove/undo test starts with an empty dictionary",
              gboard_user_dict_clear())

        func position() -> Int? {
            let (session, _) = makeSession(preferences: preferences)
            for ch in "ceshi" { _ = session.appendLetter(String(ch)) }
            defer { session.cancel() }
            return candidatePosition("侧试", in: session)
        }
        func learnPhrase() -> PinyinSession {
            let (session, delegate) = makeSession(preferences: preferences)
            for ch in "ceshi" { _ = session.appendLetter(String(ch)) }
            _ = selectCandidate("侧", in: session)
            _ = selectCandidate("试", in: session)
            check("remove/undo test commits 侧试", delegate.committedText == "侧试")
            return session
        }
        func select(_ text: String, in session: PinyinSession) -> Bool {
            guard let index = session.candidates.firstIndex(of: text) else {
                return false
            }
            while session.selectedIndex < index { _ = session.moveRight() }
            return session.selectedIndex == index
        }

        let baseline = position()
        let learned = learnPhrase()
        check("learned commit can be undone", learned.canUndoLearning)
        let boosted = position()
        check("learning boosts the phrase",
              boosted != nil && (baseline == nil || boosted! < baseline!))
        check("undo decrements the learned phrase", learned.undoLastLearning())
        check("undo record is consumed", !learned.canUndoLearning &&
              !learned.undoLastLearning())
        check("undo restores the original ranking", position() == baseline)

        let (other, _) = makeSession(preferences: preferences)
        for ch in "ceshi" { _ = other.appendLetter(String(ch)) }
        _ = selectCandidate("侧", in: other)
        _ = selectCandidate("试", in: other)
        for ch in "ni" { _ = other.appendLetter(String(ch)) }
        check("new composition forgets the undo record", !other.canUndoLearning)
        other.cancel()
        let afterOther = position()
        check("a later commit keeps its learning", afterOther == boosted)

        for key in [Character(","), Character(".")] {
            let (punct, _) = makeSession(preferences: preferences)
            for ch in "ceshi" { _ = punct.appendLetter(String(ch)) }
            _ = selectCandidate("侧", in: punct)
            _ = punct.handlePunctuation(key)
            check("punctuation commit forgets the undo record (\(key))",
                  !punct.canUndoLearning)
        }
        let (passed, _) = makeSession(preferences: preferences)
        for ch in "ceshi" { _ = passed.appendLetter(String(ch)) }
        _ = selectCandidate("侧", in: passed)
        _ = passed.commitAndPassThrough()
        check("pass-through commit forgets the undo record",
              !passed.canUndoLearning)

        let forgotten = learnPhrase()
        forgotten.forgetLearningUndo()
        check("forgetting the record blocks undo", !forgotten.undoLastLearning())

        preferences.contextEnabled = true
        let (mismatch, delegate) = makeSession(preferences: preferences)
        for ch in "ceshi" { _ = mismatch.appendLetter(String(ch)) }
        _ = selectCandidate("侧", in: mismatch)
        _ = selectCandidate("试", in: mismatch)
        delegate.contextBeforeInput = "其他"
        check("mismatch case has an undo record", mismatch.canUndoLearning)
        check("undo is skipped when the text before the caret differs",
              !mismatch.undoLastLearning())
        let (matched, matchedDelegate) = makeSession(preferences: preferences)
        for ch in "ceshi" { _ = matched.appendLetter(String(ch)) }
        _ = selectCandidate("侧", in: matched)
        _ = selectCandidate("试", in: matched)
        matchedDelegate.contextBeforeInput = "这是侧试"
        check("undo runs when the caret follows the committed text",
              matched.undoLastLearning())
        preferences.contextEnabled = false

        for _ in 0..<5 { _ = learnPhrase() }
        let (session, mock) = makeSession(preferences: preferences)
        for ch in "ceshi" { _ = session.appendLetter(String(ch)) }
        check("remove test shows the learned phrase on the first page",
              select("侧试", in: session))
        check("selected learned phrase is marked removable",
              session.selectedCandidateRemovable)
        check("selected system phrase is not marked removable",
              select("测试", in: session) && !session.selectedCandidateRemovable)
        session.cancel()
        for ch in "ceshi" { _ = session.appendLetter(String(ch)) }
        _ = select("侧试", in: session)
        check("remove deletes the learned phrase", session.removeSelectedCandidate())
        // Without its user-dictionary entry, 侧试 survives only if the system
        // model composes it, and then it is never removable.
        if baseline == nil {
            check("removed phrase leaves the refreshed list",
                  !session.candidates.contains("侧试"))
        } else {
            check("removed phrase falls back to an unremovable system candidate",
                  !session.candidates.contains("侧试") ||
                  (select("侧试", in: session) && !session.selectedCandidateRemovable))
        }
        check("remove keeps composing with refreshed candidates",
              session.composition == "ceshi" && !mock.shownCandidates.isEmpty &&
              mock.shownCandidates == session.candidates &&
              session.selectedIndex < session.candidates.count &&
              mock.committedText.isEmpty)
        check("system candidate cannot be removed",
              select("测试", in: session) && !session.removeSelectedCandidate() &&
              session.candidates.contains("测试"))
        session.cancel()
        let removed = position()
        check("removed phrase no longer has its learned boost",
              removed == baseline)
        check("remove without composition does nothing",
              !session.removeSelectedCandidate())

        testRemoveOnLaterPage(preferences)
        _ = gboard_user_dict_clear()
    }
}

/// Removal from a page after the first, both normally and while a decoder
/// refresh is pending (the session re-decodes and restores the page).
private func testRemoveOnLaterPage(_ preferences: Preferences) {
    let pageSize = 5
    let originalPageSize = preferences.candidatePageSize
    preferences.candidatePageSize = .five
    defer { preferences.candidatePageSize = originalPageSize }
    check("later-page test starts with an empty dictionary",
          gboard_user_dict_clear())
    let phrases = [["侧", "试"], ["厕", "视"], ["策", "士"], ["测", "视"],
                   ["侧", "视"], ["册", "时"], ["策", "试"], ["侧", "时"],
                   ["厕", "试"], ["测", "时"], ["册", "试"], ["策", "视"]]
    for parts in phrases {
        let (learn, _) = makeSession(preferences: preferences)
        for ch in "ceshi" { _ = learn.appendLetter(String(ch)) }
        for part in parts { _ = selectCandidate(part, in: learn) }
    }

    // Leaves the session on the first removable candidate past page 0.
    func selectRemovableOnLaterPage(_ session: PinyinSession) -> String? {
        session.cancel()
        for ch in "ceshi" { _ = session.appendLetter(String(ch)) }
        for _ in 0..<3 {
            guard session.hasNextPage else { return nil }
            _ = session.nextPage()
            for index in session.candidates.indices {
                while session.selectedIndex < index { _ = session.moveRight() }
                if session.selectedCandidateRemovable {
                    return session.candidates[index]
                }
            }
        }
        return nil
    }
    func offeredAsLearned(_ text: String) -> Bool {
        let (probe, _) = makeSession(preferences: preferences)
        for ch in "ceshi" { _ = probe.appendLetter(String(ch)) }
        defer { probe.cancel() }
        while true {
            if let index = probe.candidates.firstIndex(of: text) {
                while probe.selectedIndex < index { _ = probe.moveRight() }
                return probe.selectedCandidateRemovable
            }
            guard probe.hasNextPage else { return false }
            _ = probe.nextPage()
        }
    }
    func checkRemovedInPlace(_ label: String, _ session: PinyinSession,
                             _ mock: MockDelegate, page: Int) {
        check("\(label) keeps the page and a valid selection",
              session.composition == "ceshi" && session.candidatePage == page &&
              !session.candidates.isEmpty &&
              session.selectedIndex < session.candidates.count &&
              mock.shownCandidates == session.candidates &&
              mock.committedText.isEmpty)
    }

    let (session, mock) = makeSession(preferences: preferences)
    guard let text = selectRemovableOnLaterPage(session) else {
        check("learned phrases reach a later page", false)
        return
    }
    let page = session.candidatePage
    check("later-page removal deletes \(text)", session.removeSelectedCandidate())
    checkRemovedInPlace("later-page removal", session, mock, page: page)
    session.cancel()
    check("later-page removal drops the learned entry", !offeredAsLearned(text))

    let (pending, pendingMock) = makeSession(preferences: preferences)
    guard let pendingText = selectRemovableOnLaterPage(pending) else {
        check("another learned phrase reaches a later page", false)
        return
    }
    let pendingPage = pending.candidatePage
    let engineIndex = Int32(pendingPage * pageSize + pending.selectedIndex)
    let tokenCount = Int(gboard_user_dict_extract_token_count(engineIndex))
    var tokens: [String] = []
    var types: [Int32] = []
    for i in 0..<tokenCount {
        var buffer = [CChar](repeating: 0, count: 16)
        var type: Int32 = 0
        if gboard_user_dict_get_token(engineIndex, Int32(i), &buffer, &type) {
            tokens.append(String(cString: buffer))
            types.append(type)
        }
    }
    check("pending test captures the phrase tokens",
          tokenCount > 0 && tokens.count == tokenCount)
    // Clearing without a user-data directory marks the decoder stale, like a
    // save does; relearning the phrase directly keeps that state.
    check("clear while composing leaves a decoder refresh pending",
          gboard_user_dict_clear() && gboard_user_dict_decoder_refresh_pending())
    let relearned = tokens.withCString2DArray { cTokens in
        types.withUnsafeMutableBufferPointer { cTypes in
            pendingText.withCString { cValue in
                gboard_user_dict_learn(cTokens, cTypes.baseAddress!,
                                       Int32(tokens.count), cValue, true)
            }
        }
    }
    check("pending test relearns \(pendingText) without a reset",
          relearned && gboard_user_dict_decoder_refresh_pending() &&
          pending.selectedCandidateRemovable)
    check("pending removal deletes \(pendingText)",
          pending.removeSelectedCandidate())
    check("pending removal refreshes the decoder",
          !gboard_user_dict_decoder_refresh_pending())
    checkRemovedInPlace("pending removal", pending, pendingMock, page: pendingPage)
    pending.cancel()
    check("pending removal drops the learned entry",
          !offeredAsLearned(pendingText))
}

private func storeError(_ result: Result<Void, LearnedWordStore.StoreError>)
    -> LearnedWordStore.StoreError? {
    if case .failure(let error) = result { return error }
    return nil
}

func testLearnedWordStore() {
    print("[test_learned_word_store]")
    withTestPreferences { preferences, _ in
        preferences.predictionsEnabled = false
        preferences.contextEnabled = false
        check("word store test starts with an empty dictionary",
              gboard_user_dict_clear())
        for _ in 0..<2 {
            let (session, _) = makeSession(preferences: preferences)
            for ch in "ceshi" { _ = session.appendLetter(String(ch)) }
            _ = selectCandidate("侧", in: session)
            _ = selectCandidate("试", in: session)
        }
        func learnedRank() -> (Int?, Bool) {
            let (session, _) = makeSession(preferences: preferences)
            for ch in "ceshi" { _ = session.appendLetter(String(ch)) }
            defer { session.cancel() }
            guard let index = session.candidates.firstIndex(of: "侧试") else {
                return (nil, false)
            }
            while session.selectedIndex < index { _ = session.moveRight() }
            return (index, session.selectedCandidateRemovable)
        }

        var saves = 0
        let native = LearnedWordOperations.native
        let store = LearnedWordStore(operations: LearnedWordOperations(
            export: native.export, remove: native.remove, setCount: native.setCount,
            persist: { saves += 1; return true }))
        check("store loads learned words", (try? store.reload().get()) != nil)
        guard let word = store.words.first(where: { $0.text == "侧试" }) else {
            check("store lists the learned phrase", false)
            return
        }
        check("store lists the learned phrase with its reading",
              store.words.count == 1 && word.dictionary == .pinyin &&
              word.tokens == ["ce", "shi"] && word.reading == "ce'shi" &&
              word.languages == [.chinese, .chinese] && word.fullMatch &&
              word.count == 2)
        check("search matches text and reading",
              word.matches("侧") && word.matches("ceshi") &&
              word.matches("ce'shi") && word.matches("CE SHI") &&
              !word.matches("测") && !word.matches("ceshu"))

        check("count edits reject zero",
              storeError(store.setCount(word.id, to: 0)) == .changeFailed(1))
        check("count edit saves and updates the list",
              (try? store.setCount(word.id, to: 40).get()) != nil && saves == 1 &&
              store.words.first?.count == 40)
        let (rank, removable) = learnedRank()
        check("raised count ranks the phrase first", rank == 0 && removable)
        check("count edit can lower a count",
              (try? store.setCount(word.id, to: 3).get()) != nil &&
              store.words.first?.count == 3 && store.words.first?.fullMatch == true)
        check("count edit raises it again",
              (try? store.setCount(word.id, to: 40).get()) != nil &&
              store.words.first?.count == 40 && saves == 3)

        let exported = LearnedWordStore.exportText(store.words)
        let parsed = LearnedWordStore.parse(exported)
        check("export round-trips", parsed?.words == store.words &&
              parsed?.invalid == 0)
        check("export uses readable field names",
              exported.contains("\"reading\":[\"ce\",\"shi\"]") &&
              exported.contains("\"languages\":[\"chinese\",\"chinese\"]") &&
              exported.contains("\"fullMatch\":true") &&
              exported.contains("\"dictionary\":\"pinyin\""))

        check("remove deletes the entry",
              (try? store.remove([word.id]).get()) != nil && saves == 4 &&
              store.words.isEmpty)
        check("removed phrase is no longer learned", !learnedRank().1)

        if case .success(let summary) = store.importText(exported) {
            check("import restores the removed phrase",
                  summary == .init(added: 1) && store.words.first?.count == 40 &&
                  saves == 5)
        } else {
            check("import restores the removed phrase", false)
        }
        let lower = exported.replacingOccurrences(of: "\"count\":40", with: "\"count\":5")
        let higher = exported.replacingOccurrences(of: "\"count\":40", with: "\"count\":60")
        check("import never lowers a count",
              (try? store.importText(lower).get()) == .init(unchanged: 1) &&
              store.words.first?.count == 40 && saves == 5)
        check("import raises a lower count",
              (try? store.importText(higher).get()) == .init(updated: 1) &&
              store.words.first?.count == 60)
        check("import rejects other files",
              store.importText("text\tonly\n") == .failure(.notWordList))
        func entry(_ fields: String) -> String {
            "{\"dictionary\":\"pinyin\",\"text\":\"坏\",\"fullMatch\":true,\(fields)}"
        }
        let chinese = "\"languages\":[\"chinese\"]"
        let tooMany = Int(HMM_USER_DICT_MAX_TOKENS) + 1
        let manyTokens = Array(repeating: "\"a\"", count: tooMany).joined(separator: ",")
        let manyLanguages = Array(repeating: "\"chinese\"", count: tooMany)
            .joined(separator: ",")
        let mixed = LearnedWordStore.exportText([]) + [
            entry("\"reading\":[\"ce\"],\"languages\":[\"chinese\",\"chinese\"],\"count\":3"),
            entry("\"reading\":[\"ce\"],\(chinese),\"count\":0"),
            entry("\"reading\":[\"abcdefghijklmnop\"],\(chinese),\"count\":3"),
            entry("\"reading\":[\"c e\"],\(chinese),\"count\":3"),
            entry("\"reading\":[\"ce\"],\"languages\":[\"japanese\"],\"count\":3"),
            entry("\"reading\":[\"ce\"],\"languages\":[16],\"count\":3"),
            entry("\"reading\":[\(manyTokens)],\"languages\":[\(manyLanguages)],\"count\":3"),
            "{\"dictionary\":\"other\",\"text\":\"坏\",\"reading\":[\"ce\"],\(chinese)," +
                "\"count\":3,\"fullMatch\":true}",
            "not json",
            "{\"dictionary\":\"english\",\"text\":\"ce\",\"reading\":[\"ce\"],\(chinese)," +
                "\"count\":3,\"fullMatch\":true}",
            entry("\"reading\":[\"q\",\"x\"],\"languages\":[\"english\",\"english\"]," +
                  "\"count\":3"),
            "{\"dictionary\":\"english\",\"text\":\"qx\",\"reading\":[\"qx\"]," +
                "\"languages\":[\"english\"],\"count\":3,\"fullMatch\":true}",
            entry("\"reading\":[\"ce\"],\(chinese),\"count\":3")
                .replacingOccurrences(of: "坏", with: "坏\\u0000好"),
            entry("\"reading\":[\"ce\"],\(chinese),\"count\":3")
                .replacingOccurrences(of: "坏", with: "坏\\n"),
            "{\"dictionary\":\"pinyin\",\"text\":\"坏\",\"reading\":[\"ce\"],\(chinese)," +
                "\"count\":3}",
        ].joined(separator: "\n")
        check("malformed lines are counted, not imported",
              (try? store.importText(mixed).get()) == .init(invalid: 15) &&
              store.words.count == 1)
        check("other formats and versions are rejected",
              LearnedWordStore.parse("{\"format\":\"other\",\"version\":1}\n") == nil &&
              LearnedWordStore.parse(
                  "{\"format\":\"gboardime-learned-words\",\"version\":2}\n") == nil)

        let multiline = LearnedWord(dictionary: .english, text: "a\nb\r\nc\u{2028}d",
                                    tokens: ["a"], languages: [.english], count: 1,
                                    fullMatch: true)
        func sample(_ dictionary: LearnedWordDictionary, _ text: String,
                    _ tokens: [String], _ count: Int) -> LearnedWord {
            LearnedWord(dictionary: dictionary, text: text, tokens: tokens,
                        languages: tokens.map { _ in
                            dictionary == .pinyin ? .chinese : .english },
                        count: count, fullMatch: true)
        }
        let unordered = [
            sample(.english, "ab", ["a", "b"], 50),
            sample(.pinyin, "测试", ["ce", "shi"], 3),
            sample(.pinyin, "爱", ["ai"], 3),
            sample(.pinyin, "你好", ["ni", "hao"], 9),
        ]
        check("export orders by dictionary, count, then reading",
              LearnedWordStore.parse(LearnedWordStore.exportText(unordered))?
                .words.map(\.text) == ["你好", "爱", "测试", "ab"] &&
              LearnedWordStore.exportText(unordered) ==
                LearnedWordStore.exportText(unordered.reversed()))
        check("export writes keys in a fixed order",
              LearnedWordStore.exportText([]) ==
                "{\"format\":\"gboardime-learned-words\",\"version\":1}\n" &&
              LearnedWordStore.exportText([unordered[3]]).split(separator: "\n").last ==
                "{\"dictionary\":\"pinyin\",\"text\":\"你好\",\"reading\":[\"ni\",\"hao\"]," +
                "\"languages\":[\"chinese\",\"chinese\"],\"count\":9,\"fullMatch\":true}")
        check("export keeps one word per line",
              LearnedWordStore.exportText([multiline]).split(separator: "\n").count == 2)
        let tricky = LearnedWord(dictionary: .english, text: "a\\b\"c/d😀",
                                 tokens: ["a", "b"], languages: [.english, .english],
                                 count: 20_000, fullMatch: false)
        let trickyText = LearnedWordStore.exportText([tricky])
        check("export round-trips escaped characters and large counts",
              LearnedWordStore.parse(trickyText)?.words == [tricky])
        check("CRLF files with a BOM are accepted",
              LearnedWordStore.parse("\u{FEFF}" +
                  trickyText.replacingOccurrences(of: "\n", with: "\r\n"))?
                .words == [tricky])

        _ = UserDictionary(preferences: preferences).clearLearningData()
        check("clearing learning data empties the list", store.words.isEmpty)

        let mock = MockLearnedWords([tricky])
        let stale = LearnedWordStore(operations: mock.operations)
        stale.reload()
        mock.live[0].count = 30_000
        var lowerTricky = tricky
        lowerTricky.count = 25_000
        check("import compares against the live dictionary",
              (try? stale.importText(LearnedWordStore.exportText([lowerTricky])).get())
                == .init(unchanged: 1) && mock.setCounts.isEmpty)
        var changes = 0
        let observer = NotificationCenter.default.addObserver(
            forName: UserDictionary.didChangeNotification, object: nil, queue: nil
        ) { _ in changes += 1 }
        let english = LearnedWord(dictionary: .english, text: "qxz", tokens: ["q", "x", "z"],
                                  languages: [.english, .english, .english], count: 2,
                                  fullMatch: true)
        check("import reports a failed save",
              (try? stale.importText(LearnedWordStore.exportText([english])).get())
                == .init(added: 1, saveFailed: true) && changes == 1)
        mock.live[0].count = 7
        check("count edits start from the live count",
              storeError(stale.setCount(tricky.id, to: 7)) == nil && mock.setCounts == [2] &&
              changes == 1)
        check("count edits announce dictionary changes",
              storeError(stale.setCount(tricky.id, to: 8)) == .saveFailed &&
              changes == 2 && stale.words.first?.count == 8)
        NotificationCenter.default.removeObserver(observer)

        mock.applies = false
        mock.saves = true
        check("a count edit that does not take effect is reported",
              storeError(stale.setCount(tricky.id, to: 9)) == .changeFailed(1) &&
              stale.words.first?.count == 8)
        var higherTricky = tricky
        higherTricky.count = 90_000
        check("an import that does not take effect is reported",
              (try? stale.importText(LearnedWordStore.exportText([higherTricky])).get())
                == .init(failed: 1))
        mock.applies = true

        mock.saves = false
        mock.removable = { _ in false }
        check("failed removal is reported",
              storeError(stale.remove([tricky.id])) == .changeFailed(1))
        mock.removable = { $0.text == "qxz" }
        check("partial removal also reports a failed save",
              storeError(stale.remove([tricky.id, english.id])) == .changeAndSaveFailed(1) &&
              stale.words.map(\.text) == [tricky.text])
        mock.saves = true
        mock.removable = { _ in true }
        var duplicate = english
        duplicate.count = 5
        check("duplicate import lines apply once with the highest count",
              (try? stale.importText(LearnedWordStore.exportText([english, duplicate]))
                .get()) == .init(added: 1, unchanged: 1) &&
              mock.setCounts.last == 5 && mock.live.count == 2)
        changes = 0
        let dropObserver = NotificationCenter.default.addObserver(
            forName: UserDictionary.didChangeNotification, object: nil, queue: nil
        ) { _ in changes += 1 }
        mock.drops = true
        check("a failed count edit that drops the word is announced",
              storeError(stale.setCount(english.id, to: 6)) == .changeFailed(1) &&
              changes == 1 && !stale.words.contains { $0.id == english.id })
        check("a failed import call is still saved and announced",
              (try? stale.importText(LearnedWordStore.exportText([duplicate])).get())
                == .init(failed: 1) && changes == 2)
        NotificationCenter.default.removeObserver(dropObserver)
        mock.drops = false
        mock.live.append(english)
        mock.exportFailsAfterRemove = true
        check("a failed reload reports every target",
              storeError(stale.remove([tricky.id, english.id])) == .changeFailed(2))
        mock.exportFailsAfterRemove = false
        let partialMock = MockLearnedWords([tricky])
        partialMock.saves = true
        let upgrading = LearnedWordStore(operations: partialMock.operations)
        let fullTricky = LearnedWord(dictionary: tricky.dictionary, text: tricky.text,
                                     tokens: tricky.tokens, languages: tricky.languages,
                                     count: 30_000, fullMatch: true)
        check("import upgrades a partial match to a full match",
              (try? upgrading.importText(LearnedWordStore.exportText([fullTricky])).get())
                == .init(updated: 1) && partialMock.live.first?.fullMatch == true)
        let samePartial = MockLearnedWords([tricky])
        samePartial.saves = true
        let sameStore = LearnedWordStore(operations: samePartial.operations)
        let sameFull = LearnedWord(dictionary: tricky.dictionary, text: tricky.text,
                                   tokens: tricky.tokens, languages: tricky.languages,
                                   count: tricky.count, fullMatch: true)
        check("import upgrades the match type at the same count",
              (try? sameStore.importText(LearnedWordStore.exportText([sameFull])).get())
                == .init(updated: 1) && samePartial.live.first?.fullMatch == true &&
              samePartial.live.first?.count == tricky.count)
        var huge = sameFull
        huge.count = 150_000
        check("imported counts are capped at the editable maximum",
              LearnedWordStore.parse(LearnedWordStore.exportText([huge]))?.words.first?
                .count == LearnedWordStore.countRange.upperBound)
        let unavailable = LearnedWordStore(operations: LearnedWordOperations(
            export: { nil }, remove: { _ in true },
            setCount: { _, _, _ in true }, persist: { true }))
        check("unavailable dictionary reports a load failure",
              storeError(unavailable.reload()) == .loadFailed && !unavailable.loaded)
    }
}

/// An in-memory learned-word dictionary for store tests.
private final class MockLearnedWords {
    var live: [LearnedWord]
    var setCounts: [Int] = []
    var applies = true
    var saves = false
    var removable: (LearnedWord) -> Bool = { _ in true }
    var drops = false
    var exportFailsAfterRemove = false
    private var exportFails = false

    init(_ words: [LearnedWord]) { live = words }

    var operations: LearnedWordOperations {
        LearnedWordOperations(
            export: { [unowned self] in
                defer { self.exportFails = false }
                return self.exportFails ? nil : self.live
            },
            remove: { [unowned self] word in
                guard self.removable(word) else { return false }
                if self.exportFailsAfterRemove { self.exportFails = true }
                self.live.removeAll { $0.id == word.id }
                return true
            },
            setCount: { [unowned self] word, count, fullMatch in
                self.setCounts.append(count)
                if self.drops {
                    self.live.removeAll { $0.id == word.id }
                    return false
                }
                guard self.applies else { return true }
                let stored = LearnedWord(dictionary: word.dictionary, text: word.text,
                                         tokens: word.tokens, languages: word.languages,
                                         count: count, fullMatch: fullMatch)
                if let i = self.live.firstIndex(where: { $0.id == word.id }) {
                    self.live[i] = stored
                } else {
                    self.live.append(stored)
                }
                return true
            },
            persist: { [unowned self] in self.saves })
    }
}

func testPreferenceSessionUpdates() {
    print("[test_preference_session_updates]")
    withTestPreferences { preferences, _ in
        let (session, delegate) = makeSession(preferences: preferences)
        for ch in "nihao" { _ = session.appendLetter(String(ch)) }
        _ = session.selectCurrent()
        check("default preference produces predictions", session.isPredicting)
        preferences.predictionsEnabled = false
        check("disabling predictions immediately clears the window",
              !session.isPredicting && delegate.shownCandidates.isEmpty)
        check("disabled predictions let selection keys pass through",
              session.selectNumber(1) == .passThrough &&
              session.selectCurrent() == .passThrough)
        session.showPredictions(context: "生日快")
        check("disabled predictions cannot be explicitly regenerated", !session.isPredicting)

        delegate.clear()
        for ch in "nihao" { _ = session.appendLetter(String(ch)) }
        _ = session.selectCurrent()
        check("disabled predictions do not change committed text",
              delegate.committedText == "你好" && !session.isPredicting)
        preferences.predictionsEnabled = true
        check("enabling predictions does not invent a pending commit", !session.isPredicting)
        for ch in "nihao" { _ = session.appendLetter(String(ch)) }
        _ = session.selectCurrent()
        check("reenabling predictions takes effect on the next commit", session.isPredicting)
        session.dismissPredictions()

        weak var releasedSession: PinyinSession?
        do {
            let temporary = PinyinSession(preferences: preferences)
            releasedSession = temporary
        }
        check("preference observation does not retain sessions", releasedSession == nil)
    }
}

func testContextPreference() {
    print("[test_context_preference]")
    withTestPreferences { preferences, _ in
        preferences.contextEnabled = false
        let (session, delegate) = makeSession(preferences: preferences)
        delegate.contextBeforeInput = "我对这里很"
        for ch in "bushu" { _ = session.appendLetter(String(ch)) }
        check("disabled context does not query the editor", delegate.contextRequestCount == 0)
        check("disabled context preserves context-free ranking", session.candidates.first == "部署")
        session.cancel()

        preferences.contextEnabled = true
        for ch in "bushu" { _ = session.appendLetter(String(ch)) }
        check("enabled context reads once and improves ranking",
              delegate.contextRequestCount == 1 && session.candidates.first == "不熟")
        let composition = session.composition
        preferences.contextEnabled = false
        check("disabling context reranks the active composition",
              session.candidates.first == "部署")
        check("disabling context neither commits nor loses composition",
              session.composition == composition && delegate.committedText.isEmpty)
        check("disabling context does not read the editor", delegate.contextRequestCount == 1)
        session.cancel()

        delegate.contextBeforeInput = "生日"
        for ch in "kuai" { _ = session.appendLetter(String(ch)) }
        _ = session.selectCurrent()
        let contextFreePredictions = session.predictions
        check("context-free commits still offer predictions", session.isPredicting)
        session.dismissPredictions()
        preferences.contextEnabled = true
        for ch in "kuai" { _ = session.appendLetter(String(ch)) }
        _ = session.selectCurrent()
        check("enabled prediction context includes editor text",
              session.predictions.first == "乐" && session.predictions != contextFreePredictions)
        preferences.contextEnabled = false
        check("disabling context dismisses predictions made from old editor text",
              !session.isPredicting && delegate.shownCandidates.isEmpty)
    }
}

func testCandidatePageSizePreference() {
    print("[test_candidate_page_size_preference]")
    withTestPreferences { preferences, _ in
        let (session, delegate) = makeSession(preferences: preferences)
        for size in CandidatePageSize.allCases {
            preferences.candidatePageSize = .nine
            for ch in "shi" { _ = session.appendLetter(String(ch)) }
            var reference = session.candidates
            _ = session.nextPage()
            reference += session.candidates
            check("\(size.rawValue)-item test has enough reference candidates",
                  reference.count >= 18)
            _ = session.previousPage()
            preferences.candidatePageSize = size
            check("\(size.rawValue) candidates are shown",
                  session.candidates == Array(reference.prefix(size.rawValue)))
            _ = session.nextPage()
            check("\(size.rawValue)-item paging uses the correct offset",
                  session.candidates.first == reference[size.rawValue])
            let expected = session.candidates.first ?? ""
            _ = session.selectNumber(1)
            check("\(size.rawValue)-item selection commits the current page",
                  delegate.committedText == expected)
            delegate.clear()
            session.cancel()
        }

        preferences.candidatePageSize = .nine
        for ch in "shi" { _ = session.appendLetter(String(ch)) }
        let firstPage = session.candidates
        _ = session.nextPage()
        preferences.candidatePageSize = .five
        check("changing page size resets the active candidate page safely",
              session.candidatePage == 0 && session.selectedIndex == 0 &&
              session.candidates == Array(firstPage.prefix(5)))
        for number in 6...9 {
            check("unavailable candidate \(number) is consumed without changing composition",
                  session.selectNumber(number) == .handled &&
                  delegate.committedText.isEmpty && session.composition == "shi")
        }
        session.cancel()

        preferences.candidatePageSize = .nine
        session.showPredictions(context: "生日快")
        let predictions = session.predictions
        check("prediction page-size test has more than one page", predictions.count > 9)
        session.nextPredictionPage()
        preferences.candidatePageSize = .five
        check("changing page size immediately repages predictions",
              session.predictionPage == 0 &&
              delegate.shownCandidates == Array(predictions.prefix(5)))
        session.nextPredictionPage()
        check("prediction paging uses the configured offset",
              delegate.shownCandidates == Array(predictions.dropFirst(5).prefix(5)))
        let expected = session.visiblePredictions.first ?? ""
        _ = session.selectNumber(1)
        check("prediction selection uses the configured page", delegate.committedText == expected)
        session.dismissPredictions()
    }
}

func testCorrectionStylePreference() {
    print("[test_correction_style_preference]")
    withTestPreferences { preferences, _ in
        let (session, delegate) = makeSession(preferences: preferences)
        for ch in "nihoa" { _ = session.appendLetter(String(ch)) }
        let candidates = session.candidates
        let corrected = delegate.candidateReading.text
        let originalReading = delegate.candidateReading
        check("default correction style retains strikeout spans",
              originalReading.spans.contains(where: \.isTypo))

        preferences.correctionStyle = .color
        check("color-only style removes extra letters but retains highlights",
              delegate.candidateReading.displayText == corrected &&
              delegate.candidateReading.hasCorrection &&
              !delegate.candidateReading.spans.contains(where: \.isTypo))
        preferences.correctionStyle = .hidden
        check("hidden marks show corrected pinyin without highlighting",
              delegate.candidateReading.displayText == corrected &&
              !delegate.candidateReading.hasCorrection)
        preferences.correctionStyle = .edits
        check("changing back immediately restores correction marks",
              delegate.candidateReading == originalReading)
        check("correction appearance does not change composition or candidates",
              session.composition == "nihoa" && session.candidates == candidates &&
              delegate.committedText.isEmpty)
        preferences.correctionStyle = .hidden
        _ = session.selectCurrent()
        check("hiding marks does not disable correction", delegate.committedText == "你好")
        session.dismissPredictions()
    }
}

func testCorrectionAlignmentPreference() {
    print("[test_correction_alignment_preference]")
    withTestPreferences { preferences, defaults in
        let (session, delegate) = makeSession(preferences: preferences)
        for ch in "nihoa" { _ = session.appendLetter(String(ch)) }
        let candidates = session.candidates
        let selectedIndex = session.selectedIndex
        let originalReading = delegate.candidateReading
        check("default LCS retains the previous transposition marks",
              originalReading.displayText == "ni'hoao" &&
              originalReading.spans.filter(\.isTypo).map(\.text) == ["o"])

        for style in CorrectionStyle.allCases {
            preferences.correctionStyle = style
            preferences.correctionAlignment = .dl
            let reading = delegate.candidateReading
            check("\(style.rawValue) DL immediately shows corrected order without deletions",
                  reading.displayText == "ni'hao" &&
                  !reading.spans.contains(where: \.isTypo))
            check("\(style.rawValue) DL respects the highlight preference",
                  reading.spans.filter(\.isCorrected).map(\.text) ==
                      (style == .hidden ? [] : ["ao"]))
            preferences.correctionAlignment = .lcs
            check("\(style.rawValue) returning to LCS restores the original style",
                  delegate.candidateReading == originalReading.styled(style))
        }
        preferences.correctionAlignment = .dl
        check("alignment changes preserve composition, candidate order and selection",
              session.composition == "nihoa" && session.candidates == candidates &&
              session.selectedIndex == selectedIndex && delegate.committedText.isEmpty)
        check("alignment and style persist independently",
              Preferences(defaults: defaults).correctionAlignment == .dl &&
              Preferences(defaults: defaults).correctionStyle == .hidden)
        _ = session.selectCurrent()
        check("DL alignment does not change committed text", delegate.committedText == "你好")
        session.dismissPredictions()
    }
}

func testShiftTogglePreference() {
    print("[test_shift_toggle_preference]")
    let tracker = ShiftToggleTracker()
    _ = tracker.handleFlagsChanged(keyCode: 56, modifierFlags: .shift)
    check("Shift toggles by default",
          tracker.handleFlagsChanged(keyCode: 56, modifierFlags: []) == .shouldToggle)
    _ = tracker.handleFlagsChanged(keyCode: 56, modifierFlags: .shift)
    tracker.isEnabled = false
    check("disabling Shift discards a pending press",
          tracker.handleFlagsChanged(keyCode: 56, modifierFlags: []) == .none)
    tracker.handleKeyDown(keyCode: 60, modifierFlags: .shift)
    check("disabled Shift key-up does not toggle",
          tracker.handleKeyUp(keyCode: 60) == .none)
    _ = tracker.handleFlagsChanged(keyCode: 56, modifierFlags: .shift)
    tracker.isEnabled = true
    check("reenabling does not revive an ignored press",
          tracker.handleFlagsChanged(keyCode: 56, modifierFlags: []) == .none)
    _ = tracker.handleFlagsChanged(keyCode: 60, modifierFlags: .shift)
    check("reenabled right Shift toggles normally",
          tracker.handleKeyUp(keyCode: 60) == .shouldToggle)
    _ = tracker.handleFlagsChanged(keyCode: 56, modifierFlags: .shift)
    tracker.handleKeyDown(keyCode: 0, modifierFlags: .shift)
    check("Shift combinations still do not toggle",
          tracker.handleFlagsChanged(keyCode: 56, modifierFlags: []) == .none)

    // Command + Shift + F does not trigger bare-Shift toggle
    tracker.cancelTracking()
    _ = tracker.handleFlagsChanged(keyCode: 55, modifierFlags: .command)
    check("Shift down with Command held does not track",
          tracker.handleFlagsChanged(keyCode: 56, modifierFlags: [.command, .shift]) == .none)
    tracker.handleKeyDown(keyCode: 3, modifierFlags: [.command, .shift])
    check("Shift release with Command held does not toggle",
          tracker.handleFlagsChanged(keyCode: 56, modifierFlags: .command) == .none)
    check("Command release does not toggle",
          tracker.handleFlagsChanged(keyCode: 55, modifierFlags: []) == .none)
}

func testThemeAndTraditionalPreferences() {
    print("[test_theme_and_traditional_preferences]")
    withTestPreferences { preferences, defaults in
        check("Traditional defaults to false", !preferences.isTraditional)
        check("Theme defaults to system", preferences.themeMode == .system)

        preferences.toggleTraditional()
        preferences.themeMode = .dark
        let reloaded = Preferences(defaults: defaults)
        check("Traditional and theme persist",
              reloaded.isTraditional && reloaded.themeMode == .dark)
    }
}

func testCandidateWindowFollowsTheme() {
    print("[test_candidate_window_follows_theme]")
    _ = NSApplication.shared
    withTestPreferences { preferences, _ in
        preferences.themeMode = .light
        let controller = CandidateWindowController(
            preferences: preferences, glassEffectClass: nil)
        controller.update(
            candidates: ["你好"], pinyin: .plain("nihao"),
            canGoPrevious: false, canGoNext: false,
            onSelect: { _ in }, onPrevious: {}, onNext: {}
        )
        check("light theme pins the panel to Aqua",
              controller.window?.appearance?.name == .aqua)
        check("light theme uses the light material",
              controller.visualEffectView?.material == .popover)

        preferences.themeMode = .dark
        check("theme change applies without a candidate update",
              controller.window?.appearance?.name == .darkAqua)
        check("dark theme uses the dark material",
              controller.visualEffectView?.material == .hudWindow)

        preferences.themeMode = .system
        let systemDark = controller.window?.effectiveAppearance
            .bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        check("system theme follows the system appearance",
              controller.window?.appearance == nil &&
              controller.visualEffectView?.material ==
                  (systemDark ? .hudWindow : .popover))
        controller.close()
    }
}

/// Stand-in for `NSGlassEffectView` so the glass path is covered on any OS.
final class FakeGlassEffectView: NSView {
    @objc dynamic var cornerRadius: CGFloat = 0
    @objc dynamic var tintColor: NSColor?
    @objc dynamic var contentView: NSView? {
        didSet {
            oldValue?.removeFromSuperview()
            if let contentView {
                contentView.frame = bounds
                addSubview(contentView)
            }
        }
    }
}

func checkGlassPanel(_ glassClass: NSView.Type, label: String) {
    withTestPreferences { preferences, _ in
        preferences.themeMode = .dark
        let controller = CandidateWindowController(
            preferences: preferences, glassEffectClass: glassClass)
        controller.update(
            candidates: ["你好", "拟好"], pinyin: .plain("nihao"),
            canGoPrevious: false, canGoNext: false,
            onSelect: { _ in }, onPrevious: {}, onNext: {}
        )
        let glass = controller.backgroundView
        let hosted = glass?.value(forKey: "contentView") as? FirstMouseHostingView<CandidateView>
        check("\(label): glass backs the panel",
              glass.map { type(of: $0) == glassClass } == true &&
              controller.window?.contentView === glass &&
              controller.visualEffectView == nil)
        check("\(label): candidates are the glass content view", hosted != nil)
        check("\(label): candidates sit directly on glass",
              hosted?.rootView.glassBacked == true)
        check("\(label): glass uses the panel corner radius",
              (glass?.value(forKey: "cornerRadius") as? CGFloat) ==
                  CandidateWindowController.glassCornerRadius)
        if glass?.responds(to: NSSelectorFromString("tintColor")) == true {
            check("\(label): glass is untinted",
                  glass?.value(forKey: "tintColor") == nil)
        }
        check("\(label): window server shadows the glass panel",
              controller.window?.hasShadow == true)
        check("\(label): theme still pins the glass panel appearance",
              controller.window?.appearance?.name == .darkAqua)
        let many = ["一", "二", "三", "四", "五", "六", "七", "八", "九"]
        controller.update(
            candidates: many, pinyin: .empty,
            canGoPrevious: false, canGoNext: true,
            onSelect: { _ in }, onPrevious: {}, onNext: {}
        )
        let row = NSHostingView(rootView: CandidateView(
            candidates: many, pinyin: .empty, selectedIndex: 0,
            canGoPrevious: false, canGoNext: true,
            onSelect: { _ in }, onPrevious: {}, onNext: {},
            glassBacked: true, preferences: preferences
        ).fixedSize())
        check("\(label): glass window fits every candidate and the page arrows",
              row.fittingSize.width <= (controller.window?.contentLayoutRect.width ?? 0))
        controller.update(
            candidates: ["你好"], pinyin: .empty,
            canGoPrevious: false, canGoNext: false,
            onSelect: { _ in }, onPrevious: {}, onNext: {}
        )
        hosted?.superview?.layoutSubtreeIfNeeded()
        check("\(label): glass panel keeps the requested height",
              controller.backgroundView === glass &&
              controller.window?.contentLayoutRect.size.height == 40 &&
              hosted?.frame.size.height == 40)
        controller.close()
    }
}

func testCandidateWindowUsesLiquidGlass() {
    print("[test_candidate_window_uses_liquid_glass]")
    _ = NSApplication.shared
    checkGlassPanel(FakeGlassEffectView.self, label: "fake glass")
    if let glassClass = CandidateWindowController.systemGlassEffectClass {
        checkGlassPanel(glassClass, label: "Liquid Glass")
    } else {
        check("frosted fallback without NSGlassEffectView",
              NSClassFromString("NSGlassEffectView") == nil)
    }
    withTestPreferences { preferences, _ in
        let controller = CandidateWindowController(
            preferences: preferences, glassEffectClass: nil)
        controller.update(
            candidates: ["你好"], pinyin: .plain("nihao"),
            canGoPrevious: false, canGoNext: false,
            onSelect: { _ in }, onPrevious: {}, onNext: {}
        )
        let hosted = controller.visualEffectView?.subviews.first
            as? FirstMouseHostingView<CandidateView>
        check("frosted fallback keeps themed fills and the window shadow",
              controller.visualEffectView != nil &&
              hosted?.rootView.glassBacked == false &&
              controller.window?.hasShadow == true)
        controller.close()
    }
}

func testLegacyTraditionalKeyIgnored() {
    print("[test_legacy_traditional_key_ignored]")
    withTestPreferences { _, defaults in
        defaults.set(true, forKey: "GboardIME_isTraditional")
        check("unreleased legacy key does not enable Traditional",
              !Preferences(defaults: defaults).isTraditional)
    }
}

func testCandidateWindowOpacity() {
    print("[test_candidate_window_opacity]")
    withTestPreferences { preferences, defaults in
        check("window opacity defaults to 70%",
              preferences.candidateWindowOpacity == 0.7)
        var notified = false
        let observer = NotificationCenter.default.addObserver(
            forName: Preferences.didChangeNotification, object: preferences, queue: nil
        ) { _ in notified = true }
        preferences.candidateWindowOpacity = 0.6
        NotificationCenter.default.removeObserver(observer)
        check("window opacity change is announced", notified)
        check("window opacity persists",
              Preferences(defaults: defaults).candidateWindowOpacity == 0.6)
        defaults.set(-0.5, forKey: "candidateWindowOpacity")
        check("negative stored opacity is raised to 0%",
              Preferences(defaults: defaults).candidateWindowOpacity == 0)
        defaults.set(3.0, forKey: "candidateWindowOpacity")
        check("too-high stored opacity is limited to 100%",
              Preferences(defaults: defaults).candidateWindowOpacity == 1.0)
        defaults.set(Double.nan, forKey: "candidateWindowOpacity")
        check("non-finite stored opacity falls back to 70%",
              Preferences(defaults: defaults).candidateWindowOpacity == 0.7)
        defaults.set("dim", forKey: "candidateWindowOpacity")
        check("non-numeric stored opacity falls back to 70%",
              Preferences(defaults: defaults).candidateWindowOpacity == 0.7)
        var clampNotifications = 0
        let clampObserver = NotificationCenter.default.addObserver(
            forName: Preferences.didChangeNotification, object: preferences, queue: nil
        ) { _ in clampNotifications += 1 }
        preferences.candidateWindowOpacity = -0.2
        NotificationCenter.default.removeObserver(clampObserver)
        check("assigned opacity is limited to 0%",
              preferences.candidateWindowOpacity == 0)
        check("clamped opacity is announced once", clampNotifications == 1)
        check("clamped opacity persists",
              Preferences(defaults: defaults).candidateWindowOpacity == 0)
        preferences.candidateWindowOpacity = .nan
        check("assigned non-finite opacity falls back to 70%",
              preferences.candidateWindowOpacity == 0.7)
    }
}

func testPinyinTextAttributes() {
    print("[test_pinyin_text_attributes]")
    let reading = PinyinReading(spans: [
        .init(text: "o", isCorrected: false),
        .init(text: "o", isCorrected: false, isTypo: true),
        .init(text: "o", isCorrected: true)
    ])
    check("pinyin uses the same 13-point font for measuring and drawing",
          PinyinReading.displayNSFont.pointSize == 13 &&
          PinyinReading.displayFont == Font(PinyinReading.displayNSFont))
    for style in CorrectionStyle.allCases {
        let styled = reading.styled(style)
        let attributed = styled.attributedText
        check("\(style.rawValue) attributed text preserves the displayed letters",
              String(attributed.characters) == styled.displayText)
        check("\(style.rawValue) all color runs have the same explicit font",
              attributed.runs.allSatisfy {
                  $0[AttributeScopes.SwiftUIAttributes.FontAttribute.self]
                      == PinyinReading.displayFont
              })
        check("\(style.rawValue) all color runs use the same zero baseline offset",
              attributed.runs.allSatisfy {
                  $0[AttributeScopes.SwiftUIAttributes.BaselineOffsetAttribute.self] == 0
              })
        let strikeoutRuns = attributed.runs.filter {
            $0[AttributeScopes.SwiftUIAttributes.StrikethroughStyleAttribute.self] != nil
        }
        check("\(style.rawValue) only deleted letters receive a strikeout",
              strikeoutRuns.count == (style == .edits ? 1 : 0))
        check("\(style.rawValue) every letter has an explicit color",
              attributed.runs.allSatisfy {
                  $0[AttributeScopes.SwiftUIAttributes.ForegroundColorAttribute.self] != nil
              })
    }
}

private func failure<T>(_ result: Result<T, ShortcutStore.StoreError>) -> ShortcutStore.StoreError? {
    if case .failure(let error) = result { return error }
    return nil
}

private func withTemporaryDirectory(_ body: (URL) -> Void) {
    let url = FileManager.default.temporaryDirectory
        .appendingPathComponent("gboard-shortcuts-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: url) }
    body(url)
}

func testShortcutStore() {
    print("[test_shortcut_store]")
    withTemporaryDirectory { directory in
        let fileURL = directory.appendingPathComponent("data/shortcuts.jsonl")
        var applied: [[Shortcut]] = []
        var replaceSucceeds = true
        var engineReady = true
        let operations = ShortcutOperations(
            isEngineReady: { engineReady },
            replace: { applied.append($0); return replaceSucceeds },
            isValid: { gboard_shortcut_is_valid($0, $1) })
        let store = ShortcutStore(fileURL: fileURL, operations: operations)
        check("missing shortcut file loads as empty",
              (try? store.loadAndSync().get()) != nil && store.loaded &&
              store.shortcuts.isEmpty && applied == [[]])

        check("shortcut input is normalized",
              (try? store.add(shortcut: " BJ ", phrase: " 北京市 ").get()) != nil &&
              store.shortcuts == [Shortcut(shortcut: "bj", phrase: "北京市")] &&
              applied.last == store.shortcuts)
        check("one shortcut may expand to several phrases",
              (try? store.add(shortcut: "bj", phrase: "北京欢迎你").get()) != nil &&
              store.shortcuts.count == 2)
        check("ASCII-only phrases are accepted",
              (try? store.add(shortcut: "yx", phrase: "me@example.com").get()) != nil)
        check("duplicate shortcuts are rejected",
              failure(store.add(shortcut: "bj", phrase: "北京市")) == .duplicate &&
              store.shortcuts.count == 3)
        let invalid = [("b1", "北京"), ("", "北京"), ("bj", ""), ("bj", "北\n京"),
                       (String(repeating: "a", count: Int(HMM_SHORTCUT_MAX_LETTERS) + 1), "长"),
                       ("bj", String(repeating: "长", count: ShortcutStore.maxPhraseLength + 1))]
        check("invalid shortcuts are rejected before reaching the engine",
              invalid.allSatisfy { failure(store.add(shortcut: $0.0, phrase: $0.1)) == .invalid } &&
              applied.count == 4)

        let reloaded = ShortcutStore(fileURL: fileURL, operations: operations)
        check("saved shortcuts reload in order",
              (try? reloaded.load().get()) != nil && reloaded.shortcuts == store.shortcuts)

        let original = Shortcut(shortcut: "bj", phrase: "北京市")
        check("shortcuts can be edited in place",
              (try? store.update(original, shortcut: "bjs", phrase: "北京市").get()) != nil &&
              store.shortcuts.first == Shortcut(shortcut: "bjs", phrase: "北京市"))
        check("editing into an existing shortcut is rejected",
              failure(store.update(Shortcut(shortcut: "bjs", phrase: "北京市"),
                           shortcut: "bj", phrase: "北京欢迎你")) == .duplicate)
        check("editing a removed shortcut reports it missing",
              failure(store.update(original, shortcut: "bj", phrase: "北京")) == .notFound)

        replaceSucceeds = false
        let beforeFailure = store.shortcuts
        applied.removeAll()
        check("engine failure leaves shortcuts unchanged",
              failure(store.add(shortcut: "sh", phrase: "上海市")) == .applyFailed &&
              store.shortcuts == beforeFailure &&
              (try? ShortcutStore(fileURL: fileURL, operations: operations).load().get()) != nil)
        check("engine failure restores the previous list",
              applied.count == 2 && applied.last == beforeFailure)
        check("a failed restore is reported", store.needsEngineSync)
        replaceSucceeds = true
        check("resyncing clears the warning",
              (try? store.loadAndSync().get()) != nil && !store.needsEngineSync &&
              applied.last == beforeFailure)

        engineReady = false
        let appliedBefore = applied.count
        check("shortcuts can be edited before the engine is ready",
              (try? store.add(shortcut: "sh", phrase: "上海市").get()) != nil &&
              applied.count == appliedBefore)
        engineReady = true

        let exported = ShortcutStore.exportText(store.shortcuts)
        check("export starts with a format header",
              exported.hasPrefix("{\"format\":\"gboardime-shortcuts\",\"version\":1}\n") &&
              exported.contains("{\"shortcut\":\"yx\",\"phrase\":\"me@example.com\"}"))
        check("remove deletes selected shortcuts",
              (try? store.remove([Shortcut(shortcut: "sh", phrase: "上海市"),
                                  Shortcut(shortcut: "yx", phrase: "me@example.com")]).get()) != nil &&
              store.shortcuts.count == 2)
        let imported = "\u{FEFF}" + exported.replacingOccurrences(of: "\n", with: "\r\n") +
            "{\"shortcut\":\"x1\",\"phrase\":\"坏\"}\nnot json\n"
        if case .success(let summary) = store.importText(imported) {
            check("import adds missing shortcuts and skips existing ones",
                  summary == ShortcutStore.ImportSummary(added: 2, unchanged: 2, invalid: 2) &&
                  store.shortcuts.count == 4 && applied.last == store.shortcuts)
        } else {
            check("import adds missing shortcuts and skips existing ones", false)
        }
        let appliedBeforeNoop = applied.count
        check("importing only existing shortcuts changes nothing",
              (try? store.importText(exported).get())?.added == 0 &&
              applied.count == appliedBeforeNoop)
        check("files without the header are rejected",
              failure(store.importText("{\"shortcut\":\"bj\",\"phrase\":\"北京\"}\n")) ==
                  .notShortcutList)

        let partial = ShortcutStore.exportText([Shortcut(shortcut: "bj", phrase: "北京")]) +
            "{\"shortcut\":\"b1\",\"phrase\":\"坏\"}\nnot json\n"
        try? partial.write(to: fileURL, atomically: true, encoding: .utf8)
        let damaged = ShortcutStore(fileURL: fileURL, operations: operations)
        check("unreadable lines are counted on load",
              (try? damaged.load().get()) != nil && damaged.shortcuts.count == 1 &&
              damaged.skippedLines == 2)
        check("saving clears the unreadable-line count",
              (try? damaged.add(shortcut: "sh", phrase: "上海").get()) != nil &&
              damaged.skippedLines == 0)

        try? "garbage".write(to: fileURL, atomically: true, encoding: .utf8)
        let corrupt = ShortcutStore(fileURL: fileURL, operations: operations)
        check("unreadable files are never overwritten",
              failure(corrupt.load()) == .loadFailed &&
              failure(corrupt.add(shortcut: "bj", phrase: "北京")) == .loadFailed &&
              (try? String(contentsOf: fileURL, encoding: .utf8)) == "garbage")

        let blocked = directory.appendingPathComponent("blocked")
        try? "file".write(to: blocked, atomically: true, encoding: .utf8)
        let unsaved = ShortcutStore(fileURL: blocked.appendingPathComponent("shortcuts.jsonl"),
                                    operations: operations)
        _ = unsaved.load()
        applied.removeAll()
        check("save failure restores the engine",
              failure(unsaved.add(shortcut: "bj", phrase: "北京")) == .saveFailed &&
              unsaved.shortcuts.isEmpty &&
              applied == [[Shortcut(shortcut: "bj", phrase: "北京")], []] &&
              !unsaved.needsEngineSync)
    }
}

func testShortcutsPreference() {
    print("[test_shortcuts_preference]")
    withTestPreferences { preferences, defaults in
        check("shortcuts default to disabled", !preferences.shortcutsEnabled)
        var requested: [Bool] = []
        check("enabling shortcuts applies and saves",
              preferences.applyShortcuts(true, using: { requested.append($0); return true }) &&
              preferences.shortcutsEnabled && Preferences(defaults: defaults).shortcutsEnabled)
        check("a failed change keeps the setting",
              !preferences.applyShortcuts(false, using: { _ in false }) &&
              preferences.shortcutsEnabled && !preferences.shortcutsApplyFailed)
        check("a failed restore is reported for retry",
              !preferences.applyShortcuts(using: { _ in false }) &&
              preferences.shortcutsApplyFailed)
        check("a successful retry clears the failure",
              preferences.applyShortcuts(using: { requested.append($0); return true }) &&
              !preferences.shortcutsApplyFailed && requested == [true, true])
    }
}

/// Needs a user data directory, so it runs in its own process.
func testShortcutsSession() {
    print("[test_shortcuts_session]")
    withTemporaryDirectory { directory in
        withTestPreferences { preferences, _ in
            preferences.contextEnabled = false
            preferences.predictionsEnabled = false
            let store = ShortcutStore(fileURL: directory.appendingPathComponent("shortcuts.jsonl"))
            check("session store starts empty", (try? store.loadAndSync().get()) != nil)
            check("session store saves a shortcut",
                  (try? store.add(shortcut: "bjcy", phrase: "北京市朝阳区").get()) != nil)
            let (session, delegate) = makeSession(preferences: preferences)
            for ch in "bjcy" { _ = session.appendLetter(String(ch)) }
            check("disabled shortcuts are not offered",
                  !session.candidates.contains("北京市朝阳区"))
            check("enabling shortcuts succeeds", preferences.applyShortcuts(true))
            check("enabling shortcuts refreshes the active composition",
                  session.candidates.first == "北京市朝阳区" &&
                  delegate.shownCandidates == session.candidates)
            check("adding a shortcut refreshes the active composition",
                  (try? store.add(shortcut: "bjcy", phrase: "北京市朝阳区建国路").get()) != nil &&
                  session.candidates.contains("北京市朝阳区建国路"))
            _ = session.selectCurrent()
            check("selecting a shortcut commits its phrase",
                  delegate.committedText == "北京市朝阳区" && !session.isComposing)
            check("shortcut selections are not learned",
                  !LearnedWordStore().words.contains { $0.text == "北京市朝阳区" })

            delegate.clear()
            preferences.isTraditional = true
            for ch in "bjcy" { _ = session.appendLetter(String(ch)) }
            check("traditional mode keeps shortcut phrases verbatim",
                  session.candidates.first == "北京市朝阳区")
            for ch in "q" { _ = session.appendLetter(String(ch)) }
            check("traditional mode still converts other candidates",
                  !session.candidates.contains("北京市朝阳区去"))
            preferences.isTraditional = false
            session.cancel()

            for ch in "bjcy" { _ = session.appendLetter(String(ch)) }
            check("removing a shortcut refreshes the active composition",
                  (try? store.remove([Shortcut(shortcut: "bjcy", phrase: "北京市朝阳区")]).get()) != nil &&
                  !session.candidates.contains("北京市朝阳区") &&
                  session.candidates.contains("北京市朝阳区建国路"))
            check("disabling shortcuts succeeds", preferences.applyShortcuts(false))
            check("disabling shortcuts refreshes the active composition",
                  !session.candidates.contains("北京市朝阳区建国路"))
            session.cancel()
        }
    }
}
