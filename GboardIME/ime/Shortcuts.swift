import Foundation

/// A user-defined shortcut: typing `shortcut` offers `phrase` as a
/// candidate. Like an entry in Gboard's personal dictionary, the same
/// shortcut may expand to several phrases.
struct Shortcut: Hashable, Identifiable {
    let shortcut: String
    let phrase: String

    var id: Shortcut { self }

    func matches(_ query: String) -> Bool {
        let query = query.trimmingCharacters(in: .whitespaces).lowercased()
        guard !query.isEmpty else { return true }
        return shortcut.contains(query) || phrase.lowercased().contains(query)
    }
}

struct ShortcutOperations {
    /// Whether the native dictionaries can be rebuilt now.
    var isEngineReady: () -> Bool
    /// Rebuilds the native shortcut dictionaries from the complete list.
    var replace: ([Shortcut]) -> Bool
    var isValid: (String, String) -> Bool

    static let native = ShortcutOperations(
        isEngineReady: { gboard_user_dict_is_ready() },
        replace: { shortcuts in
            let strings = shortcuts.map { (strdup($0.shortcut), strdup($0.phrase)) }
            defer { strings.forEach { free($0.0); free($0.1) } }
            var entries = strings.map {
                HmmShortcut(shortcut: UnsafePointer($0.0), phrase: UnsafePointer($0.1))
            }
            return entries.withUnsafeMutableBufferPointer {
                gboard_replace_shortcuts($0.baseAddress, Int32($0.count))
            }
        },
        isValid: { gboard_shortcut_is_valid($0, $1) }
    )
}

/// The source of truth for shortcuts, mirroring Gboard's personal
/// dictionary database: a JSON Lines file synced in full to the native
/// shortcut dictionaries after every change.
final class ShortcutStore: ObservableObject {
    static let shared = ShortcutStore(fileURL: defaultFileURL)
    static let didChangeNotification = Notification.Name("GboardIME.ShortcutsDidChange")

    static let formatName = "gboardime-shortcuts"
    static let formatVersion = 1
    static let maxPhraseLength = 1000

    static var defaultFileURL: URL? {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)
            .first?.appendingPathComponent("GboardIME/shortcuts.jsonl")
    }

    enum StoreError: LocalizedError, Equatable {
        case invalid
        case duplicate
        case notFound
        case loadFailed
        case saveFailed
        case applyFailed
        case notShortcutList

        var errorDescription: String? {
            switch self {
            case .invalid:
                return "缩写只能包含 1–\(HMM_SHORTCUT_MAX_LETTERS) 个英文字母，短语不能为空、不能换行，且不超过 \(ShortcutStore.maxPhraseLength) 个字符。"
            case .duplicate:
                return "已有相同的缩写和短语。"
            case .notFound:
                return "该快捷短语已不存在。"
            case .loadFailed:
                return "无法读取快捷短语文件。"
            case .saveFailed:
                return "无法保存快捷短语文件，已保留原内容。"
            case .applyFailed:
                return "输入法未能更新快捷短语，已保留原内容，请重试。"
            case .notShortcutList:
                return "这不是 GboardIME 快捷短语文件。"
            }
        }
    }

    struct ImportSummary: Equatable {
        var added = 0
        var unchanged = 0
        var invalid = 0
    }

    @Published private(set) var shortcuts: [Shortcut] = []
    @Published private(set) var loaded = false
    /// Set when a failed change could not be undone in the engine, so the
    /// engine may differ from the saved list until the next sync.
    @Published private(set) var needsEngineSync = false
    /// Lines of the saved file that could not be read; the next save
    /// drops them.
    @Published private(set) var skippedLines = 0
    private let fileURL: URL?
    private let operations: ShortcutOperations

    init(fileURL: URL?, operations: ShortcutOperations = .native) {
        self.fileURL = fileURL
        self.operations = operations
    }

    /// Normalizes user input: shortcuts are case-insensitive, and
    /// surrounding whitespace is never intended.
    func normalized(shortcut: String, phrase: String) -> Shortcut? {
        let shortcut = shortcut.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let phrase = phrase.trimmingCharacters(in: .whitespacesAndNewlines)
        guard phrase.count <= Self.maxPhraseLength,
              !phrase.unicodeScalars.contains(where: Self.isControl),
              operations.isValid(shortcut, phrase) else { return nil }
        return Shortcut(shortcut: shortcut, phrase: phrase)
    }

    /// Reads the saved list and rebuilds the native dictionaries from it,
    /// so they always match the file after launch.
    @discardableResult
    func loadAndSync() -> Result<Void, StoreError> {
        let result = load()
        guard case .success = result else { return result }
        guard operations.isEngineReady() else { return .success(()) }
        let synced = operations.replace(shortcuts)
        needsEngineSync = !synced
        NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
        return synced ? .success(()) : .failure(.applyFailed)
    }

    @discardableResult
    func load() -> Result<Void, StoreError> {
        guard let url = fileURL else {
            loaded = false
            return .failure(.loadFailed)
        }
        guard FileManager.default.fileExists(atPath: url.path) else {
            shortcuts = []
            skippedLines = 0
            loaded = true
            return .success(())
        }
        guard let text = try? String(contentsOf: url, encoding: .utf8),
              let parsed = parse(text) else {
            loaded = false
            return .failure(.loadFailed)
        }
        shortcuts = unique(parsed.shortcuts)
        skippedLines = parsed.invalid
        loaded = true
        return .success(())
    }

    func add(shortcut: String, phrase: String) -> Result<Void, StoreError> {
        guard let entry = normalized(shortcut: shortcut, phrase: phrase) else {
            return .failure(.invalid)
        }
        guard !shortcuts.contains(entry) else { return .failure(.duplicate) }
        return commit(shortcuts + [entry])
    }

    func update(_ id: Shortcut.ID, shortcut: String, phrase: String) -> Result<Void, StoreError> {
        guard let index = shortcuts.firstIndex(of: id) else { return .failure(.notFound) }
        guard let entry = normalized(shortcut: shortcut, phrase: phrase) else {
            return .failure(.invalid)
        }
        guard entry != id else { return .success(()) }
        guard !shortcuts.contains(entry) else { return .failure(.duplicate) }
        var updated = shortcuts
        updated[index] = entry
        return commit(updated)
    }

    func remove(_ ids: Set<Shortcut.ID>) -> Result<Void, StoreError> {
        let remaining = shortcuts.filter { !ids.contains($0) }
        guard remaining.count != shortcuts.count else { return .success(()) }
        return commit(remaining)
    }

    /// Applies the list to the engine first, then saves it. Any failure
    /// rebuilds the engine from the previous list, since a failed native
    /// replace may have applied only part of the change.
    private func commit(_ updated: [Shortcut]) -> Result<Void, StoreError> {
        guard loaded, let url = fileURL else { return .failure(.loadFailed) }
        let engineReady = operations.isEngineReady()
        if engineReady && !operations.replace(updated) {
            restoreEngine()
            return .failure(.applyFailed)
        }
        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Self.exportText(updated).write(to: url, atomically: true, encoding: .utf8)
        } catch {
            if engineReady { restoreEngine() }
            return .failure(.saveFailed)
        }
        shortcuts = updated
        skippedLines = 0
        if engineReady { needsEngineSync = false }
        NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
        return .success(())
    }

    private func restoreEngine() {
        if operations.replace(shortcuts) {
            needsEngineSync = false
        } else {
            needsEngineSync = true
            NSLog("Unable to restore %d shortcuts in the engine", shortcuts.count)
        }
        NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
    }

    private func unique(_ entries: [Shortcut]) -> [Shortcut] {
        var seen = Set<Shortcut>()
        return entries.filter { seen.insert($0).inserted }
    }

    // MARK: - Import and export

    private struct FileHeader: Decodable {
        let format: String
        let version: Int
    }

    private struct FileEntry: Decodable {
        let shortcut: String
        let phrase: String
    }

    /// JSON Lines: a header object, then one shortcut per line, in list
    /// order so the file keeps the user's arrangement.
    static func exportText(_ shortcuts: [Shortcut]) -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.withoutEscapingSlashes]
        func value(_ text: String) -> String? {
            (try? encoder.encode(text)).flatMap { String(data: $0, encoding: .utf8) }
        }
        var lines = ["{\"format\":\(value(formatName) ?? "\"\""),\"version\":\(formatVersion)}"]
        // Write keys in a readable fixed order (shortcut before phrase),
        // which JSONEncoder cannot express; encode only the values.
        for entry in shortcuts {
            if let shortcut = value(entry.shortcut), let phrase = value(entry.phrase) {
                lines.append("{\"shortcut\":\(shortcut),\"phrase\":\(phrase)}")
            }
        }
        return lines.joined(separator: "\n") + "\n"
    }

    /// Parses an exported file. Returns nil when the header is missing.
    func parse(_ text: String) -> (shortcuts: [Shortcut], invalid: Int)? {
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
              header.format == Self.formatName, header.version == Self.formatVersion else {
            return nil
        }
        var entries: [Shortcut] = []
        var invalid = 0
        for line in lines.dropFirst() where !line.isEmpty {
            if let entry = try? decoder.decode(FileEntry.self, from: Data(line.utf8)),
               let shortcut = normalized(shortcut: entry.shortcut, phrase: entry.phrase) {
                entries.append(shortcut)
            } else {
                invalid += 1
            }
        }
        return (entries, invalid)
    }

    /// Adds shortcuts missing from the current list; never removes any.
    func importText(_ text: String) -> Result<ImportSummary, StoreError> {
        guard let parsed = parse(text) else { return .failure(.notShortcutList) }
        var summary = ImportSummary(invalid: parsed.invalid)
        var updated = shortcuts
        var existing = Set(shortcuts)
        for entry in parsed.shortcuts {
            if existing.insert(entry).inserted {
                updated.append(entry)
                summary.added += 1
            } else {
                summary.unchanged += 1
            }
        }
        if summary.added > 0, case .failure(let error) = commit(updated) {
            return .failure(error)
        }
        return .success(summary)
    }

    private static func isControl(_ scalar: Unicode.Scalar) -> Bool {
        switch scalar.properties.generalCategory {
        case .control, .lineSeparator, .paragraphSeparator: return true
        default: return false
        }
    }
}
