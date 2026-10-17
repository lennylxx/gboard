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

    init(defaults: UserDefaults) {
        self.defaults = defaults
        defaults.register(defaults: [
            Key.predictions: true,
            Key.context: true,
            Key.shift: true,
            Key.correction: CorrectionStyle.edits.rawValue,
            Key.pageSize: CandidatePageSize.nine.rawValue
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
        super.init()
    }

    private func save(_ value: Any, forKey key: String) {
        defaults.set(value, forKey: key)
        NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
    }
}
