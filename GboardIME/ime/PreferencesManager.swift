import Foundation
import Combine

enum ChineseEnglishSwitchMode: Int, CaseIterable {
    case shift = 0
    case capsLock = 1
    case disabled = 2

    var displayName: String {
        switch self {
        case .shift: return "Shift 键 (单击切换)"
        case .capsLock: return "Caps Lock 键"
        case .disabled: return "关闭快捷切换 (纯手动)"
        }
    }
}

enum ThemeMode: Int, CaseIterable {
    case system = 0
    case light = 1
    case dark = 2

    var displayName: String {
        switch self {
        case .system: return "跟随系统"
        case .light: return "明亮浅色"
        case .dark: return "经典深色"
        }
    }
}

final class PreferencesManager: ObservableObject {
    static let shared = PreferencesManager()

    static let didChangeNotification = Notification.Name("GboardIME.PreferencesDidChange")

    private let defaults = UserDefaults.standard

    private enum Keys {
        static let isTraditional = "GboardIME_isTraditional"
        static let switchMode = "GboardIME_switchMode"
        static let themeMode = "GboardIME_themeMode"
        static let candidatePageSize = "GboardIME_candidatePageSize"
    }

    @Published var isTraditional: Bool {
        didSet {
            defaults.set(isTraditional, forKey: Keys.isTraditional)
            notifyChange()
        }
    }

    @Published var switchMode: ChineseEnglishSwitchMode {
        didSet {
            defaults.set(switchMode.rawValue, forKey: Keys.switchMode)
            notifyChange()
        }
    }

    @Published var themeMode: ThemeMode {
        didSet {
            defaults.set(themeMode.rawValue, forKey: Keys.themeMode)
            notifyChange()
        }
    }

    @Published var candidatePageSize: Int {
        didSet {
            defaults.set(candidatePageSize, forKey: Keys.candidatePageSize)
            notifyChange()
        }
    }

    private init() {
        self.isTraditional = defaults.object(forKey: Keys.isTraditional) as? Bool ?? false
        let rawSwitch = defaults.object(forKey: Keys.switchMode) as? Int ?? 0
        self.switchMode = ChineseEnglishSwitchMode(rawValue: rawSwitch) ?? .shift
        let rawTheme = defaults.object(forKey: Keys.themeMode) as? Int ?? 0
        self.themeMode = ThemeMode(rawValue: rawTheme) ?? .system
        let pageSize = defaults.object(forKey: Keys.candidatePageSize) as? Int ?? 9
        self.candidatePageSize = (pageSize >= 3 && pageSize <= 9) ? pageSize : 9
    }

    func toggleTraditional() {
        isTraditional.toggle()
    }

    private func notifyChange() {
        NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
    }
}
