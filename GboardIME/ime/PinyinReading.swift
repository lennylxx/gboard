import Cocoa
import SwiftUI

/// Pinyin shown above the candidates, including the engine's corrections.
struct PinyinReading: Equatable {
    struct Span: Equatable {
        let text: String
        let isCorrected: Bool
        /// A typed letter the correction drops; shown struck out, not read.
        var isTypo: Bool = false
    }

    var spans: [Span]

    static let empty = PinyinReading(spans: [])
    static let displayNSFont = NSFont.systemFont(ofSize: 13, weight: .medium)
    static let displayFont = Font(displayNSFont)

    static func plain(_ text: String) -> PinyinReading {
        PinyinReading(spans: text.isEmpty ? [] : [Span(text: text, isCorrected: false)])
    }

    /// The corrected reading, without struck-out letters.
    var text: String { spans.filter { !$0.isTypo }.map(\.text).joined() }
    /// Text as rendered, including struck-out letters.
    var displayText: String { spans.map(\.text).joined() }
    var isEmpty: Bool { spans.allSatisfy { $0.text.isEmpty } }
    var hasCorrection: Bool { spans.contains { $0.isCorrected || $0.isTypo } }

    func styled(_ style: CorrectionStyle) -> PinyinReading {
        switch style {
        case .edits: return self
        case .color: return PinyinReading(spans: spans.filter { !$0.isTypo })
        case .hidden: return .plain(text)
        }
    }

    var attributedText: AttributedString {
        let accent = Color(red: 0.102, green: 0.451, blue: 0.910)
        let correction = Color(red: 0.910, green: 0.443, blue: 0.039)
        let typo = Color(red: 0.604, green: 0.627, blue: 0.651)
        var result = AttributedString()
        for span in spans {
            var part = AttributedString(span.text)
            part.font = Self.displayFont
            part.baselineOffset = 0
            part.foregroundColor = span.isTypo ? typo
                : (span.isCorrected ? correction : accent)
            if span.isTypo {
                part.strikethroughStyle = Text.LineStyle(pattern: .solid, color: correction)
            }
            result += part
        }
        return result
    }
}
