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

enum FuzzyPinyinGroup: String, CaseIterable {
    case zZh = "z_zh", cCh = "c_ch", sSh = "s_sh"
    case anAng = "an_ang", enEng = "en_eng", inIng = "in_ing"
    case lN = "l_n", fH = "f_h", rL = "r_l", kG = "k_g"
    case ianIang = "ian_iang", uanUang = "uan_uang"

    var title: String { rawValue.replacingOccurrences(of: "_", with: " / ") }

    var mask: UInt32 {
        switch self {
        case .zZh: return UInt32(HMM_FUZZY_Z_ZH)
        case .cCh: return UInt32(HMM_FUZZY_C_CH)
        case .sSh: return UInt32(HMM_FUZZY_S_SH)
        case .anAng: return UInt32(HMM_FUZZY_AN_ANG)
        case .enEng: return UInt32(HMM_FUZZY_EN_ENG)
        case .inIng: return UInt32(HMM_FUZZY_IN_ING)
        case .lN: return UInt32(HMM_FUZZY_L_N)
        case .fH: return UInt32(HMM_FUZZY_F_H)
        case .rL: return UInt32(HMM_FUZZY_R_L)
        case .kG: return UInt32(HMM_FUZZY_K_G)
        case .ianIang: return UInt32(HMM_FUZZY_IAN_IANG)
        case .uanUang: return UInt32(HMM_FUZZY_UAN_UANG)
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
        static let fuzzyPinyin = "fuzzyPinyinGroups"
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

    @Published var fuzzyPinyinGroups: Set<FuzzyPinyinGroup> {
        didSet {
            save(FuzzyPinyinGroup.allCases.filter { fuzzyPinyinGroups.contains($0) }
                .map(\.rawValue), forKey: Key.fuzzyPinyin)
        }
    }

    @Published private(set) var fuzzyPinyinApplyFailed = false

    var fuzzyPinyinMask: UInt32 {
        fuzzyPinyinGroups.reduce(0) { $0 | $1.mask }
    }

    @discardableResult
    func applyFuzzyPinyin(
        _ groups: Set<FuzzyPinyinGroup>? = nil,
        using apply: (UInt32) -> Bool = { gboard_set_fuzzy_pinyin($0) }
    ) -> Bool {
        precondition(Thread.isMainThread)
        let requested = groups ?? fuzzyPinyinGroups
        let mask = requested.reduce(UInt32(0)) { $0 | $1.mask }
        guard apply(mask) else {
            if groups == nil {
                fuzzyPinyinApplyFailed = true
            }
            NSLog("Unable to apply fuzzy pinyin: mask=%u; saved selection retained", mask)
            return false
        }
        fuzzyPinyinApplyFailed = false
        if requested != fuzzyPinyinGroups {
            fuzzyPinyinGroups = requested
        } else {
            NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
        }
        return true
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
        fuzzyPinyinGroups = Set(
            (defaults.stringArray(forKey: Key.fuzzyPinyin) ?? [])
                .compactMap(FuzzyPinyinGroup.init(rawValue:)))
        super.init()
    }

    private func save(_ value: Any, forKey key: String) {
        defaults.set(value, forKey: key)
        NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
    }
}
