import Foundation

struct SessionInputContext {
    let beforeSelection: String
    let selectedText: String
    let afterSelection: String
}

enum SessionContextRetriever {
    static let maximumUTF16Length = 20
    static let maximumSnapshotUTF16Length = 50

    static func retrieve(
        selection: NSRange,
        attributedSubstring: (NSRange) -> NSAttributedString?
    ) -> String? {
        retrieveBeforeSelection(
            selection: selection,
            maximumLength: maximumUTF16Length,
            attributedSubstring: attributedSubstring
        )
    }

    static func retrieveInputContext(
        selection: NSRange,
        attributedSubstring: (NSRange) -> NSAttributedString?
    ) -> SessionInputContext? {
        guard selection.location != NSNotFound else { return nil }

        let before = retrieveBeforeSelection(
            selection: selection,
            maximumLength: maximumSnapshotUTF16Length,
            attributedSubstring: attributedSubstring
        )
        guard let before else { return nil }

        let selected: String
        if selection.length == 0 {
            selected = ""
        } else {
            guard let value = attributedSubstring(selection)?.string else {
                return nil
            }
            selected = value
        }

        let after = retrieveAfterSelection(
            selection: selection,
            maximumLength: maximumSnapshotUTF16Length,
            attributedSubstring: attributedSubstring
        ) ?? ""
        return SessionInputContext(
            beforeSelection: before,
            selectedText: selected,
            afterSelection: after
        )
    }

    private static func retrieveBeforeSelection(
        selection: NSRange,
        maximumLength: Int,
        attributedSubstring: (NSRange) -> NSAttributedString?
    ) -> String? {
        guard selection.location != NSNotFound else { return nil }
        guard selection.location > 0 else { return "" }

        let maximumRequestLength = min(
            maximumLength + 1,
            selection.location
        )
        for length in stride(
            from: maximumRequestLength,
            through: 1,
            by: -1
        ) {
            let range = NSRange(
                location: selection.location - length,
                length: length
            )
            guard let text = attributedSubstring(range)?.string else {
                continue
            }

            let suffix = trailingContext(in: text, maximumLength: maximumLength)
            if !suffix.isEmpty {
                return suffix
            }
        }
        return nil
    }

    private static func retrieveAfterSelection(
        selection: NSRange,
        maximumLength: Int,
        attributedSubstring: (NSRange) -> NSAttributedString?
    ) -> String? {
        let start = NSMaxRange(selection)
        for length in stride(
            from: maximumLength + 1,
            through: 1,
            by: -1
        ) {
            let range = NSRange(location: start, length: length)
            guard let text = attributedSubstring(range)?.string else {
                continue
            }
            return leadingContext(in: text, maximumLength: maximumLength)
        }
        return attributedSubstring(NSRange(location: start, length: 0)) == nil
            ? nil
            : ""
    }

    static func trailingContext(in text: String) -> String {
        trailingContext(in: text, maximumLength: maximumUTF16Length)
    }

    private static func trailingContext(
        in text: String,
        maximumLength: Int
    ) -> String {
        let utf16 = text as NSString
        var start = max(0, utf16.length - maximumLength)
        var end = utf16.length

        if start < end, isLowSurrogate(utf16.character(at: start)) {
            start += 1
        }
        if start < end, isHighSurrogate(utf16.character(at: end - 1)) {
            end -= 1
        }

        guard start < end else { return "" }
        return utf16.substring(with: NSRange(location: start, length: end - start))
    }

    private static func leadingContext(
        in text: String,
        maximumLength: Int
    ) -> String {
        let utf16 = text as NSString
        var end = min(utf16.length, maximumLength)
        if end > 0, isHighSurrogate(utf16.character(at: end - 1)) {
            end -= 1
        }
        guard end > 0 else { return "" }
        return utf16.substring(to: end)
    }

    private static func isHighSurrogate(_ unit: unichar) -> Bool {
        unit >= 0xd800 && unit <= 0xdbff
    }

    private static func isLowSurrogate(_ unit: unichar) -> Bool {
        unit >= 0xdc00 && unit <= 0xdfff
    }
}

struct SessionContextTracker {
    private(set) var fallbackContext = ""

    mutating func resolve(clientContext: String?) -> String {
        guard let clientContext else {
            return fallbackContext
        }
        fallbackContext = clientContext
        return clientContext
    }

    mutating func recordInsertedText(_ text: String) {
        fallbackContext = SessionContextRetriever.trailingContext(
            in: fallbackContext + text
        )
    }

    mutating func invalidate() {
        fallbackContext = ""
    }
}
