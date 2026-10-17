import Cocoa
import SwiftUI

private func withTestPreferences(_ body: (Preferences, UserDefaults) -> Void) {
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
              preferences.shiftToggleEnabled)
        check("preferences preserve page and correction defaults",
              preferences.candidatePageSize == .nine && preferences.correctionStyle == .edits)

        var notifications = 0
        let observer = NotificationCenter.default.addObserver(
            forName: Preferences.didChangeNotification, object: preferences, queue: nil
        ) { _ in notifications += 1 }
        defer { NotificationCenter.default.removeObserver(observer) }
        preferences.predictionsEnabled = false
        preferences.contextEnabled = false
        preferences.shiftToggleEnabled = false
        preferences.correctionStyle = .color
        preferences.candidatePageSize = .five
        check("each preference change notifies synchronously", notifications == 5)

        let restored = Preferences(defaults: defaults)
        check("disabled switches persist",
              !restored.predictionsEnabled && !restored.contextEnabled &&
              !restored.shiftToggleEnabled)
        check("page size and correction style persist",
              restored.candidatePageSize == .five && restored.correctionStyle == .color)
        restored.correctionStyle = .hidden
        check("hidden correction style persists",
              Preferences(defaults: defaults).correctionStyle == .hidden)

        defaults.set(100, forKey: "candidatePageSize")
        defaults.set("invalid", forKey: "correctionStyle")
        let recovered = Preferences(defaults: defaults)
        check("unrecognized stored options use supported defaults",
              recovered.candidatePageSize == .nine && recovered.correctionStyle == .edits)
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
        check("default correction style retains strikeout spans",
              delegate.candidateReading.spans.contains(where: \.isTypo))

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
        check("changing back immediately restores strikeouts",
              delegate.candidateReading.spans.contains(where: \.isTypo))
        check("correction appearance does not change composition or candidates",
              session.composition == "nihoa" && session.candidates == candidates &&
              delegate.committedText.isEmpty)
        preferences.correctionStyle = .hidden
        _ = session.selectCurrent()
        check("hiding marks does not disable correction", delegate.committedText == "你好")
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
