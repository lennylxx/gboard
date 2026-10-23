import Foundation
import os

enum UserDictionaryError: LocalizedError {
    case notReady
    case clearFailed

    var errorDescription: String? {
        switch self {
        case .notReady:
            return "用户词典尚未就绪，请先使用输入法后重试。"
        case .clearFailed:
            return "未能完整清除并保存学习数据，请重试。部分旧数据可能在重启后恢复。"
        }
    }
}

final class UserDictionary: NSObject {
    static let shared = UserDictionary()
    static let didClearNotification = Notification.Name("GboardIME.UserDictionaryDidClear")

    private let preferences: Preferences
    private let isReady: () -> Bool
    private let persistOperation: () -> Bool
    private let clearOperation: () -> Bool
    private let logger = Logger(subsystem: "com.lennylxx.inputmethod.GboardIME", category: "ime")
    private var preferencesObserver: NSObjectProtocol?
    private(set) var saveTimer: Timer?

    init(
        preferences: Preferences = .shared,
        isReady: @escaping () -> Bool = { gboard_user_dict_is_ready() },
        persist: @escaping () -> Bool = { gboard_user_dict_persist() },
        clear: @escaping () -> Bool = { gboard_user_dict_clear() }
    ) {
        self.preferences = preferences
        self.isReady = isReady
        persistOperation = persist
        clearOperation = clear
        super.init()
        preferencesObserver = NotificationCenter.default.addObserver(
            forName: Preferences.didChangeNotification, object: preferences, queue: .main
        ) { [weak self] _ in
            guard let self = self, let timer = self.saveTimer,
                  timer.timeInterval != TimeInterval(preferences.userDictionarySaveInterval.rawValue)
            else { return }
            self.scheduleTimer()
        }
    }

    deinit {
        saveTimer?.invalidate()
        if let observer = preferencesObserver {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    func startAutomaticSaving() {
        precondition(Thread.isMainThread)
        guard saveTimer == nil, isReady() else { return }
        scheduleTimer()
    }

    private func scheduleTimer() {
        saveTimer?.invalidate()
        let timer = Timer(
            timeInterval: TimeInterval(preferences.userDictionarySaveInterval.rawValue),
            repeats: true
        ) { [weak self] _ in
            self?.persist()
        }
        saveTimer = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    @discardableResult
    func persist() -> Bool {
        // The native engine shares mutable state with main-thread decoding.
        precondition(Thread.isMainThread)
        guard isReady() else {
            logger.error("Unable to save user dictionary: not initialized")
            return false
        }
        let saved = persistOperation()
        if !saved {
            logger.error("Unable to save user dictionary")
        }
        return saved
    }

    func clearLearningData() -> Result<Void, UserDictionaryError> {
        precondition(Thread.isMainThread)
        guard isReady() else {
            logger.error("Unable to clear user dictionary: not initialized")
            return .failure(.notReady)
        }
        let cleared = clearOperation()
        // A failed clear can still have changed part of the in-memory dictionary.
        NotificationCenter.default.post(name: Self.didClearNotification, object: self)
        guard cleared else {
            logger.error("Unable to completely clear and save user dictionary")
            return .failure(.clearFailed)
        }
        return .success(())
    }
}
