import Cocoa
import SwiftUI
import UniformTypeIdentifiers

private let jsonLinesType = UTType(filenameExtension: "jsonl", conformingTo: .json) ?? .json

private struct ShortcutsView: View {
    @ObservedObject var store: ShortcutStore
    @ObservedObject var preferences: Preferences
    let onClose: () -> Void
    @State private var query = ""
    @State private var sortOrder = [KeyPathComparator(\Shortcut.shortcut)]
    @State private var selection = Set<Shortcut.ID>()
    @State private var pendingDeletion = Set<Shortcut.ID>()
    @State private var confirmDelete = false
    @State private var editing: ShortcutDraft?
    @State private var alertTitle = ""
    @State private var alertMessage = ""
    @State private var showAlert = false

    private var visibleShortcuts: [Shortcut] {
        store.shortcuts.filter { $0.matches(query) }.sorted(using: sortOrder)
    }

    var body: some View {
        let shortcuts = visibleShortcuts
        VStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 8) {
                TextField("搜索缩写或短语", text: $query)
                    .textFieldStyle(.roundedBorder)
                if !preferences.shortcutsEnabled {
                    Label("快捷短语已关闭，请在设置的“快捷短语”中开启后使用。",
                          systemImage: "info.circle")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                }
                if store.skippedLines > 0 {
                    Label("快捷短语文件中有 \(store.skippedLines) 行无法读取，下次保存时将被移除。",
                          systemImage: "exclamationmark.triangle")
                        .font(.system(size: 12))
                        .foregroundStyle(.orange)
                }
                if store.needsEngineSync {
                    HStack {
                        Label("输入法中的快捷短语可能与列表不一致。",
                              systemImage: "exclamationmark.triangle")
                            .font(.system(size: 12))
                            .foregroundStyle(.orange)
                        Button("重新同步") {
                            report(store.loadAndSync(), failure: "无法同步快捷短语")
                        }
                    }
                }
            }
            .padding(12)

            Table(shortcuts, selection: $selection, sortOrder: $sortOrder) {
                TableColumn("缩写", value: \.shortcut)
                    .width(min: 60, ideal: 100, max: 200)
                TableColumn("短语", value: \.phrase)
            }
            .contextMenu(forSelectionType: Shortcut.ID.self) { ids in
                if ids.count == 1, let entry = ids.first {
                    Button("编辑…") { editing = ShortcutDraft(original: entry) }
                }
                if !ids.isEmpty {
                    Button("删除", role: .destructive) { requestDelete(ids) }
                }
            } primaryAction: { ids in
                if ids.count == 1, let entry = ids.first {
                    editing = ShortcutDraft(original: entry)
                }
            }
            .onDeleteCommand { requestDelete(selection) }
            .overlay {
                if !store.loaded {
                    Text("无法读取快捷短语文件。").foregroundStyle(.secondary)
                } else if shortcuts.isEmpty {
                    Text(store.shortcuts.isEmpty ? "还没有快捷短语" : "没有匹配的快捷短语")
                        .foregroundStyle(.secondary)
                }
            }

            Divider()

            HStack(spacing: 8) {
                Text(summary(visible: shortcuts.count))
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                Spacer()
                Button("导入…", action: importShortcuts)
                    .disabled(!store.loaded)
                Button("导出…", action: exportShortcuts)
                    .disabled(store.shortcuts.isEmpty)
                Button("添加…") { editing = ShortcutDraft(original: nil) }
                    .disabled(!store.loaded)
                Button("编辑…") { editing = ShortcutDraft(original: selection.first) }
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
        .onChange(of: store.shortcuts) { shortcuts in
            let ids = Set(shortcuts)
            selection = selection.filter { ids.contains($0) }
        }
        .alert(deleteTitle, isPresented: $confirmDelete) {
            Button("取消", role: .cancel) {}
            Button("删除", role: .destructive) {
                report(store.remove(pendingDeletion), failure: "无法删除快捷短语")
                selection.subtract(pendingDeletion)
            }
        }
        .alert(alertTitle, isPresented: $showAlert) {
            Button("好", role: .cancel) {}
        } message: {
            Text(alertMessage)
        }
        .sheet(item: $editing) { draft in
            ShortcutEditor(store: store, original: draft.original) { editing = nil }
        }
    }

    private var deleteTitle: String {
        if pendingDeletion.count == 1, let entry = pendingDeletion.first {
            return "删除快捷短语「\(entry.shortcut)」？"
        }
        return "删除 \(pendingDeletion.count) 个快捷短语？"
    }

    private func summary(visible: Int) -> String {
        var text = visible == store.shortcuts.count
            ? "共 \(store.shortcuts.count) 个"
            : "显示 \(visible) / \(store.shortcuts.count) 个"
        if !selection.isEmpty { text += "，已选 \(selection.count) 个" }
        return text
    }

    private func requestDelete(_ ids: Set<Shortcut.ID>) {
        guard !ids.isEmpty else { return }
        pendingDeletion = ids
        confirmDelete = true
    }

    private func report(_ result: Result<Void, ShortcutStore.StoreError>,
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

    private func exportShortcuts() {
        let panel = NSSavePanel()
        panel.title = "导出快捷短语"
        panel.nameFieldStringValue = "GboardIME 快捷短语.jsonl"
        panel.allowedContentTypes = [jsonLinesType]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try ShortcutStore.exportText(store.shortcuts)
                .write(to: url, atomically: true, encoding: .utf8)
        } catch {
            present("无法导出快捷短语", error.localizedDescription)
        }
    }

    private func importShortcuts() {
        let panel = NSOpenPanel()
        panel.title = "导入快捷短语"
        panel.allowedContentTypes = [jsonLinesType, .json, .plainText]
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let text: String
        do {
            text = try String(contentsOf: url, encoding: .utf8)
        } catch {
            present("无法导入快捷短语", error.localizedDescription)
            return
        }
        switch store.importText(text) {
        case .success(let summary):
            var parts = ["新增 \(summary.added) 个", "已存在 \(summary.unchanged) 个"]
            if summary.invalid > 0 { parts.append("无法识别 \(summary.invalid) 行") }
            present("导入完成", parts.joined(separator: "，") + "。")
        case .failure(let error):
            present("无法导入快捷短语", error.localizedDescription)
        }
    }
}

private struct ShortcutDraft: Identifiable {
    let id = UUID()
    let original: Shortcut?
}

private struct ShortcutEditor: View {
    @ObservedObject var store: ShortcutStore
    let original: Shortcut?
    let onFinish: () -> Void
    @State private var shortcut: String
    @State private var phrase: String
    @State private var error: String?

    init(store: ShortcutStore, original: Shortcut?, onFinish: @escaping () -> Void) {
        self.store = store
        self.original = original
        self.onFinish = onFinish
        _shortcut = State(initialValue: original?.shortcut ?? "")
        _phrase = State(initialValue: original?.phrase ?? "")
    }

    private var isValid: Bool {
        store.normalized(shortcut: shortcut, phrase: phrase) != nil
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(original == nil ? "添加快捷短语" : "编辑快捷短语")
                .font(.headline)
            Form {
                TextField("缩写", text: $shortcut, prompt: Text("例如 dz"))
                TextField("短语", text: $phrase, prompt: Text("例如 北京市朝阳区建国路 88 号"))
            }
            Text("在中文模式下完整输入缩写时，短语会作为首选候选出现。缩写只能包含英文字母。")
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            if let error = error {
                Text(error)
                    .font(.system(size: 12))
                    .foregroundStyle(.red)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                Spacer()
                Button("取消", action: onFinish)
                    .keyboardShortcut(.cancelAction)
                Button("保存", action: save)
                    .keyboardShortcut(.defaultAction)
                    .disabled(!isValid)
            }
        }
        .padding(20)
        .frame(width: 400)
    }

    private func save() {
        let result = original.map { store.update($0, shortcut: shortcut, phrase: phrase) }
            ?? store.add(shortcut: shortcut, phrase: phrase)
        switch result {
        case .success:
            onFinish()
        case .failure(let failure):
            error = failure.localizedDescription
        }
    }
}

final class ShortcutsWindowController: AuxiliaryWindowController {
    static let shared = ShortcutsWindowController()
    private let store = ShortcutStore.shared

    private init() {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 640, height: 520),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        window.title = "管理快捷短语"
        super.init(window: window)
        window.contentView = NSHostingView(rootView: ShortcutsView(
            store: store, preferences: .shared
        ) { [weak window] in window?.performClose(nil) })
        window.center()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func show() {
        if !store.loaded { store.loadAndSync() }
        super.show()
    }
}
