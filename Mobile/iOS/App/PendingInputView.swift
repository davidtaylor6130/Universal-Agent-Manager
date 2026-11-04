import SwiftUI

struct PendingInputView: View
{
    @Environment(MobileAppModel.self) private var model
    let chat: MobileChat
    @State private var expanded = true
    @State private var selected: [String: Set<String>] = [:]
    @State private var freeText: [String: String] = [:]

    var body: some View
    {
        let permission = chat.runtime.object("pendingPermission")
        let input = chat.runtime.object("pendingUserInput")
        let originContextID = model.draftContextID
        let originChatID = chat.id
        let permissionRequestID = permission.text("requestId")
        let inputRequestID = input.text("requestId")
        VStack(alignment: .leading, spacing: 12)
        {
            DisclosureGroup(isExpanded: $expanded)
            {
                ScrollView
                {
                    VStack(alignment: .leading, spacing: 12)
                    {
                        if !permission.isEmpty
                        {
                            Text(permission.text("title", "Permission requested")).font(.subheadline.weight(.medium))
                            if !permission.text("content").isEmpty
                            { Text(permission.text("content")).font(.system(size: 12, design: .monospaced)).textSelection(.enabled) }
                            ForEach(permission.objects("options"), id: \.selfID) { option in
                                Button(option.text("name", option.text("id")))
                                {
                                    Task
                                    {
                                        guard model.draftContextID == originContextID, model.activeChatID == originChatID,
                                              !permissionRequestID.isEmpty, let current = model.chat,
                                              current.id == originChatID,
                                              current.runtime.object("pendingPermission").text("requestId") == permissionRequestID else { return }
                                        await model.perform("resolveAcpPermission", ["chatId": originChatID, "requestId": permissionRequestID, "optionId": option.text("id")])
                                    }
                                }.buttonStyle(.bordered).frame(maxWidth: .infinity, alignment: .leading)
                                    .disabled(model.busyActions.contains("resolveAcpPermission"))
                            }
                        }
                        if !input.isEmpty
                        {
                            ForEach(input.objects("questions"), id: \.selfID) { question in questionView(question) }
                            Button("Send answer")
                            {
                                var answers: [String: [String]] = [:]
                                for question in input.objects("questions")
                                {
                                    let id = question.text("id")
                                    var values = Array(selected[id] ?? []).sorted()
                                    if let text = freeText[id]?.trimmingCharacters(in: .whitespacesAndNewlines), !text.isEmpty { values.append(text) }
                                    answers[id] = values
                                }
                                Task
                                {
                                    guard model.draftContextID == originContextID, model.activeChatID == originChatID,
                                          !inputRequestID.isEmpty, let current = model.chat,
                                          current.id == originChatID,
                                          current.runtime.object("pendingUserInput").text("requestId") == inputRequestID else { return }
                                    await model.perform("resolveAcpUserInput", ["chatId": originChatID, "requestId": inputRequestID, "answers": answers])
                                }
                            }.buttonStyle(.borderedProminent).disabled(!canAnswer(input) || model.busyActions.contains("resolveAcpUserInput"))
                        }
                    }.padding(.top, 10)
                }.frame(maxHeight: 270)
            } label: {
                Label("Your input is needed", systemImage: "hand.raised").font(.subheadline.weight(.medium))
            }
        }.padding(15).background(UAMTheme.surface, in: .rect(cornerRadius: 20))
            .id(permission.text("requestId", input.text("requestId")))
    }

    private func canAnswer(_ input: WireObject) -> Bool
    {
        input.objects("questions").allSatisfy { question in
            let id = question.text("id")
            return !(selected[id] ?? []).isEmpty || !(freeText[id] ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        }
    }

    @ViewBuilder private func questionView(_ question: WireObject) -> some View
    {
        let id = question.text("id")
        Text(question.text("question")).font(.subheadline)
        ForEach(question.objects("options"), id: \.optionLabel) { option in
            let label = option.text("label")
            Button
            {
                var choices = selected[id] ?? []
                if choices.contains(label) { choices.remove(label) }
                else if question.flag("isMultiple") { choices.insert(label) }
                else { choices = [label]; freeText[id] = "" }
                selected[id] = choices
            } label: {
                HStack(alignment: .top, spacing: 9)
                {
                    Image(systemName: (selected[id] ?? []).contains(label) ? "checkmark.circle.fill" : "circle")
                    VStack(alignment: .leading, spacing: 4)
                    {
                        Text(label)
                        if !option.text("description").isEmpty { Text(option.text("description")).font(.caption).foregroundStyle(.secondary) }
                    }
                }.font(.subheadline).frame(maxWidth: .infinity, alignment: .leading).padding(.vertical, 5)
            }.buttonStyle(.plain)
        }
        if question.objects("options").isEmpty || question.flag("isOther")
        {
            let binding = Binding(get: { freeText[id] ?? "" }, set: { freeText[id] = $0; if !question.flag("isMultiple") { selected[id] = [] } })
            if question.flag("isSecret") { SecureField("Your answer", text: binding).textFieldStyle(.roundedBorder) }
            else { TextField("Your answer", text: binding, axis: .vertical).textFieldStyle(.roundedBorder).lineLimit(1...5) }
        }
    }
}

private extension Dictionary where Key == String, Value == Any
{
    var optionLabel: String { text("label") }
}
