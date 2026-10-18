import Combine
import Foundation

enum CorrectionStyle: String, CaseIterable {
    case edits
    case color
    case hidden

    var title: String {
        switch self {
        case .edits: return "删除线与插入高亮"
        case .color: return "仅颜色"
        case .hidden: return "隐藏标记"
        }
    }
}

enum CandidatePageSize: Int, CaseIterable {
    case five = 5, six, seven, eight, nine
}

enum PinyinDisplayMode: String, CaseIterable {
    case candidateWindow
    case inline
    case hidden

    var title: String {
        switch self {
        case .candidateWindow: return "候选窗口"
        case .inline: return "输入框（原生）"
        case .hidden: return "隐藏"
        }
    }

    func markedText(for pinyin: String) -> String {
        guard !pinyin.isEmpty else { return "" }
        return self == .inline ? pinyin : "\u{200B}"
    }
}

enum UserDictionarySaveInterval: Int, CaseIterable {
    case fifteenMinutes = 900
    case thirtyMinutes = 1800
    case oneHour = 3600
    case fourHours = 14400

    var title: String {
        switch self {
        case .fifteenMinutes: return "15 分钟"
        case .thirtyMinutes: return "30 分钟"
        case .oneHour: return "1 小时"
        case .fourHours: return "4 小时"
        }
    }
}

final class Preferences: NSObject, ObservableObject {
    static let shared = Preferences(defaults: .standard)
    static let didChangeNotification = Notification.Name("GboardIME.PreferencesDidChange")

    private let defaults: UserDefaults

    private enum Key {
        static let predictions = "predictionsEnabled"
        static let context = "contextEnabled"
        static let shift = "shiftToggleEnabled"
        static let correction = "correctionStyle"
        static let pageSize = "candidatePageSize"
        static let legacyPinyinBar = "pinyinBarEnabled"
        static let pinyinDisplayMode = "pinyinDisplayMode"
        static let learning = "learningEnabled"
        static let saveInterval = "userDictionarySaveInterval"
    }

    @Published var predictionsEnabled: Bool {
        didSet { save(predictionsEnabled, forKey: Key.predictions) }
    }

    @Published var contextEnabled: Bool {
        didSet { save(contextEnabled, forKey: Key.context) }
    }

    @Published var shiftToggleEnabled: Bool {
        didSet { save(shiftToggleEnabled, forKey: Key.shift) }
    }

    @Published var correctionStyle: CorrectionStyle {
        didSet { save(correctionStyle.rawValue, forKey: Key.correction) }
    }

    @Published var candidatePageSize: CandidatePageSize {
        didSet { save(candidatePageSize.rawValue, forKey: Key.pageSize) }
    }

    @Published var pinyinDisplayMode: PinyinDisplayMode {
        didSet { save(pinyinDisplayMode.rawValue, forKey: Key.pinyinDisplayMode) }
    }

    @Published var learningEnabled: Bool {
        didSet { save(learningEnabled, forKey: Key.learning) }
    }

    @Published var userDictionarySaveInterval: UserDictionarySaveInterval {
        didSet { save(userDictionarySaveInterval.rawValue, forKey: Key.saveInterval) }
    }

    init(defaults: UserDefaults) {
        self.defaults = defaults
        let defaultDisplayMode: PinyinDisplayMode =
            defaults.object(forKey: Key.legacyPinyinBar) as? Bool == false
                ? .hidden : .candidateWindow
        defaults.register(defaults: [
            Key.predictions: true,
            Key.context: true,
            Key.shift: true,
            Key.correction: CorrectionStyle.edits.rawValue,
            Key.pageSize: CandidatePageSize.nine.rawValue,
            Key.pinyinDisplayMode: defaultDisplayMode.rawValue,
            Key.learning: true,
            Key.saveInterval: UserDictionarySaveInterval.fourHours.rawValue
        ])
        predictionsEnabled = defaults.bool(forKey: Key.predictions)
        contextEnabled = defaults.bool(forKey: Key.context)
        shiftToggleEnabled = defaults.bool(forKey: Key.shift)
        correctionStyle = CorrectionStyle(
            rawValue: defaults.string(forKey: Key.correction) ?? ""
        ) ?? .edits
        candidatePageSize = CandidatePageSize(
            rawValue: defaults.integer(forKey: Key.pageSize)
        ) ?? .nine
        pinyinDisplayMode = PinyinDisplayMode(
            rawValue: defaults.string(forKey: Key.pinyinDisplayMode) ?? ""
        ) ?? defaultDisplayMode
        learningEnabled = defaults.bool(forKey: Key.learning)
        userDictionarySaveInterval = UserDictionarySaveInterval(
            rawValue: defaults.integer(forKey: Key.saveInterval)
        ) ?? .fourHours
        super.init()
    }

    private func save(_ value: Any, forKey key: String) {
        defaults.set(value, forKey: key)
        NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
    }
}
