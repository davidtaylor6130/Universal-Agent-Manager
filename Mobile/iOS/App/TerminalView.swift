import SwiftUI

struct TerminalView: View
{
    @Environment(MobileAppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase
    @State private var output = ""
    @State private var input = ""
    @State private var cursor = 0
    @State private var startupTime: Double = 0
    @State private var attached = false
    @State private var error: String?
    @State private var sending = false
    @State private var origin: Origin?

    private struct Origin
    {
        let context: String
        let chatID: String
        let terminalID: String
    }

    private var isCurrent: Bool
    { origin.map { $0.context == model.draftContextID && $0.chatID == model.activeChatID } ?? false }

    var body: some View
    {
        NavigationStack
        {
            VStack(spacing: 0)
            {
                Text("Live CLI session on your computer").font(.caption).foregroundStyle(.secondary).padding(10)
                ScrollView([.horizontal, .vertical])
                { Text(output).font(.system(size: 11, design: .monospaced)).textSelection(.enabled).padding(15).frame(maxWidth: .infinity, alignment: .topLeading) }
                if let error { Text(error).font(.caption).foregroundStyle(.orange).padding(10) }
                HStack
                {
                    TextField("Terminal input", text: $input).textInputAutocapitalization(.never).autocorrectionDisabled()
                    Button("Send") { Task { await write() } }.disabled(!attached || !isCurrent || scenePhase != .active || input.isEmpty || sending)
                }.padding(14).background(UAMTheme.surface)
            }.background(UAMTheme.background)
                .navigationTitle("CLI view").navigationBarTitleDisplayMode(.inline)
                .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
                .task
                {
                    if origin == nil, let chat = model.chat
                    { origin = Origin(context: model.draftContextID, chatID: chat.id, terminalID: chat.raw.object("cliTerminal").text("terminalId")) }
                    while !Task.isCancelled, isCurrent
                    {
                        if scenePhase == .active { await read() }
                        do { try await Task.sleep(for: .seconds(2)) } catch { return }
                    }
                }
        }
    }

    private func read() async
    {
        guard let origin, isCurrent else { return }
        do
        {
            var payload: WireObject = ["chatId": origin.chatID, "terminalId": origin.terminalID, "operation": attached ? "read" : "attach"]
            if attached { payload["startupTime"] = startupTime; payload["cursor"] = cursor }
            let response = try await model.request("companionCliTerminal", payload)
            guard isCurrent, !Task.isCancelled else { return }
            guard let bytes = Data(base64Encoded: response.text("replayData")) else { throw MobileError.message("The CLI returned invalid output.") }
            let text = String(decoding: bytes, as: UTF8.self).replacingOccurrences(of: "\u{1B}\\[[0-?]*[ -/]*[@-~]", with: "", options: .regularExpression)
            output = String((attached ? output + text : text).suffix(200000))
            cursor = Int(response.number("cursor")); startupTime = response.number("startupTime"); attached = true; error = nil
        }
        catch { if isCurrent, !Task.isCancelled { self.error = error.localizedDescription; attached = false } }
    }

    private func write() async
    {
        guard let origin, isCurrent, attached, scenePhase == .active else { return }
        let submitted = input
        sending = true
        defer { sending = false }
        do
        {
            _ = try await model.request("companionCliTerminal", ["chatId": origin.chatID, "terminalId": origin.terminalID, "startupTime": startupTime, "operation": "write", "data": submitted + "\r"])
            if isCurrent, input == submitted { input = "" }
        }
        catch { if isCurrent { self.error = error.localizedDescription } }
    }
}
