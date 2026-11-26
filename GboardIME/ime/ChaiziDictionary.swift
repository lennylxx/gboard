// u-mode character composition (拆字): `u` followed by the pinyin of a
// character's components, e.g. `uniuniuniu` → 犇. Gboard has no such data,
// so this project maintains chaizi_components.tsv (component readings) and
// chaizi_characters.tsv (character splits). scripts/gen_chaizi_tables.py
// compiles them into static arrays, which lookups read in place.

import Foundation

final class ChaiziDictionary {
    struct Match: Equatable {
        let character: String
        /// Readings of the components consumed by the input.
        let readings: [String]
        /// Every component was typed; otherwise the input is a prefix.
        let isComplete: Bool
    }

    static let shared = ChaiziDictionary(gboard_chaizi_tables().pointee)

    private let tables: ChaiziTables

    var count: Int { Int(tables.entry_count) }

    private init(_ tables: ChaiziTables) {
        self.tables = tables
    }

    /// Characters whose components spell `input` (letters and apostrophes,
    /// without the leading `u`): fewest components left first, then readings
    /// typed in full, the primary split, and encoding order.
    func lookup(_ input: String, limit: Int = 100) -> [Match] {
        var letters: [UInt8] = []
        var boundaries: [Int] = []
        for byte in input.lowercased().utf8 {
            if byte == UInt8(ascii: "'") {
                boundaries.append(letters.count)
            } else if byte >= UInt8(ascii: "a") && byte <= UInt8(ascii: "z") {
                letters.append(byte)
            } else {
                return []
            }
        }
        guard letters.count >= 2 else { return [] }

        var scored: [(key: (Int, Int, Int, Int), entry: Int, path: Path)] = []
        let firstLetter = UInt32(1) << UInt32(letters[0] - UInt8(ascii: "a"))
        letters.withUnsafeBufferPointer { letters in
            for entry in 0..<count where tables.entry_first_letters[entry] & firstLetter != 0 {
                var best: (key: (Int, Int, Int, Int), path: Path)?
                let first = Int(tables.entry_split_starts[entry])
                let end = Int(tables.entry_split_starts[entry + 1])
                for split in first..<end {
                    guard let result = match(letters, boundaries, split) else { continue }
                    let key = (result.remaining, result.partial ? 1 : 0, split - first, entry)
                    if best == nil || key < best!.key { best = (key, result.path) }
                }
                if let best { scored.append((best.key, entry, best.path)) }
            }
        }
        scored.sort { $0.key < $1.key }
        return scored.prefix(limit).map { item in
            Match(character: character(item.entry),
                  readings: item.path.map { reading($0.id, prefix: $0.length) },
                  isComplete: item.key.0 == 0)
        }
    }

    /// Toned pinyin for display, e.g. 犇 → bēn.
    static func tonedReading(of character: String) -> String {
        let text = NSMutableString(string: character)
        guard CFStringTransform(text, nil, kCFStringTransformMandarinLatin, false),
              text as String != character else { return "" }
        return text as String
    }

    // MARK: - Private

    /// Reading ids consumed, with how many letters of each were typed.
    private typealias Path = [(id: Int, length: Int)]

    /// Matches the input against one split. Each component consumes one of
    /// its readings; the input may end inside a reading or before the last
    /// components, but at least one component must be typed in full. A
    /// reading typed in full outranks one cut short with as many left.
    private func match(_ letters: UnsafeBufferPointer<UInt8>, _ boundaries: [Int],
                       _ split: Int)
        -> (remaining: Int, partial: Bool, path: Path)? {
        let componentStart = Int(tables.split_starts[split])
        let componentCount = Int(tables.split_starts[split + 1]) - componentStart
        var best: (remaining: Int, partial: Bool, path: Path)?
        var path: Path = []
        func better(_ remaining: Int, _ partial: Bool) -> Bool {
            guard let best else { return true }
            return (remaining, partial ? 1 : 0) < (best.remaining, best.partial ? 1 : 0)
        }
        func visit(_ component: Int, _ position: Int) {
            if position == letters.count {
                guard component > 0 else { return }
                if better(componentCount - component, false) {
                    best = (componentCount - component, false, path)
                }
                return
            }
            guard component < componentCount else { return }
            let id = Int(tables.split_components[componentStart + component])
            let readingsEnd = Int(tables.component_reading_starts[id + 1])
            for index in Int(tables.component_reading_starts[id])..<readingsEnd {
                let readingID = Int(tables.component_reading_ids[index])
                let offset = Int(tables.reading_offsets[readingID])
                let length = Int(tables.reading_offsets[readingID + 1]) - offset
                let available = letters.count - position
                let compared = min(length, available)
                guard memcmp(tables.reading_pool + offset,
                             letters.baseAddress! + position, compared) == 0 else { continue }
                if length <= available {
                    let end = position + length
                    guard !boundaries.contains(where: { $0 > position && $0 < end })
                    else { continue }
                    path.append((readingID, length))
                    visit(component + 1, end)
                    path.removeLast()
                } else if component > 0,
                          !boundaries.contains(where: { $0 > position }),
                          better(componentCount - component, true) {
                    // Input ends inside this reading: one more component counts
                    // as typed only partly.
                    best = (componentCount - component, true, path + [(readingID, available)])
                }
                if best?.remaining == 0 && best?.partial == false { return }
            }
        }
        visit(0, 0)
        return best
    }

    private func character(_ entry: Int) -> String {
        let start = Int(tables.character_starts[entry])
        let end = Int(tables.character_starts[entry + 1])
        return string(tables.character_pool + start, end - start)
    }

    private func reading(_ id: Int, prefix length: Int) -> String {
        string(tables.reading_pool + Int(tables.reading_offsets[id]), length)
    }

    private func string(_ start: UnsafePointer<CChar>, _ count: Int) -> String {
        start.withMemoryRebound(to: UInt8.self, capacity: count) {
            String(decoding: UnsafeBufferPointer(start: $0, count: count), as: UTF8.self)
        }
    }
}
