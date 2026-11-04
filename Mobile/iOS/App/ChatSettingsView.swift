import SwiftUI
import UAMKit

struct ChatSettingsView: View
{
    @Environment(MobileAppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var modelsOpen = false
    @State private var providerID = ""
    @State private var modelID = ""
    @State private var modelOrigin: MobileChat?
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
                        Button { modelOrigin = chat; providerID = chat.providerID; modelID = chat.modelID; modelsOpen = true } label: {
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
                        }.disabled(chat.processing || chat.readOnly || !model.connected || !model.supports("setChatModel") || !model.supports("setChatProvider"))
                            .accessibilityIdentifier("nativeProviderModelPicker")
                    }
                    if !chat.runtime.objects("configOptions").isEmpty
                    {
                        Section("Provider options")
                        {
                            ForEach(chat.runtime.objects("configOptions"), id: \.selfID) { option in
                                Menu
                                {
                                    ForEach(option.objects("options"), id: \.choiceValue) { choice in
                                        Button(choice.text("name", choice.text("value"))) { setModelSetting("setAcpConfigOption", chat: chat, values: ["configId": option.text("id"), "value": choice.text("value")]) }
                                    }
                                } label: { settingValue(option.text("name"), value: option.text("currentValue")) }
                                .disabled(chat.processing || chat.readOnly || !model.connected || !model.supports("setAcpConfigOption"))
                            }
                        }
                    }
                    if let option = chat.nativeAutoRouting
                    {
                        NativeAutoRoutingSection(chat: chat, option: option, contextID: model.draftContextID)
                            .id([model.draftContextID, option.ownership?.lifetime ?? "unavailable"])
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
                            .disabled(chat.processing || chat.readOnly || !model.connected || !model.supports("setChatCodexOptions"))
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
                                .disabled(chat.processing || chat.readOnly || !model.connected || !model.supports("setChatCodexOptions"))
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
            { ModelPickerView(providerID: $providerID, modelID: $modelID, folder: model.folders.first { $0.text("id") == modelOrigin?.folderID } ?? [:], sourceChat: modelOrigin) }
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
        let id = chat.modelID.isEmpty ? chat.runtime.text("currentModelId") : chat.modelID
        return chat.runtime.objects("availableModels").first { $0.text("id") == id }
            ?? model.catalogModels(providerID: chat.providerID, hostID: chat.hostID, workspace: chat.raw.text("workspaceDirectory")).first { $0.text("id") == id } ?? [:]
    }
    private func reasoningChoices(_ chat: MobileChat) -> [String]
    {
        modelMetadata(chat)["supportedReasoningEfforts"] as? [String] ?? []
    }
    private func setModelSetting(_ action: String, chat: MobileChat, values: WireObject)
    {
        let context = originContext
        Task { await model.performChatModelSetting(action, chatID: chat.id, contextID: context, values: values,
                                                   expectedProviderID: chat.providerID, expectedModelID: chat.modelID,
                                                   expectedContext: chat.modelSettingsContext) }
    }
    private func setReasoning(_ effort: String, chat: MobileChat)
    { setModelSetting("setChatCodexOptions", chat: chat, values: ["reasoningEffort": effort, "serviceTier": chat.raw.text("serviceTier"), "serviceTierExplicit": chat.raw.flag("serviceTierExplicit")]) }
    private func setTier(_ tier: String, chat: MobileChat, explicit: Bool = true)
    { setModelSetting("setChatCodexOptions", chat: chat, values: ["reasoningEffort": chat.raw.text("reasoningEffort"), "serviceTier": tier, "serviceTierExplicit": explicit]) }

    private func applyModel() async
    {
        guard !originContext.isEmpty, model.draftContextID == originContext, let origin = modelOrigin, !providerID.isEmpty else { return }
        defer { modelOrigin = nil }
        let provider = providerID, selectedModel = modelID
        guard provider != origin.providerID || selectedModel != origin.modelID else { return }
        guard let chat = model.chat, chat.modelSettingsContext == origin.modelSettingsContext,
              chat.providerID == origin.providerID, chat.modelID == origin.modelID else
        { model.error = "This chat's provider or execution context changed. Reopen model settings before choosing again."; return }
        if provider != chat.providerID, !(await model.performChatModelSetting("setChatProvider", chatID: chat.id, contextID: originContext, values: ["providerId": provider], expectedProviderID: chat.providerID, expectedModelID: chat.modelID, expectedContext: origin.modelSettingsContext)) { return }
        guard model.draftContextID == originContext, let current = model.chat, current.id == chat.id, current.providerID == provider else { return }
        if selectedModel != current.modelID { await model.performChatModelSetting("setChatModel", chatID: chat.id, contextID: originContext, values: ["modelId": selectedModel], expectedProviderID: current.providerID, expectedModelID: current.modelID, expectedContext: origin.modelSettingsContext) }
    }
}

private struct NativeAutoRoutingSection: View
{
    @Environment(MobileAppModel.self) private var model
    let chat: MobileChat
    let option: CopilotAutoRoutingOption
    let contextID: String
    @State private var sending = false
    @State private var accepted = false

    private var canRefresh: Bool
    {
        model.connected && model.draftContextID == contextID && chat.canChangeNativeModelOptions && option.ownership != nil &&
            !option.requestPending && !sending && !model.busyActions.contains("setNativeModelOption") && !model.busyActions.contains("refreshNativeModelOptions")
    }

    var body: some View
    {
        Section("Auto routing")
        {
            Menu
            {
                Button("Default") { request(.value(nil)) }
                ForEach(option.tiers) { tier in
                    Button(tier.name) { request(.value(tier.id)) }.disabled(!tier.selectable)
                }
            } label: {
                Text("Choose preference")
                    .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                    .contentShape(Rectangle())
            }
                .disabled(!canRefresh || !option.canChoose || !model.supports("setNativeModelOption"))
                .accessibilityIdentifier("nativeAutoRoutingPreference")
            // Native menus do not reliably expose multiline choice subtitles.
            // Keep descriptions and unavailable reasons readable in the section.
            ForEach(option.tiers.filter { !$0.selectable || !$0.description.isEmpty }) { tier in
                VStack(alignment: .leading, spacing: 4)
                {
                    Text(tier.name).fontWeight(.medium)
                    if !tier.description.isEmpty { Text(tier.description) }
                    if !tier.selectable { Text(tier.disabledReason.isEmpty ? "Unavailable for Auto routing" : tier.disabledReason) }
                }.font(.footnote).foregroundStyle(.secondary)
            }
            if option.currentModel != .value("auto")
            { Text("Auto routing applies when the model is Auto. Reported preferences are retained below.").font(.footnote).foregroundStyle(.secondary) }
            if case .value(let value) = option.effective
            { LabeledContent(option.stateStatus == "ready" ? "Effective" : "Last confirmed", value: option.name(for: value)) }
            else { Text(option.stateStatus == "ready" ? "Effective: Default" : "Effective preference unavailable").foregroundStyle(.secondary) }
            if case .value(let value) = option.pending { LabeledContent("Pending", value: option.name(for: value)) }
            if case .value(let value) = option.activating { LabeledContent("Activating", value: option.name(for: value)) }
            if sending || option.requestPending
            { ProgressView(sending ? "Sending request…" : "Waiting for Copilot…") }
            else if accepted, option.lastError.isEmpty { Text("Request accepted; effective state is reported by Copilot.").font(.footnote).foregroundStyle(.secondary) }
            if !model.supports("setNativeModelOption") || !model.supports("refreshNativeModelOptions")
            { Text("This computer does not advertise native model-option controls.").font(.footnote).foregroundStyle(.secondary) }
            if !option.available, !option.disabledReason.isEmpty
            { Text(option.disabledReason).font(.footnote).foregroundStyle(.secondary) }
            if !model.connected { Text("Reconnect to change Auto routing.").font(.footnote).foregroundStyle(.secondary) }
            else if !chat.canChangeNativeModelOptions
            { Text("Wait for this chat to be ready and its current work to finish.").font(.footnote).foregroundStyle(.secondary) }
            if !option.lastError.isEmpty { Text(option.lastError).font(.footnote).foregroundStyle(.red).textSelection(.enabled) }
            Button { request(.absent) } label: {
                Label("Refresh Auto routing", systemImage: "arrow.clockwise")
                    .frame(minHeight: 44).contentShape(Rectangle())
            }.disabled(!canRefresh || !model.supports("refreshNativeModelOptions"))
                .accessibilityIdentifier("refreshNativeAutoRouting")
        }
    }

    private func request(_ preference: NativeOptionPresence<String?>)
    {
        guard canRefresh, let ownership = option.ownership else { return }
        sending = true; accepted = false
        Task
        {
            accepted = await model.requestNativeAutoRouting(preference, ownership: ownership, contextID: contextID)
            sending = false
        }
    }
}

private extension Dictionary where Key == String, Value == Any
{
    var choiceValue: String { text("value") }
}
