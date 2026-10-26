import Cocoa
import SwiftUI
import UniformTypeIdentifiers

private let jsonLinesType = UTType(filenameExtension: "jsonl", conformingTo: .json) ?? .json

private enum DictionaryFilter: String, CaseIterable {
    case all = "全部"
    case pinyin = "中文"
    case english = "英文"

    func includes(_ word: LearnedWord) -> Bool {
        switch self {
        case .all: return true
        case .pinyin: return word.dictionary == .pinyin
        case .english: return word.dictionary == .english
        }
    }
}

private struct LearnedWordsView: View {
    @ObservedObject var store: LearnedWordStore
    let onClose: () -> Void
    @State private var query = ""
    @State private var filter = DictionaryFilter.all
    @State private var sortOrder = [KeyPathComparator(\LearnedWord.count, order: .reverse)]
    @State private var selection = Set<LearnedWord.ID>()
    @State private var pendingDeletion = Set<LearnedWord.ID>()
    @State private var confirmDelete = false
    @State private var editing: LearnedWord?
    @State private var alertTitle = ""
    @State private var alertMessage = ""
    @State private var showAlert = false

    private var visibleWords: [LearnedWord] {
        store.words
            .filter { filter.includes($0) && $0.matches(query) }
            .sorted(using: sortOrder)
    }

    var body: some View {
        let words = visibleWords
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                TextField("搜索词或拼音", text: $query)
                    .textFieldStyle(.roundedBorder)
                Picker("词典", selection: $filter) {
                    ForEach(DictionaryFilter.allCases, id: \.self) {
                        Text($0.rawValue).tag($0)
                    }
                }
                .labelsHidden()
                .pickerStyle(.segmented)
                .frame(width: 160)
            }
            .padding(12)

            Table(words, selection: $selection, sortOrder: $sortOrder) {
                TableColumn("词", value: \.text)
                TableColumn("读音", value: \.reading)
                TableColumn("词频", value: \.count) { word in
                    Text("\(word.count)").monospacedDigit()
                }
                .width(min: 50, ideal: 60, max: 90)
                TableColumn("词典", value: \.dictionaryTitle)
                    .width(min: 40, ideal: 50, max: 70)
            }
            .contextMenu(forSelectionType: LearnedWord.ID.self) { ids in
                if ids.count == 1, let word = word(for: ids.first) {
                    Button("修改词频…") { editing = word }
                }
                if !ids.isEmpty {
                    Button("删除", role: .destructive) { requestDelete(ids) }
                }
            } primaryAction: { ids in
                if ids.count == 1, let word = word(for: ids.first) { editing = word }
            }
            .onDeleteCommand { requestDelete(selection) }
            .overlay {
                if !store.loaded {
                    Text("无法读取学习词。请先使用一次输入法后重试。")
                        .foregroundStyle(.secondary)
                } else if words.isEmpty {
                    Text(store.words.isEmpty ? "还没有学习词" : "没有匹配的学习词")
                        .foregroundStyle(.secondary)
                }
            }

            Divider()

            HStack(spacing: 8) {
                Text(summary(visible: words.count))
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                Spacer()
                Button("导入…", action: importWords)
                Button("导出…", action: exportWords)
                    .disabled(store.words.isEmpty)
                Button("修改词频…") { editing = word(for: selection.first) }
                    .disabled(selection.count != 1)
                Button("删除…", role: .destructive) { requestDelete(selection) }
                    .disabled(selection.isEmpty)
                Button("完成", action: onClose)
                    .keyboardShortcut(.cancelAction)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
        }
        .frame(minWidth: 560, minHeight: 420)
        .onChange(of: store.words) { words in
            let ids = Set(words.map(\.id))
            selection = selection.filter { ids.contains($0) }
        }
        .alert(deleteTitle, isPresented: $confirmDelete) {
            Button("取消", role: .cancel) {}
            Button("删除", role: .destructive) {
                report(store.remove(pendingDeletion), failure: "无法删除学习词")
                selection.subtract(pendingDeletion)
            }
        } message: {
            Text("删除后这些词不再按学习记录排序；系统词典中的词仍会作为候选出现。")
        }
        .alert(alertTitle, isPresented: $showAlert) {
            Button("好", role: .cancel) {}
        } message: {
            Text(alertMessage)
        }
        .sheet(item: $editing) { word in
            CountEditor(word: word) { count in
                editing = nil
                if let count = count {
                    report(store.setCount(word.id, to: count), failure: "无法修改词频")
                }
            }
        }
    }

    private var deleteTitle: String {
        if pendingDeletion.count == 1, let word = word(for: pendingDeletion.first) {
            return "删除学习词「\(word.text)」？"
        }
        return "删除 \(pendingDeletion.count) 个学习词？"
    }

    private func word(for id: LearnedWord.ID?) -> LearnedWord? {
        guard let id = id else { return nil }
        return store.words.first { $0.id == id }
    }

    private func summary(visible: Int) -> String {
        var text = visible == store.words.count
            ? "共 \(store.words.count) 个"
            : "显示 \(visible) / \(store.words.count) 个"
        if !selection.isEmpty { text += "，已选 \(selection.count) 个" }
        return text
    }

    private func requestDelete(_ ids: Set<LearnedWord.ID>) {
        guard !ids.isEmpty else { return }
        pendingDeletion = ids
        confirmDelete = true
    }

    private func report(_ result: Result<Void, LearnedWordStore.StoreError>,
                        failure title: String) {
        if case .failure(let error) = result {
            present(title, error.localizedDescription)
        }
    }

    // Wait for a closing alert or sheet; SwiftUI drops overlapping ones.
    private func present(_ title: String, _ message: String) {
        DispatchQueue.main.async {
            alertTitle = title
            alertMessage = message
            showAlert = true
        }
    }

    private func exportWords() {
        let panel = NSSavePanel()
        panel.title = "导出学习词"
        panel.nameFieldStringValue = "GboardIME 学习词.jsonl"
        panel.allowedContentTypes = [jsonLinesType]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        if case .failure(let error) = store.reload() {
            present("无法导出学习词", error.localizedDescription)
            return
        }
        do {
            try LearnedWordStore.exportText(store.words)
                .write(to: url, atomically: true, encoding: .utf8)
        } catch {
            present("无法导出学习词", error.localizedDescription)
        }
    }

    private func importWords() {
        let panel = NSOpenPanel()
        panel.title = "导入学习词"
        panel.allowedContentTypes = [jsonLinesType, .json, .plainText]
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let text: String
        do {
            text = try String(contentsOf: url, encoding: .utf8)
        } catch {
            present("无法导入学习词", error.localizedDescription)
            return
        }
        switch store.importText(text) {
        case .success(let summary):
            var parts = ["新增 \(summary.added) 个", "提高词频 \(summary.updated) 个",
                         "未变 \(summary.unchanged) 个"]
            if summary.invalid > 0 { parts.append("无法识别 \(summary.invalid) 行") }
            if summary.failed > 0 { parts.append("写入失败 \(summary.failed) 个") }
            var message = parts.joined(separator: "，") + "。已有的词只会提高词频，不会降低。"
            if summary.saveFailed {
                message += LearnedWordStore.StoreError.saveFailed.localizedDescription
            }
            present("导入完成", message)
        case .failure(let error):
            present("无法导入学习词", error.localizedDescription)
        }
    }
}

private struct CountEditor: View {
    let word: LearnedWord
    let onFinish: (Int?) -> Void
    // A String field updates on every keystroke; a value-formatted field
    // commits only on Return or focus loss, so Save could read a stale value.
    @State private var text: String

    init(word: LearnedWord, onFinish: @escaping (Int?) -> Void) {
        self.word = word
        self.onFinish = onFinish
        _text = State(initialValue: String(word.count))
    }

    private var count: Int? {
        Int(text.trimmingCharacters(in: .whitespaces))
            .flatMap { LearnedWordStore.countRange.contains($0) ? $0 : nil }
    }

    private var stepperValue: Binding<Int> {
        Binding(get: { count ?? word.count }, set: { text = String($0) })
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("修改「\(word.text)」的词频")
                .font(.headline)
            Text("读音 \(word.reading)，词频越高排名越靠前。")
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
            HStack {
                TextField("词频", text: $text)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 90)
                Stepper("词频", value: stepperValue, in: LearnedWordStore.countRange)
                    .labelsHidden()
            }
            HStack {
                Spacer()
                Button("取消") { onFinish(nil) }
                    .keyboardShortcut(.cancelAction)
                Button("保存") { onFinish(count) }
                    .keyboardShortcut(.defaultAction)
                    .disabled(count == nil)
            }
        }
        .padding(20)
        .frame(width: 320)
    }
}

final class LearnedWordsWindowController: AuxiliaryWindowController {
    static let shared = LearnedWordsWindowController()
    private let store = LearnedWordStore()

    private init() {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 640, height: 520),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        window.title = "管理学习词"
        super.init(window: window)
        window.contentView = NSHostingView(rootView: LearnedWordsView(store: store) {
            [weak window] in window?.performClose(nil)
        })
        window.center()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func show() {
        store.reload()
        super.show()
    }

    // Pick up words learned in other apps while the window was in the background.
    func windowDidBecomeKey(_ notification: Notification) {
        store.reload()
    }
}
