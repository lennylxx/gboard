import Foundation

enum LearnedWordDictionary: Int, CaseIterable {
    case pinyin = 0
    case english = 1

    var title: String { self == .pinyin ? "中文" : "英文" }
    var fileName: String { self == .pinyin ? "pinyin" : "english" }
}

/// A token language: a value of Google's i18n Language enum. These are the
/// only two the user dictionaries accept.
enum LearnedWordLanguage: Int32, CaseIterable {
    case english = 0   // HMM_LANGUAGE_ENGLISH
    case chinese = 16  // HMM_LANGUAGE_CHINESE, zh-CN

    var fileName: String { self == .english ? "english" : "chinese" }
}

/// One learned user-dictionary entry, as exported by the native accessor.
struct LearnedWord: Identifiable, Hashable {
    struct Key: Hashable {
        let dictionary: LearnedWordDictionary
        let tokens: [String]
        let languages: [LearnedWordLanguage]
        let text: String
    }

    let dictionary: LearnedWordDictionary
    let text: String
    let tokens: [String]
    let languages: [LearnedWordLanguage]
    var count: Int
    /// False when the word was learned from a partial pinyin match.
    let fullMatch: Bool

    var id: Key {
        Key(dictionary: dictionary, tokens: tokens, languages: languages, text: text)
    }
    var reading: String {
        tokens.joined(separator: dictionary == .english ? "" : "'")
    }
    var dictionaryTitle: String { dictionary.title }

    /// Learning stores only all-English keys in the English dictionary,
    /// and splits English readings into one token per letter.
    var isConsistent: Bool {
        !tokens.isEmpty && tokens.count == languages.count &&
            languages.contains(.chinese) == (dictionary == .pinyin) &&
            zip(tokens, languages).allSatisfy { $1 != .english || $0.utf8.count == 1 }
    }

    func matches(_ query: String) -> Bool {
        let query = query.trimmingCharacters(in: .whitespaces).lowercased()
        guard !query.isEmpty else { return true }
        let compact = query.replacingOccurrences(of: "'", with: "")
            .replacingOccurrences(of: " ", with: "")
        return text.lowercased().contains(query) ||
            reading.lowercased().contains(query) ||
            (!compact.isEmpty && tokens.joined().lowercased().contains(compact))
    }
}

struct LearnedWordOperations {
    var export: () -> [LearnedWord]?
    var remove: (LearnedWord) -> Bool
    /// Sets the word's count exactly with the given match type. The word
    /// carries its current count and match type.
    var setCount: (LearnedWord, Int, Bool) -> Bool
    var persist: () -> Bool

    static let native = LearnedWordOperations(
        export: {
            var entries: UnsafeMutablePointer<HmmUserDictEntry>?
            let count = gboard_user_dict_export_entries(&entries)
            guard count >= 0 else { return nil }
            defer { gboard_user_dict_free_entries(entries, count) }
            guard let entries = entries else { return [] }
            return (0..<Int(count)).compactMap { LearnedWord(entries[$0]) }
        },
        remove: { word in
            word.withNativeEntry { gboard_user_dict_remove_entry($0) }
        },
        setCount: { word, count, fullMatch in
            word.withNativeEntry {
                gboard_user_dict_set_entry_count($0, Int32(count), fullMatch)
            }
        },
        persist: { UserDictionary.shared.persist() }
    )
}

extension LearnedWord {
    init?(_ entry: HmmUserDictEntry) {
        guard let value = entry.value,
              let dictionary = LearnedWordDictionary(rawValue: Int(entry.dictionary.rawValue)),
              entry.token_count > 0, let tokens = entry.tokens,
              let languages = entry.languages
        else { return nil }
        var strings: [String] = []
        var tokenLanguages: [LearnedWordLanguage] = []
        for i in 0..<Int(entry.token_count) {
            guard let token = tokens[i],
                  let language = LearnedWordLanguage(rawValue: languages[i]) else {
                return nil
            }
            strings.append(String(cString: token))
            tokenLanguages.append(language)
        }
        self.init(dictionary: dictionary, text: String(cString: value),
                  tokens: strings, languages: tokenLanguages,
                  count: Int(entry.count), fullMatch: entry.full_match)
    }

    func withNativeEntry<R>(_ body: (UnsafePointer<HmmUserDictEntry>) -> R) -> R {
        var tokenPointers = tokens.map { strdup($0) }
        let value = strdup(text)
        defer {
            tokenPointers.forEach { free($0) }
            free(value)
        }
        var nativeLanguages = languages.map(\.rawValue)
        return tokenPointers.withUnsafeMutableBufferPointer { cTokens in
            nativeLanguages.withUnsafeMutableBufferPointer { cLanguages in
                var entry = HmmUserDictEntry(
                    dictionary: HmmUserDictKind(rawValue: UInt32(dictionary.rawValue)),
                    value: value,
                    tokens: cTokens.baseAddress,
                    languages: cLanguages.baseAddress,
                    token_count: Int32(tokens.count),
                    count: Int32(count),
                    full_match: fullMatch)
                return body(&entry)
            }
        }
    }
}

/// Lists and edits learned words; every change is saved immediately.
final class LearnedWordStore: ObservableObject {
    struct ImportSummary: Equatable {
        var added = 0
        var updated = 0
        var unchanged = 0
        var invalid = 0
        var failed = 0
        var saveFailed = false
    }

    enum StoreError: LocalizedError, Equatable {
        case loadFailed
        case changeFailed(Int)
        case saveFailed
        case changeAndSaveFailed(Int)
        case notWordList

        var errorDescription: String? {
            switch self {
            case .loadFailed:
                return "无法读取学习词，请先使用输入法后重试。"
            case .changeFailed(let count):
                return "有 \(count) 个学习词未能更新。"
            case .saveFailed:
                return "更改已生效，但未能保存到磁盘；退出输入法时会再次保存。"
            case .changeAndSaveFailed(let count):
                return "有 \(count) 个学习词未能更新；其余更改已生效，但未能保存到磁盘。"
            case .notWordList:
                return "这不是 GboardIME 学习词文件。"
            }
        }
    }

    static let formatName = "gboardime-learned-words"
    static let formatVersion = 1
    static let countRange = 1...99_999
    /// Guards imports only; learned phrases can be long.
    static let maxTextLength = 4096

    @Published private(set) var words: [LearnedWord] = []
    @Published private(set) var loaded = false
    private let operations: LearnedWordOperations
    private var clearObserver: NSObjectProtocol?

    init(operations: LearnedWordOperations = .native) {
        self.operations = operations
        clearObserver = NotificationCenter.default.addObserver(
            forName: UserDictionary.didClearNotification, object: nil, queue: .main
        ) { [weak self] _ in
            _ = self?.reload()
        }
    }

    deinit {
        if let observer = clearObserver {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    @discardableResult
    func reload() -> Result<Void, StoreError> {
        guard let exported = operations.export() else {
            words = []
            loaded = false
            return .failure(.loadFailed)
        }
        words = exported
        loaded = true
        return .success(())
    }

    // The IME keeps learning while the window is open, so every change
    // starts from the live dictionary rather than the displayed snapshot,
    // and is checked against a fresh export afterwards.
    func remove(_ ids: Set<LearnedWord.ID>) -> Result<Void, StoreError> {
        if case .failure(let error) = reload() { return .failure(error) }
        let targets = words.filter { ids.contains($0.id) }
        guard !targets.isEmpty else { return .success(()) }
        let attempted = targets.filter { operations.remove($0) }.count
        return finishChange(changed: attempted > 0, targets: targets.count) {
            let remaining = Set($0.map(\.id))
            return targets.filter { remaining.contains($0.id) }.count
        }
    }

    func setCount(_ id: LearnedWord.ID, to count: Int) -> Result<Void, StoreError> {
        guard Self.countRange.contains(count) else { return .failure(.changeFailed(1)) }
        if case .failure(let error) = reload() { return .failure(error) }
        guard let word = words.first(where: { $0.id == id }) else {
            return .failure(.changeFailed(1))
        }
        guard word.count != count else { return .success(()) }
        // A failed call may still have removed the old entry.
        _ = operations.setCount(word, count, word.fullMatch)
        return finishChange(changed: true, targets: 1) { words in
            words.first(where: { $0.id == id })?.count == count ? 0 : 1
        }
    }

    private func finishChange(changed: Bool, targets: Int,
                              failures: ([LearnedWord]) -> Int) -> Result<Void, StoreError> {
        let saved = changed ? operations.persist() : true
        let reloaded = reload()
        if changed { Self.postChange() }
        let failed = (try? reloaded.get()) != nil ? failures(words) : targets
        if failed > 0 {
            return .failure(saved ? .changeFailed(failed) : .changeAndSaveFailed(failed))
        }
        return saved ? .success(()) : .failure(.saveFailed)
    }

    private static func postChange() {
        NotificationCenter.default.post(name: UserDictionary.didChangeNotification,
                                        object: nil)
    }

    // MARK: - Import and export

    private struct FileHeader: Decodable {
        let format: String
        let version: Int
    }

    private struct FileEntry: Decodable {
        let dictionary: String
        let text: String
        let reading: [String]
        let languages: [String]
        let count: Int
        let fullMatch: Bool
    }

    /// JSON Lines: a header object, then one learned word per line, in a
    /// stable order so repeated exports of the same words match.
    static func exportText(_ words: [LearnedWord]) -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.withoutEscapingSlashes]
        func line<T: Encodable>(_ value: T) -> String? {
            (try? encoder.encode(value)).flatMap { String(data: $0, encoding: .utf8) }
        }
        var lines = ["{\"format\":\(line(formatName) ?? "\"\""),\"version\":\(formatVersion)}"]
        let ordered = words.sorted {
            if $0.dictionary != $1.dictionary {
                return $0.dictionary.rawValue < $1.dictionary.rawValue
            }
            if $0.count != $1.count { return $0.count > $1.count }
            if $0.reading != $1.reading { return $0.reading < $1.reading }
            if $0.text != $1.text { return $0.text < $1.text }
            return $0.languages.map(\.rawValue).lexicographicallyPrecedes(
                $1.languages.map(\.rawValue))
        }
        // JSONEncoder orders keys by a per-process hash seed, so write the
        // keys in a fixed order and encode only the values.
        for word in ordered {
            let fields: [(String, String?)] = [
                ("dictionary", line(word.dictionary.fileName)),
                ("text", line(word.text)),
                ("reading", line(word.tokens)),
                ("languages", line(word.languages.map(\.fileName))),
                ("count", line(word.count)),
                ("fullMatch", line(word.fullMatch)),
            ]
            let values = fields.compactMap { key, value in value.map { "\"\(key)\":\($0)" } }
            if values.count == fields.count {
                lines.append("{" + values.joined(separator: ",") + "}")
            }
        }
        return lines.joined(separator: "\n") + "\n"
    }

    /// Parses an exported file. Returns nil when the header is missing.
    static func parse(_ text: String) -> (words: [LearnedWord], invalid: Int)? {
        let decoder = JSONDecoder()
        // JSON strings never contain a raw line feed; trim CR for CRLF files.
        var lines = text.components(separatedBy: "\n").map { line -> String in
            var line = line
            if line.hasPrefix("\u{FEFF}") { line.removeFirst() }
            if line.hasSuffix("\r") { line.removeLast() }
            return line.trimmingCharacters(in: .whitespaces)
        }
        while let first = lines.first, first.isEmpty { lines.removeFirst() }
        guard let first = lines.first,
              let header = try? decoder.decode(FileHeader.self, from: Data(first.utf8)),
              header.format == formatName, header.version == formatVersion else {
            return nil
        }
        var words: [LearnedWord] = []
        var invalid = 0
        for line in lines.dropFirst() where !line.isEmpty {
            if let entry = try? decoder.decode(FileEntry.self, from: Data(line.utf8)),
               let word = importedWord(entry) {
                words.append(word)
            } else {
                invalid += 1
            }
        }
        return (words, invalid)
    }

    /// Rejects values the native dictionary never produces before they
    /// reach it.
    private static func importedWord(_ entry: FileEntry) -> LearnedWord? {
        let languages = entry.languages.compactMap { name in
            LearnedWordLanguage.allCases.first { $0.fileName == name }
        }
        guard let dictionary = LearnedWordDictionary.allCases.first(where: {
                  $0.fileName == entry.dictionary }),
              !entry.text.isEmpty, entry.text.count <= maxTextLength,
              !entry.text.unicodeScalars.contains(where: isControl),
              (1...Int(HMM_USER_DICT_MAX_TOKENS)).contains(entry.reading.count),
              languages.count == entry.languages.count,
              entry.reading.allSatisfy(isValidToken),
              entry.count >= countRange.lowerBound else { return nil }
        let word = LearnedWord(dictionary: dictionary, text: entry.text,
                               tokens: entry.reading, languages: languages,
                               count: min(entry.count, countRange.upperBound),
                               fullMatch: entry.fullMatch)
        return word.isConsistent ? word : nil
    }

    private static func isControl(_ scalar: Unicode.Scalar) -> Bool {
        switch scalar.properties.generalCategory {
        case .control, .lineSeparator, .paragraphSeparator: return true
        default: return false
        }
    }

    private static func isValidToken(_ token: String) -> Bool {
        let bytes = token.utf8
        return !bytes.isEmpty && bytes.count <= Int(HMM_USER_DICT_MAX_TOKEN_BYTES) &&
            bytes.allSatisfy { (0x21...0x7E).contains($0) }
    }

    /// Adds missing words and raises lower counts; never lowers a count.
    func importText(_ text: String) -> Result<ImportSummary, StoreError> {
        guard let parsed = Self.parse(text) else { return .failure(.notWordList) }
        if case .failure(let error) = reload() { return .failure(error) }
        var summary = ImportSummary(invalid: parsed.invalid)
        let current = Dictionary(words.map { ($0.id, $0) },
                                 uniquingKeysWith: { $0.count >= $1.count ? $0 : $1 })
        var requested: [LearnedWord.ID: (count: Int, fullMatch: Bool, added: Bool)] = [:]
        var attempted = false
        var unique: [LearnedWord.ID: LearnedWord] = [:]
        var order: [LearnedWord.ID] = []
        for word in parsed.words {
            if let seen = unique[word.id] {
                summary.unchanged += 1
                unique[word.id] = LearnedWord(
                    dictionary: seen.dictionary, text: seen.text, tokens: seen.tokens,
                    languages: seen.languages, count: max(seen.count, word.count),
                    fullMatch: seen.fullMatch || word.fullMatch)
            } else {
                unique[word.id] = word
                order.append(word.id)
            }
        }
        for word in order.compactMap({ unique[$0] }) {
            let existing = current[word.id]
            // A full match outranks a partial one, as in native learning.
            let fullMatch = word.fullMatch || existing?.fullMatch == true
            let count = max(word.count, existing?.count ?? 0)
            if let existing = existing, existing.count == count,
               existing.fullMatch == fullMatch {
                summary.unchanged += 1
                continue
            }
            var target = existing ?? word
            if existing == nil { target.count = 0 }
            attempted = true
            if operations.setCount(target, count, fullMatch) {
                requested[word.id] = (count, fullMatch, existing == nil)
            } else {
                summary.failed += 1
            }
        }
        if attempted {
            summary.saveFailed = !operations.persist()
            Self.postChange()
        }
        let reloaded = (try? reload().get()) != nil
        let live = Dictionary(words.map { ($0.id, $0) },
                              uniquingKeysWith: { $0.count >= $1.count ? $0 : $1 })
        for (id, request) in requested {
            if reloaded, let word = live[id], word.count >= request.count,
               word.fullMatch || !request.fullMatch {
                if request.added { summary.added += 1 } else { summary.updated += 1 }
            } else {
                summary.failed += 1
            }
        }
        return .success(summary)
    }
}
