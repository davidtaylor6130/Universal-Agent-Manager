import SwiftUI

struct ChatSettingsView: View
{
    @Environment(MobileAppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var modelsOpen = false
    @State private var providerID = ""
    @State private var modelID = ""
    @State private var agents: [WireObject] = []
    @State private var search = ""
    @State private var originContext = ""

    var body: some View
    {
        NavigationStack
        {
            List
            {
                if let chat = model.chat
                {
                    Section("Provider and model")
                    {
                        Button { providerID = chat.providerID; modelID = chat.modelID; modelsOpen = true } label: {
                            HStack(spacing: 12)
                            {
                                ProviderMark(id: chat.providerID)
                                VStack(alignment: .leading, spacing: 4)
                                {
                                    Text(providerName(chat.providerID)).foregroundStyle(.primary)
                                    Text(shortModelName(chat.runtime.text("currentModelId", chat.modelID))).font(.footnote).foregroundStyle(.secondary)
                                }
                                Spacer(); Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary)
                            }.padding(.vertical, 5)
                        }.disabled(chat.processing || chat.readOnly)
                    }
                    if !chat.runtime.objects("configOptions").isEmpty
                    {
                        Section("Provider options")
                        {
                            ForEach(chat.runtime.objects("configOptions"), id: \.selfID) { option in
                                Menu
                                {
                                    ForEach(option.objects("options"), id: \.choiceValue) { choice in
                                        Button(choice.text("name", choice.text("value"))) { Task { await model.perform("setAcpConfigOption", ["chatId": chat.id, "configId": option.text("id"), "value": choice.text("value")]) } }
                                    }
                                } label: { settingValue(option.text("name"), value: option.text("currentValue")) }
                                .disabled(chat.processing || chat.readOnly)
                            }
                        }
                    }
                    if chat.providerID.contains("codex") || chat.providerID.contains("copilot") || !reasoningChoices(chat).isEmpty
                    {
                        Section("Options")
                        {
                            Menu
                            {
                                Button("Provider default") { setReasoning("", chat: chat) }
                                ForEach(reasoningChoices(chat), id: \.self) { value in
                                    Button(value.capitalized) { setReasoning(value, chat: chat) }
                                }
                            } label: { settingValue("Reasoning", value: chat.raw.text("reasoningEffort", "Provider default")) }
                            .disabled(chat.processing || chat.readOnly)
                            if chat.providerID.contains("codex")
                            {
                                Menu
                                {
                                    Button("Inherit") { setTier("", chat: chat, explicit: false) }
                                    Button("Standard") { setTier("", chat: chat) }
                                    ForEach(modelMetadata(chat)["additionalSpeedTiers"] as? [String] ?? [], id: \.self) { tier in
                                        Button(tier.capitalized) { setTier(tier, chat: chat) }
                                    }
                                } label: { settingValue("Service tier", value: chat.raw.text("serviceTier", "Standard")) }
                                .disabled(chat.processing || chat.readOnly)
                            }
                        }
                    }
                    Section
                    {
                        Toggle("UAM agent tools", isOn: Binding(get: { chat.raw.flag("uamControlEnabled") }, set: { enabled in Task { await model.perform("setChatUamControlEnabled", ["chatId": chat.id, "enabled": enabled]) } }))
                            .disabled(chat.readOnly || model.busyActions.contains("setChatUamControlEnabled"))
                        if !agents.isEmpty
                        {
                            Menu
                            {
                                ForEach(agents, id: \.selfID) { agent in
                                    Button(agent.text("id")) { Task { await model.perform("setChatUamAgent", ["chatId": chat.id, "agentId": agent.text("id")]) } }
                                }
                            } label: { settingValue("Agent", value: chat.raw.text("uamAgentId", "build")) }
                            .disabled(chat.readOnly)
                        }
                        Toggle("Durable memory", isOn: Binding(get: { chat.raw.flag("memoryEnabled") }, set: { enabled in Task { await model.perform("setChatMemoryEnabled", ["chatId": chat.id, "enabled": enabled]) } }))
                            .disabled(chat.readOnly || model.busyActions.contains("setChatMemoryEnabled"))
                    } header: { Text("Universal Agent Manager") } footer: { Text("These settings belong to this chat on your main computer.") }
                    if !chat.runtime.objects("availableModes").isEmpty
                    {
                        Section("Permissions")
                        {
                            Menu
                            {
                                ForEach(chat.runtime.objects("availableModes"), id: \.selfID) { mode in
                                    Button(mode.text("name", mode.text("id"))) { Task { await model.perform("setChatApprovalMode", ["chatId": chat.id, "modeId": mode.text("id")]) } }
                                }
                            } label: { settingValue("Approval mode", value: chat.runtime.text("currentModeId", chat.raw.text("approvalMode"))) }
                            .disabled(chat.processing || chat.readOnly)
                        }
                    }
                    Section("Workspace")
                    {
                        LabeledContent("Project", value: model.folderName(chat.folderID))
                        LabeledContent("Computer", value: model.hostName(chat.hostID))
                        if !chat.raw.text("workspaceBranchName").isEmpty { LabeledContent("Branch", value: chat.raw.text("workspaceBranchName")) }
                        Text(chat.raw.text("workspaceDirectory")).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                    }
                }
            }
            .scrollContentBackground(.hidden).background(UAMTheme.background)
            .navigationTitle("Chat settings").navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .safeAreaInset(edge: .bottom) { if let error = model.error { ErrorStrip(message: error) { model.error = nil } } }
            .sheet(isPresented: $modelsOpen, onDismiss: { Task { await applyModel() } })
            { ModelPickerView(providerID: $providerID, modelID: $modelID, folder: model.folders.first { $0.text("id") == model.chat?.folderID } ?? [:]) }
            .task
            {
                guard let id = model.activeChatID else { return }
                let context = model.draftContextID
                originContext = context
                if let result = try? await model.request("listUamAgents", ["chatId": id]), model.draftContextID == context { agents = result.objects("agents") }
            }
        }.presentationDragIndicator(.visible).presentationCornerRadius(34)
    }

    private func settingValue(_ name: String, value: String) -> some View
    {
        HStack { Text(name).foregroundStyle(.primary); Spacer(); Text(value).foregroundStyle(.secondary).lineLimit(1); Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary) }
            .frame(minHeight: 28)
    }
    private func modelMetadata(_ chat: MobileChat) -> WireObject
    {
        let id = chat.runtime.text("currentModelId", chat.modelID)
        return chat.runtime.objects("availableModels").first { $0.text("id") == id }
            ?? model.catalogs.filter { $0.text("providerId") == chat.providerID && $0.text("executionHostId", "local") == chat.hostID }.flatMap { $0.objects("availableModels") }.first { $0.text("id") == id } ?? [:]
    }
    private func reasoningChoices(_ chat: MobileChat) -> [String]
    {
        let supported = modelMetadata(chat)["supportedReasoningEfforts"] as? [String] ?? []
        if !supported.isEmpty { return supported }
        let selected = chat.raw.text("reasoningEffort")
        return selected.isEmpty ? [] : [selected]
    }
    private func setReasoning(_ effort: String, chat: MobileChat)
    { Task { await model.perform("setChatCodexOptions", ["chatId": chat.id, "reasoningEffort": effort, "serviceTier": chat.raw.text("serviceTier"), "serviceTierExplicit": chat.raw.flag("serviceTierExplicit")]) } }
    private func setTier(_ tier: String, chat: MobileChat, explicit: Bool = true)
    { Task { await model.perform("setChatCodexOptions", ["chatId": chat.id, "reasoningEffort": chat.raw.text("reasoningEffort"), "serviceTier": tier, "serviceTierExplicit": explicit]) } }

    private func applyModel() async
    {
        guard !originContext.isEmpty, model.draftContextID == originContext, let chat = model.chat, !providerID.isEmpty else { return }
        let provider = providerID, selectedModel = modelID
        if provider != chat.providerID, !(await model.perform("setChatProvider", ["chatId": chat.id, "providerId": provider])) { return }
        guard model.draftContextID == originContext else { return }
        if !selectedModel.isEmpty, selectedModel != chat.modelID { await model.perform("setChatModel", ["chatId": chat.id, "modelId": selectedModel]) }
    }
}

private extension Dictionary where Key == String, Value == Any
{
    var choiceValue: String { text("value") }
}
