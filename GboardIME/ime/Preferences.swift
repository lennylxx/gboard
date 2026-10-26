import Combine
import Foundation

enum CorrectionAlignment: String, CaseIterable {
    case lcs
    case dl

    var title: String {
        switch self {
        case .lcs: return "增删标记（默认）"
        case .dl: return "交换标记"
        }
    }
}

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

enum ThemeMode: String, CaseIterable {
    case system
    case light
    case dark

    var title: String {
        switch self {
        case .system: return "跟随系统"
        case .light: return "浅色"
        case .dark: return "深色"
        }
    }
}

enum PinyinScheme: String, CaseIterable {
    case full
    case flypy
    case ziranma
    case ms
    case abc
    case ziguang
    case jiajia

    var title: String {
        switch self {
        case .full: return "全拼"
        case .flypy: return "小鹤双拼"
        case .ziranma: return "自然码双拼"
        case .ms: return "微软双拼"
        case .abc: return "智能 ABC 双拼"
        case .ziguang: return "紫光双拼"
        case .jiajia: return "拼音加加双拼"
        }
    }

    var nativeValue: HmmPinyinScheme {
        switch self {
        case .full: return HMM_PINYIN_FULL
        case .flypy: return HMM_PINYIN_SHUANGPIN_FLYPY
        case .ziranma: return HMM_PINYIN_SHUANGPIN_ZIRANMA
        case .ms: return HMM_PINYIN_SHUANGPIN_MS
        case .abc: return HMM_PINYIN_SHUANGPIN_ABC
        case .ziguang: return HMM_PINYIN_SHUANGPIN_ZIGUANG
        case .jiajia: return HMM_PINYIN_SHUANGPIN_JIAJIA
        }
    }
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
    private var pinyinCompositions: Set<ObjectIdentifier> = []
    private var pinyinSchemeRetryPending = false

    private enum Key {
        static let predictions = "predictionsEnabled"
        static let context = "contextEnabled"
        static let shift = "shiftToggleEnabled"
        static let correction = "correctionStyle"
        static let correctionAlignment = "correctionAlignment"
        static let pageSize = "candidatePageSize"
        static let legacyPinyinBar = "pinyinBarEnabled"
        static let pinyinDisplayMode = "pinyinDisplayMode"
        static let learning = "learningEnabled"
        static let saveInterval = "userDictionarySaveInterval"
        static let fuzzyPinyin = "fuzzyPinyinGroups"
        static let pinyinScheme = "pinyinScheme"
        static let shortcuts = "shortcutsEnabled"
        static let isTraditional = "traditionalChineseEnabled"
        static let themeMode = "themeMode"
        static let windowOpacity = "candidateWindowOpacity"
    }

    static let candidateWindowOpacityRange: ClosedRange<Double> = 0.0...1.0
    static let defaultCandidateWindowOpacity = 0.7

    static func clampedOpacity(_ value: Double) -> Double {
        guard value.isFinite else { return defaultCandidateWindowOpacity }
        let range = candidateWindowOpacityRange
        return min(max(value, range.lowerBound), range.upperBound)
    }

    @Published var isTraditional: Bool {
        didSet { save(isTraditional, forKey: Key.isTraditional) }
    }

    @Published var themeMode: ThemeMode {
        didSet { save(themeMode.rawValue, forKey: Key.themeMode) }
    }

    /// Opacity of the candidate window colors over the frosted background
    /// (macOS < 26; Liquid Glass follows the system appearance instead).
    @Published var candidateWindowOpacity: Double {
        didSet {
            let clamped = Self.clampedOpacity(candidateWindowOpacity)
            guard clamped == candidateWindowOpacity else {
                candidateWindowOpacity = clamped
                return
            }
            save(candidateWindowOpacity, forKey: Key.windowOpacity)
        }
    }

    func toggleTraditional() {
        isTraditional.toggle()
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

    @Published var correctionAlignment: CorrectionAlignment {
        didSet { save(correctionAlignment.rawValue, forKey: Key.correctionAlignment) }
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
    @Published private(set) var pinyinScheme: PinyinScheme {
        didSet { save(pinyinScheme.rawValue, forKey: Key.pinyinScheme) }
    }
    @Published private(set) var pinyinSchemeApplyFailed = false

    var pinyinSchemePending: Bool {
        !pinyinSchemeApplyFailed && gboard_get_pinyin_scheme() != pinyinScheme.nativeValue
    }

    @discardableResult
    func applyPinyinScheme(
        _ scheme: PinyinScheme? = nil,
        using apply: (HmmPinyinScheme) -> Bool = { gboard_set_pinyin_scheme($0) }
    ) -> Bool {
        precondition(Thread.isMainThread)
        let requested = scheme ?? pinyinScheme
        if pinyinCompositions.isEmpty {
            pinyinSchemeRetryPending = false
            guard apply(requested.nativeValue) else {
                pinyinSchemeApplyFailed = true
                NSLog("Unable to apply pinyin scheme: %@", requested.rawValue)
                return false
            }
            pinyinSchemeApplyFailed = false
        } else {
            pinyinSchemeRetryPending = true
        }
        if requested != pinyinScheme {
            pinyinScheme = requested
        } else {
            NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
        }
        return true
    }

    func beginPinyinComposition(_ owner: AnyObject) {
        pinyinCompositions.insert(ObjectIdentifier(owner))
    }

    func endPinyinComposition(_ owner: AnyObject) {
        guard pinyinCompositions.remove(ObjectIdentifier(owner)) != nil else { return }
        if pinyinCompositions.isEmpty &&
            (pinyinSchemeRetryPending || pinyinSchemePending) {
            applyPinyinScheme()
        }
    }

    /// Off by default, as in Gboard's personal dictionary shortcuts.
    @Published private(set) var shortcutsEnabled: Bool {
        didSet { save(shortcutsEnabled, forKey: Key.shortcuts) }
    }
    @Published private(set) var shortcutsApplyFailed = false

    @discardableResult
    func applyShortcuts(
        _ enabled: Bool? = nil,
        using apply: (Bool) -> Bool = { gboard_set_shortcuts_enabled($0) }
    ) -> Bool {
        precondition(Thread.isMainThread)
        let requested = enabled ?? shortcutsEnabled
        guard apply(requested) else {
            if enabled == nil {
                shortcutsApplyFailed = true
            }
            NSLog("Unable to %@ shortcuts; saved setting retained",
                  requested ? "enable" : "disable")
            return false
        }
        shortcutsApplyFailed = false
        if requested != shortcutsEnabled {
            shortcutsEnabled = requested
        } else {
            NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
        }
        return true
    }

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
            Key.isTraditional: false,
            Key.themeMode: ThemeMode.system.rawValue,
            Key.windowOpacity: Self.defaultCandidateWindowOpacity,
            Key.correction: CorrectionStyle.edits.rawValue,
            Key.correctionAlignment: CorrectionAlignment.lcs.rawValue,
            Key.pageSize: CandidatePageSize.nine.rawValue,
            Key.pinyinDisplayMode: defaultDisplayMode.rawValue,
            Key.learning: true,
            Key.shortcuts: false,
            Key.saveInterval: UserDictionarySaveInterval.fourHours.rawValue
        ])
        predictionsEnabled = defaults.bool(forKey: Key.predictions)
        contextEnabled = defaults.bool(forKey: Key.context)
        shiftToggleEnabled = defaults.bool(forKey: Key.shift)
        isTraditional = defaults.bool(forKey: Key.isTraditional)
        themeMode = ThemeMode(
            rawValue: defaults.string(forKey: Key.themeMode) ?? ""
        ) ?? .system
        candidateWindowOpacity = Self.clampedOpacity(
            (defaults.object(forKey: Key.windowOpacity) as? NSNumber)?.doubleValue
                ?? Self.defaultCandidateWindowOpacity
        )
        correctionStyle = CorrectionStyle(
            rawValue: defaults.string(forKey: Key.correction) ?? ""
        ) ?? .edits
        correctionAlignment = CorrectionAlignment(
            rawValue: defaults.string(forKey: Key.correctionAlignment) ?? ""
        ) ?? .lcs
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
        pinyinScheme = PinyinScheme(
            rawValue: defaults.string(forKey: Key.pinyinScheme) ?? ""
        ) ?? .full
        shortcutsEnabled = defaults.bool(forKey: Key.shortcuts)
        super.init()
    }

    private func save(_ value: Any, forKey key: String) {
        defaults.set(value, forKey: key)
        NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
    }
}
