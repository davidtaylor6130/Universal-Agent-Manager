import SwiftUI
import Observation

enum MainComputerSettingsSection: String
{
    case providers = "Providers"
    case shared = "Agents, skills and MCP"
    case defaults = "Chat defaults"
    case memory = "Durable memory"
    case storage = "Storage cleanup"
    case hosts = "Execution environments"
}

struct MainComputerSettingsView: View
{
    @Environment(MobileAppModel.self) private var model
    let section: MainComputerSettingsSection

    var body: some View
    {
        Group
        {
            switch section
            {
            case .providers: ComputerProvidersView()
            case .shared: ComputerAgentsView()
            case .defaults: ComputerDefaultsView()
            case .memory: ComputerMemoryView()
            case .storage: ComputerStorageView()
            case .hosts: ComputerHostsView()
            }
        }
        .navigationTitle(section.rawValue).navigationBarTitleDisplayMode(.inline)
    }
}

// All forms retain a local draft until one explicit acknowledgement. A changed
// main-computer connection invalidates the form instead of retargeting the save.
@MainActor @Observable
private final class ComputerSettingsDraft
{
    var values: [String: String] = [:]
    var original: [String: String] = [:]
    var connectionID: String?
    var loaded = false
    var saving = false
    var error: String?
    var saved = false

    var changed: Bool { values != original }
    func load(_ values: [String: String], connectionID: String?)
    {
        guard !loaded else { return }
        self.values = values; original = values; self.connectionID = connectionID; loaded = true
    }
    func string(_ key: String) -> Binding<String>
    { Binding(get: { self.values[key] ?? "" }, set: { self.values[key] = $0; self.saved = false; self.error = nil }) }
    func toggle(_ key: String) -> Binding<Bool>
    { Binding(get: { self.values[key] == "true" }, set: { self.values[key] = $0 ? "true" : "false"; self.saved = false; self.error = nil }) }
    func discard() { values = original; error = nil; saved = false }
    func changedStringFields(_ keys: [String]) -> WireObject
    {
        var result: WireObject = [:]
        for key in keys where values[key] != original[key] { result[key] = values[key] ?? "" }
        return result
    }
    func save(model: MobileAppModel, action: String, payload: WireObject) async
    {
        guard !saving, changed else { return }
        guard loaded, connectionID != nil, connectionID == model.activeConnectionID else
        { error = "This form belongs to a different computer. Reopen it before saving."; return }
        guard model.connected, model.supports(action) else
        { error = "Reconnect to a UAM computer that supports these settings."; return }
        let submitted = values
        saving = true; error = nil; saved = false
        defer { saving = false }
        do
        {
            _ = try await model.request(action, payload)
            guard connectionID == model.activeConnectionID else { throw CancellationError() }
            original = submitted
            saved = values == submitted
            await model.refresh(force: true)
        }
        catch is CancellationError { error = "The connection changed. Reopen these settings to confirm what was saved." }
        catch { self.error = error.localizedDescription }
    }
}

private struct ComputerEditorChrome: ViewModifier
{
    @Environment(MobileAppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let draft: ComputerSettingsDraft
    let action: String
    var valid = true
    let save: () -> Void
    @State private var discardOpen = false

    private var sameComputer: Bool { draft.connectionID != nil && draft.connectionID == model.activeConnectionID }
    func body(content: Content) -> some View
    {
        content
            .scrollContentBackground(.hidden).background(UAMTheme.background)
            .disabled(draft.saving || !sameComputer || !model.supports(action))
            .navigationBarBackButtonHidden(draft.changed || draft.saving)
            .interactiveDismissDisabled(draft.changed || draft.saving)
            .toolbar
            {
                if draft.changed || draft.saving
                {
                    ToolbarItem(placement: .cancellationAction) { Button("Back") { discardOpen = true }.disabled(draft.saving) }
                }
                ToolbarItem(placement: .confirmationAction)
                {
                    if draft.saving { ProgressView().accessibilityLabel("Saving settings") }
                    else { Button("Save", action: save).disabled(!draft.changed || !valid || !sameComputer || !model.connected || !model.supports(action)).accessibilityIdentifier("saveMainSettings") }
                }
            }
            .safeAreaInset(edge: .bottom)
            {
                VStack(spacing: 0)
                {
                    if !sameComputer && draft.loaded { ErrorStrip(message: "The main computer changed. Reopen these settings before editing.") {} }
                    else if !model.supports(action) { Text("Update UAM on your computer to edit these settings from your phone.").font(.footnote).foregroundStyle(.secondary).padding() }
                    else if let error = draft.error { ErrorStrip(message: error) { draft.error = nil } }
                    else if draft.saved { Label("Saved on your computer", systemImage: "checkmark.circle").font(.footnote).foregroundStyle(.secondary).padding(12).frame(maxWidth: .infinity).background(UAMTheme.background) }
                }
            }
            .confirmationDialog("Discard unsaved changes?", isPresented: $discardOpen, titleVisibility: .visible)
            {
                Button("Discard changes", role: .destructive) { draft.discard(); dismiss() }
                Button("Keep editing", role: .cancel) {}
            }
    }
}

private struct ComputerProvidersView: View
{
    @Environment(MobileAppModel.self) private var model
    @State private var hostID = "local"
    @State private var originID: String?
    @State private var pendingAvailability: WireObject?
    @State private var savingProvider = ""
    @State private var error: String?
    @State private var savedProvider = ""
    var body: some View
    {
        List
        {
            ComputerHostPicker(selection: $hostID, allEnvironments: false)
            ForEach(model.providers, id: \.selfID) { provider in
                let id = provider.text("id")
                Section
                {
                    HStack(spacing: 12)
                    {
                        ProviderMark(id: id)
                        VStack(alignment: .leading, spacing: 4)
                        {
                            Text(provider.text("name", providerName(id)))
                            Text(availability(id) ? "Enabled on this computer" : "Disabled on this computer").font(.caption).foregroundStyle(.secondary)
                        }
                    }.padding(.vertical, 4)
                    Toggle("Enabled", isOn: Binding(get: { availability(id) }, set: { enabled in
                        pendingAvailability = ["providerId": id, "executionHostId": hostID, "enabled": enabled]
                    })).disabled(!model.connected || !model.supports("setProviderAvailability") || originID != model.activeConnectionID || !savingProvider.isEmpty)
                    if savingProvider == id { ProgressView("Saving provider preference") }
                    if savedProvider == id { Label("Saved on your computer", systemImage: "checkmark.circle").font(.footnote).foregroundStyle(.secondary) }
                    LabeledContent("Chat", value: provider.flag("supportsStructured") ? "Structured" : "Not advertised")
                    LabeledContent("CLI View", value: provider.flag("supportsCli") ? "Available" : "Not advertised")
                    NavigationLink("New chat defaults") { ComputerProviderDefaultsView(provider: provider, scope: hostID == "local" ? "*" : hostID, settings: effectiveSettings(model.settings, scope: hostID)) }
                }
            }
            if let error { Section { Text(error).font(.footnote).foregroundStyle(.red) } }
            Section { Text("Provider sign-in, installation and executable paths are managed on the main computer. Enabled is a saved preference, not proof of a live provider session.").font(.footnote).foregroundStyle(.secondary) }
        }.scrollContentBackground(.hidden).background(UAMTheme.background)
        .task { originID = model.activeConnectionID }
        .confirmationDialog("Change provider availability?", isPresented: Binding(get: { pendingAvailability != nil }, set: { if !$0 { pendingAvailability = nil } }), titleVisibility: .visible)
        {
            Button("Save preference") { if let payload = pendingAvailability { Task { await saveAvailability(payload) } }; pendingAvailability = nil }
            Button("Cancel", role: .cancel) { pendingAvailability = nil }
        } message: { Text("This changes whether the provider is available on the selected execution computer. Existing conversations are retained.") }
    }
    private func saveAvailability(_ payload: WireObject) async
    {
        guard originID != nil, originID == model.activeConnectionID, payload.text("executionHostId") == hostID else { error = "The computer changed. Reopen provider settings before editing."; return }
        savingProvider = payload.text("providerId"); error = nil; savedProvider = ""
        defer { savingProvider = "" }
        if await model.perform("setProviderAvailability", payload) { savedProvider = payload.text("providerId") }
        else { error = model.error ?? "The provider preference could not be saved. Try again." }
    }
    private func availability(_ id: String) -> Bool
    { model.settings.objects("providerAvailability").last { $0.text("providerId") == id && $0.text("executionHostId", "local") == hostID }?["enabled"] as? Bool ?? true }
}

private struct ComputerDefaultsView: View
{
    @Environment(MobileAppModel.self) private var model
    @State private var scope = "*"
    var body: some View
    {
        List
        {
            ComputerHostPicker(selection: $scope, allEnvironments: true)
            Section
            {
                NavigationLink
                {
                    ComputerDefaultProviderView(scope: scope, settings: effectiveSettings(model.settings, scope: scope))
                } label: { LabeledContent("New chat provider", value: providerName(effectiveSettings(model.settings, scope: scope).text("defaultNewChatProviderId"))) }
            } footer: { Text("Applies to new chats. Existing conversations keep their explicit choices.") }
            Section("Provider defaults")
            {
                ForEach(model.providers, id: \.selfID) { provider in
                    NavigationLink { ComputerProviderDefaultsView(provider: provider, scope: scope, settings: effectiveSettings(model.settings, scope: scope)) } label: {
                        HStack(spacing: 12) { ProviderMark(id: provider.text("id")); Text(provider.text("name", providerName(provider.text("id")))) }
                    }.accessibilityIdentifier("providerDefaults-\(provider.text("id"))")
                }
            }
        }.scrollContentBackground(.hidden).background(UAMTheme.background)
    }
}

private struct ComputerHostPicker: View
{
    @Environment(MobileAppModel.self) private var model
    @Binding var selection: String
    let allEnvironments: Bool
    var body: some View
    {
        Section
        {
            Picker("Applying settings for", selection: $selection)
            {
                if allEnvironments { Text("All environments").tag("*") }
                else { Text(model.connection?.name ?? "Main computer").tag("local") }
                ForEach(model.settings.objects("executionHosts").filter { $0.text("transport") == "ssh" }, id: \.selfID) { host in
                    Text(host.text("label", host.text("id"))).tag(host.text("id"))
                }
            }
        }
    }
}

private func effectiveSettings(_ settings: WireObject, scope: String) -> WireObject
{
    guard scope != "*", scope != "local" else { return settings }
    let override = settings.object("hostScopedSettingsValues").object(scope)
    return override.isEmpty ? settings : override
}

private struct ComputerDefaultProviderView: View
{
    @Environment(MobileAppModel.self) private var model
    let scope: String
    let settings: WireObject
    @State private var draft = ComputerSettingsDraft()
    var body: some View
    {
        List
        {
            Section
            {
                Picker("Provider", selection: draft.string("provider"))
                {
                    ForEach(model.providers, id: \.selfID) { provider in Text(provider.text("name", providerName(provider.text("id")))).tag(provider.text("id")) }
                }
            } footer: { Text("Used for new chats in \(scope == "*" ? "all environments" : model.hostName(scope)).") }
        }
        .navigationTitle("New chat provider").navigationBarTitleDisplayMode(.inline)
        .task { draft.load(["provider": settings.text("defaultNewChatProviderId")], connectionID: model.activeConnectionID) }
        .modifier(ComputerEditorChrome(draft: draft, action: "setProviderChatDefaults", valid: !draft.values["provider", default: ""].isEmpty, save: {
            Task { await draft.save(model: model, action: "setProviderChatDefaults", payload: scopedPayload(["defaultProviderId": draft.values["provider", default: ""]], scope: scope, model: model)) }
        }))
    }
}

@MainActor
private func scopedPayload(_ values: WireObject, scope: String, model: MobileAppModel) -> WireObject
{
    var payload = values
    if model.settings.flag("hostScopedSettings") { payload["scopeExecutionHostId"] = scope }
    return payload
}

private struct ComputerProviderDefaultsView: View
{
    @Environment(MobileAppModel.self) private var model
    let provider: WireObject
    let scope: String
    let settings: WireObject
    @State private var draft = ComputerSettingsDraft()
    @State private var yoloConfirmation = false
    @State private var advanced = false
    private var id: String { provider.text("id") }
    private var valid: Bool { ["modelId", "reviewerModelId"].allSatisfy { allowedModelID(draft.values[$0, default: ""]) } }

    var body: some View
    {
        List
        {
            Section
            {
                LabeledContent("Applying settings for", value: scope == "*" ? "All environments" : model.hostName(scope))
                ExactModelField(title: "Model", selection: draft.string("modelId"), providerID: id, hostID: scope == "*" ? "local" : scope)
            } footer: { Text("Use the exact model ID, including letter case. Leave it blank for the provider default.") }
            Section("Behaviour")
            {
                Picker("Provider mode", selection: draft.string("approvalMode")) { Text("Build").tag("default"); Text("Plan").tag("plan"); if draft.original["approvalMode"] == "acceptEdits" { Text("Accept edits (legacy)").tag("acceptEdits") } }
                if provider.text("structuredPermissionControl") == "uam"
                {
                    Picker("Permissions", selection: Binding(get: { draft.values["commandSafetyTier", default: "off"] }, set: { value in
                        if value == "yolo" && draft.values["commandSafetyTier"] != "yolo" { yoloConfirmation = true }
                        else { draft.values["commandSafetyTier"] = value; draft.saved = false }
                    }))
                    { Text("Ask").tag("off"); Text("Accept edits").tag("acceptEdits"); Text("AI review").tag("aiReview"); Text("YOLO").tag("yolo") }
                }
                else { LabeledContent("Permissions", value: "Provider managed") }
                MemoryPolicyPicker(selection: draft.string("memoryLevel"), inherit: true)
            }
            if id.contains("codex") || id.contains("copilot")
            {
                Section("Model options")
                {
                    Picker("Reasoning", selection: draft.string("reasoningEffort"))
                    {
                        Text("Provider default").tag("")
                        ForEach(["none", "minimal", "low", "medium", "high", "xhigh", "max"] + (id.contains("codex") ? ["ultra"] : []), id: \.self) { Text($0.capitalized).tag($0) }
                    }
                    if id.contains("codex")
                    {
                        Picker("Speed", selection: draft.string("serviceTier")) { Text("Standard").tag(""); Text("Fast").tag("fast"); Text("Flex").tag("flex") }
                    }
                }
            }
            Section
            {
                DisclosureGroup("Advanced", isExpanded: $advanced)
                {
                    Picker("Feature preference", selection: draft.string("featurePreference")) { Text("Prefer UAM").tag("uam"); Text("Prefer provider").tag("provider") }
                    ExactModelField(title: "Goal reviewer model", selection: draft.string("reviewerModelId"), providerID: id, hostID: scope == "*" ? "local" : scope)
                    Toggle("Compact model picker", isOn: draft.toggle("smallModelMode"))
                }
            } footer: { Text("Saved choices apply when a new session starts. Available provider options can vary by model.") }
            if !valid { Text("Model IDs must be at most 160 characters and use letters, digits, dots, underscores, hyphens, colons or slashes.").font(.footnote).foregroundStyle(.red) }
        }
        .navigationTitle(provider.text("name", providerName(id))).navigationBarTitleDisplayMode(.inline)
        .task
        {
            let values = settings.object("providerChatDefaults").object(id)
            draft.load(["modelId": values.text("modelId"), "approvalMode": values.text("approvalMode", "default"), "commandSafetyTier": values.text("commandSafetyTier", "off"), "memoryLevel": values.text("memoryLevel", "default"), "reasoningEffort": values.text("reasoningEffort"), "serviceTier": values.text("serviceTier"), "featurePreference": values.text("featurePreference", "uam"), "reviewerModelId": values.text("reviewerModelId"), "smallModelMode": values.flag("smallModelMode") ? "true" : "false"], connectionID: model.activeConnectionID)
        }
        .modifier(ComputerEditorChrome(draft: draft, action: "setProviderChatDefaults", valid: valid, save: save))
        .confirmationDialog("Use YOLO for new chats?", isPresented: $yoloConfirmation, titleVisibility: .visible)
        {
            Button("Use YOLO", role: .destructive) { draft.values["commandSafetyTier"] = "yolo"; draft.saved = false }
            Button("Cancel", role: .cancel) {}
        } message: { Text("New \(providerName(id)) chats will automatically approve computer use, commands, file changes and every other permission request.") }
    }
    private func save()
    {
        var changes = draft.changedStringFields(["modelId", "approvalMode", "commandSafetyTier", "memoryLevel", "reasoningEffort", "serviceTier", "featurePreference", "reviewerModelId"])
        if draft.values["smallModelMode"] != draft.original["smallModelMode"] { changes["smallModelMode"] = draft.values["smallModelMode"] == "true" }
        if let level = changes["memoryLevel"] as? String { changes["memoryEnabled"] = level == "default" ? model.settings.flag("memoryEnabledDefault") : level != "off" }
        let payload = scopedPayload(["defaults": [id: changes]], scope: scope, model: model)
        Task { await draft.save(model: model, action: "setProviderChatDefaults", payload: payload) }
    }
}

private struct ExactModelField: View
{
    @Environment(MobileAppModel.self) private var model
    let title: String
    @Binding var selection: String
    let providerID: String
    let hostID: String
    private var models: [WireObject]
    {
        var found = model.catalogs.filter { $0.text("providerId") == providerID && $0.text("executionHostId", "local") == hostID }.flatMap { $0.objects("availableModels") }
        if model.chat?.providerID == providerID && model.chat?.hostID == hostID { found += model.chat?.runtime.objects("availableModels") ?? [] }
        var seen = Set<String>()
        return found.filter { !$0.text("id").isEmpty && seen.insert($0.text("id")).inserted }
    }
    var body: some View
    {
        VStack(alignment: .leading, spacing: 8)
        {
            HStack
            {
                Text(title)
                Spacer()
                if !models.isEmpty
                {
                    Menu("Choose")
                    {
                        Button("Provider default") { selection = "" }
                        ForEach(models, id: \.selfID) { item in Button(item.text("name", item.text("id"))) { selection = item.text("id") } }
                    }.font(.subheadline)
                }
            }
            TextField("Exact model ID or provider default", text: $selection).textInputAutocapitalization(.never).autocorrectionDisabled().font(.subheadline).foregroundStyle(.secondary)
                .accessibilityLabel("\(title), exact model ID").accessibilityIdentifier(title == "Model" ? "defaultModelID" : title == "Worker model" ? "memoryWorkerModelID" : "reviewerModelID")
        }.padding(.vertical, 4)
    }
}

private func allowedModelID(_ value: String) -> Bool
{
    value.utf8.count <= 160 && !value.hasPrefix("-") && value.utf8.allSatisfy { (65...90).contains($0) || (97...122).contains($0) || (48...57).contains($0) || [46, 95, 45, 58, 47].contains($0) }
}

private struct MemoryPolicyPicker: View
{
    @Binding var selection: String
    var inherit = false
    var body: some View
    {
        Picker("Memory policy", selection: $selection)
        {
            if inherit { Text("App default").tag("default") }
            Text("Off").tag("off"); Text("Read Only").tag("readOnly"); Text("Strict").tag("strict"); Text("Balanced").tag("balanced"); Text("Open").tag("open")
        }
    }
}

private struct ComputerMemoryView: View
{
    @Environment(MobileAppModel.self) private var model
    @State private var draft = ComputerSettingsDraft()
    @State private var advanced = false
    private let numbers: [(String, String, ClosedRange<Int>)] = [("idleDelaySeconds", "Idle delay, seconds", 30...3600), ("recallBudgetBytes", "Recall budget, bytes", 512...8192), ("goalMaxLoopIterations", "Goal loop limit", 1...200), ("acpTurnOutputLimitMiB", "Turn output limit, MiB", 256...4096), ("acpSetupInactivityTimeoutSeconds", "Setup timeout, seconds", 60...3600)]
    private var valid: Bool { numbers.allSatisfy { field in Int(draft.values[field.0, default: ""]).map { field.2.contains($0) } ?? false } }
    var body: some View
    {
        List
        {
            Section
            {
                MemoryPolicyPicker(selection: draft.string("levelDefault"))
                NumericSettingRow(title: "Idle delay, seconds", value: draft.string("idleDelaySeconds"), range: 30...3600)
                NumericSettingRow(title: "Recall budget, bytes", value: draft.string("recallBudgetBytes"), range: 512...8192)
            } footer: { Text("Off prevents future recall and AI extraction. Read Only permits recall without extraction. Changing policy cannot retract memory already sent to a provider.") }
            Section("Extraction workers")
            {
                ForEach(model.providers, id: \.selfID) { provider in
                    NavigationLink(provider.text("name", providerName(provider.text("id")))) { ComputerMemoryWorkerView(chatProviderID: provider.text("id")) }
                }
            }
            Section
            {
                DisclosureGroup("Budgets and reviewer", isExpanded: $advanced)
                {
                    ForEach(Array(numbers.dropFirst(2)), id: \.0) { field in NumericSettingRow(title: field.1, value: draft.string(field.0), range: field.2) }
                    Picker("Permission reviewer", selection: draft.string("permissionReviewerProviderId"))
                    {
                        Text("Automatic").tag("")
                        ForEach(model.providers.filter { $0.flag("supportsStructured") }, id: \.selfID) { provider in Text(provider.text("name", providerName(provider.text("id")))).tag(provider.text("id")) }
                    }
                    ExactModelField(title: "Reviewer model", selection: draft.string("permissionReviewerModelId"), providerID: draft.values["permissionReviewerProviderId", default: ""], hostID: "local")
                }
            }
            if !model.settings.text("memoryLastStatus").isEmpty { Section("Last observed run") { Text(model.settings.text("memoryLastStatus")).font(.footnote).foregroundStyle(.secondary) } }
        }
        .task
        {
            let s = model.settings
            draft.load(["levelDefault": s.text("memoryLevelDefault", s.flag("memoryEnabledDefault") ? "strict" : "off"), "idleDelaySeconds": numberString(s, "memoryIdleDelaySeconds", 60), "recallBudgetBytes": numberString(s, "memoryRecallBudgetBytes", 4096), "goalMaxLoopIterations": numberString(s, "goalMaxLoopIterations", 200), "acpTurnOutputLimitMiB": numberString(s, "acpTurnOutputLimitMiB", 1024), "acpSetupInactivityTimeoutSeconds": numberString(s, "acpSetupInactivityTimeoutSeconds", 600), "permissionReviewerProviderId": s.text("permissionReviewerProviderId"), "permissionReviewerModelId": s.text("permissionReviewerModelId")], connectionID: model.activeConnectionID)
        }
        .modifier(ComputerEditorChrome(draft: draft, action: "setMemorySettings", valid: valid && allowedModelID(draft.values["permissionReviewerModelId", default: ""]), save: {
            var payload = draft.changedStringFields(["levelDefault", "permissionReviewerProviderId", "permissionReviewerModelId"])
            for field in numbers where draft.values[field.0] != draft.original[field.0] { if let number = Int(draft.values[field.0, default: ""]) { payload[field.0] = number } }
            if let level = payload["levelDefault"] as? String { payload["enabledDefault"] = level != "off" }
            Task { await draft.save(model: model, action: "setMemorySettings", payload: payload) }
        }))
    }
}

private struct ComputerMemoryWorkerView: View
{
    @Environment(MobileAppModel.self) private var model
    let chatProviderID: String
    @State private var draft = ComputerSettingsDraft()
    var body: some View
    {
        List
        {
            Section
            {
                Picker("Worker provider", selection: draft.string("workerProviderId"))
                {
                    ForEach(model.providers.filter { $0.flag("supportsStructured") }, id: \.selfID) { provider in Text(provider.text("name", providerName(provider.text("id")))).tag(provider.text("id")) }
                }
                .onChange(of: draft.values["workerProviderId"]) { old, new in if draft.loaded && old != nil && old != new { draft.values["workerModelId"] = "" } }
                ExactModelField(title: "Worker model", selection: draft.string("workerModelId"), providerID: draft.values["workerProviderId", default: ""], hostID: "local")
            } footer: { Text("This worker extracts memory for \(providerName(chatProviderID)) conversations. It may use a different provider from the chat. The exact model ID is case sensitive.") }
        }.navigationTitle("Memory worker").navigationBarTitleDisplayMode(.inline)
        .task
        {
            let value = model.settings.object("memoryWorkerBindings").object(chatProviderID)
            draft.load(["workerProviderId": value.text("workerProviderId", chatProviderID), "workerModelId": value.text("workerModelId")], connectionID: model.activeConnectionID)
        }
        .modifier(ComputerEditorChrome(draft: draft, action: "setMemorySettings", valid: allowedModelID(draft.values["workerModelId", default: ""]) && !draft.values["workerProviderId", default: ""].isEmpty, save: {
            Task { await draft.save(model: model, action: "setMemorySettings", payload: ["workerBindings": [chatProviderID: ["workerProviderId": draft.values["workerProviderId", default: ""], "workerModelId": draft.values["workerModelId", default: ""]]]]) }
        }))
    }
}

private struct NumericSettingRow: View
{
    let title: String
    @Binding var value: String
    let range: ClosedRange<Int>
    var body: some View
    {
        VStack(alignment: .leading, spacing: 6)
        {
            HStack { Text(title); Spacer(); TextField(title, text: $value).keyboardType(.numberPad).multilineTextAlignment(.trailing).frame(maxWidth: 100).accessibilityLabel(title) }
            if let number = Int(value), range.contains(number) {} else { Text("Enter \(range.lowerBound)–\(range.upperBound).").font(.caption).foregroundStyle(.red) }
        }
    }
}

private func numberString(_ object: WireObject, _ key: String, _ fallback: Int) -> String
{ String((object[key] as? NSNumber)?.intValue ?? fallback) }

private struct ComputerStorageView: View
{
    @Environment(MobileAppModel.self) private var model
    var body: some View
    {
        List
        {
            Section
            {
                NavigationLink("Backup logs and HTTP cache") { ComputerStoragePolicyView() }
                NavigationLink("Managed worktrees") { ComputerWorktreePolicyView() }
            } footer: { Text("Cleanup policies keep their own save state. They do not delete chats or provider histories.") }
        }.scrollContentBackground(.hidden).background(UAMTheme.background)
    }
}

private struct ComputerStoragePolicyView: View
{
    @Environment(MobileAppModel.self) private var model
    @State private var draft = ComputerSettingsDraft()
    private var valid: Bool { ["backupLogRetentionDays", "httpCacheClearIntervalDays"].allSatisfy { Int(draft.values[$0, default: ""]).map { (0...3650).contains($0) } ?? false } }
    var body: some View
    {
        List
        {
            Section
            {
                NumericSettingRow(title: "Keep backup logs, days", value: draft.string("backupLogRetentionDays"), range: 0...3650)
                NumericSettingRow(title: "Clear HTTP cache every, days", value: draft.string("httpCacheClearIntervalDays"), range: 0...3650)
            } footer: { Text("Zero disables age cleanup. The current log is retained. HTTP cleanup preserves profiles and sign-ins and does not purge the cache immediately.") }
        }.navigationTitle("Logs and cache").navigationBarTitleDisplayMode(.inline)
        .task { draft.load(["backupLogRetentionDays": numberString(model.settings, "backupLogRetentionDays", 30), "httpCacheClearIntervalDays": numberString(model.settings, "httpCacheClearIntervalDays", 0)], connectionID: model.activeConnectionID) }
        .modifier(ComputerEditorChrome(draft: draft, action: "setStorageSettings", valid: valid, save: {
            var payload: WireObject = [:]
            for key in ["backupLogRetentionDays", "httpCacheClearIntervalDays"] where draft.values[key] != draft.original[key] { if let number = Int(draft.values[key, default: ""]) { payload[key] = number } }
            Task { await draft.save(model: model, action: "setStorageSettings", payload: payload) }
        }))
    }
}

private struct ComputerWorktreePolicyView: View
{
    @Environment(MobileAppModel.self) private var model
    @State private var draft = ComputerSettingsDraft()
    var body: some View
    {
        List
        {
            Section
            {
                Toggle("Clean up managed worktrees", isOn: draft.toggle("enabled"))
                NumericSettingRow(title: "Keep worktrees, days", value: draft.string("days"), range: 1...3650)
            } footer: { Text("Removes eligible old managed working directories after UAM’s ownership and safety checks. Branches, commits and active or unresolved work are retained.") }
        }.navigationTitle("Managed worktrees").navigationBarTitleDisplayMode(.inline)
        .task { draft.load(["enabled": model.settings.flag("worktreeCleanupEnabled") ? "true" : "false", "days": numberString(model.settings, "worktreeRetentionDays", 30)], connectionID: model.activeConnectionID) }
        .modifier(ComputerEditorChrome(draft: draft, action: "setVcsSettings", valid: Int(draft.values["days", default: ""]).map { (1...3650).contains($0) } ?? false, save: {
            var payload: WireObject = [:]
            if draft.values["enabled"] != draft.original["enabled"] { payload["worktreeCleanupEnabled"] = draft.values["enabled"] == "true" }
            if draft.values["days"] != draft.original["days"], let days = Int(draft.values["days", default: ""]) { payload["worktreeRetentionDays"] = days }
            Task { await draft.save(model: model, action: "setVcsSettings", payload: payload) }
        }))
    }
}

private struct ComputerAgentsView: View
{
    @Environment(MobileAppModel.self) private var model
    @State private var agents: [WireObject] = []
    @State private var loading = false
    @State private var loadError: String?
    @State private var draft = ComputerSettingsDraft()
    var body: some View
    {
        List
        {
            Section("Connections") { NavigationLink("MCP connections") { ComputerMcpView() } }
            Section
            {
                if loading { ProgressView("Loading agents") }
                ForEach(agents, id: \.selfID) { agent in
                    Toggle(isOn: favorite(agent.text("id")))
                    {
                        VStack(alignment: .leading, spacing: 5)
                        {
                            Text(agent.text("id"))
                            if !agent.text("description").isEmpty { Text(agent.text("description")).font(.caption).foregroundStyle(.secondary) }
                        }.padding(.vertical, 4)
                    }.accessibilityLabel("Favourite \(agent.text("id"))")
                }
                if !loading && agents.isEmpty { Text("Open or create a chat to see the UAM agents available in its workspace.").foregroundStyle(.secondary) }
                if let loadError { Text(loadError).font(.footnote).foregroundStyle(.red) }
            } header: { Text("Favourite agents") } footer: { Text("Favourites control the main computer’s agent cycle. Agent definitions and shared skills remain managed in desktop Central Setup.") }
            Section("Desktop keyboard")
            {
                Picker("Cycle agents", selection: draft.string("shortcut"))
                {
                    Text("Shift + Tab").tag("shift+tab"); Text("Control + Shift + Tab").tag("control+shift+tab"); Text("Option + Shift + Tab").tag("alt+shift+tab"); Text("Command + Shift + Tab").tag("meta+shift+tab"); Text("Disabled").tag("disabled")
                }
            }
        }
        .task
        {
            var initial = ["shortcut": model.settings.text("uamAgentCycleShortcut", "shift+tab")]
            for id in model.settings["favoriteUamAgentIds"] as? [String] ?? [] { initial["favorite:\(id)"] = "true" }
            draft.load(initial, connectionID: model.activeConnectionID)
            guard let chatID = model.activeChatID ?? model.chats.first?.id, model.supports("listUamAgents") else { return }
            loading = true; defer { loading = false }
            do { agents = try await model.request("listUamAgents", ["chatId": chatID]).objects("agents") }
            catch { loadError = error.localizedDescription }
        }
        .modifier(ComputerEditorChrome(draft: draft, action: "setUamAgentPreferences", save: {
            let favorites = draft.values.keys.filter { $0.hasPrefix("favorite:") && draft.values[$0] == "true" }.map { String($0.dropFirst("favorite:".count)) }.sorted()
            let originalFavorites = draft.original.keys.filter { $0.hasPrefix("favorite:") && draft.original[$0] == "true" }.map { String($0.dropFirst("favorite:".count)) }.sorted()
            var payload: WireObject = [:]
            if favorites != originalFavorites { payload["favoriteUamAgentIds"] = favorites }
            if draft.values["shortcut"] != draft.original["shortcut"] { payload["uamAgentCycleShortcut"] = draft.values["shortcut", default: "shift+tab"] }
            Task { await draft.save(model: model, action: "setUamAgentPreferences", payload: payload) }
        }))
    }
    private func favorite(_ id: String) -> Binding<Bool>
    {
        Binding(get: { draft.values["favorite:\(id)"] == "true" }, set: { value in
            if value && draft.values.filter({ $0.key.hasPrefix("favorite:") && $0.value == "true" }).count >= 64
            { draft.error = "Choose at most 64 favourite agents."; return }
            if value { draft.values["favorite:\(id)"] = "true" }
            else { draft.values.removeValue(forKey: "favorite:\(id)") }
            draft.saved = false
        })
    }
}

private struct ComputerMcpView: View
{
    @Environment(MobileAppModel.self) private var model
    @State private var hostID = "local"
    @State private var status: WireObject = [:]
    @State private var loading = false
    @State private var error: String?
    @State private var originID: String?
    @State private var pending: WireObject?
    @State private var pendingConnectionID: String?
    @State private var loadRevision = UUID()
    @State private var saving = false
    @State private var savedMessage: String?
    var body: some View
    {
        List
        {
            ComputerHostPicker(selection: $hostID, allEnvironments: false)
            if loading && status.isEmpty { ProgressView("Loading MCP connections") }
            ForEach(status.objects("providers"), id: \.mcpProviderID) { provider in
                let id = provider.text("providerId")
                Section
                {
                    VStack(alignment: .leading, spacing: 5)
                    {
                        Text(mcpStatusLabel(provider.text("status"))).font(.subheadline)
                        if !provider.text("detail").isEmpty { Text(provider.text("detail")).font(.caption).foregroundStyle(.secondary) }
                    }.padding(.vertical, 4)
                    mcpToggle("UAM agent tools", provider: id, server: "uam-mcp", enabled: provider.flag("uamControlEnabled"))
                    mcpToggle("Shared setup", provider: id, server: "shared-setup", enabled: provider.flag("sharedSetupEnabled"))
                    ForEach(provider.objects("mcpServers"), id: \.selfID) { server in
                        mcpToggle(server.text("name", server.text("id")), provider: id, server: server.text("id"), enabled: server.flag("enabled"))
                    }
                } header: { Text(providerName(id)) }
            }
            if !loading && status.objects("providers").isEmpty && error == nil { Text("No MCP status is available from this computer.").foregroundStyle(.secondary) }
            if let error { Section { Text(error).font(.footnote).foregroundStyle(.red); Button("Retry") { Task { await load() } } } }
            if let savedMessage { Section { Label(savedMessage, systemImage: "checkmark.circle").font(.footnote).foregroundStyle(.secondary) } }
            Section { Text("Switches save the provider’s connection preference. Changes take effect on its next session launch; connected and handshake status report observed proof separately. Add or edit server credentials in desktop settings.").font(.footnote).foregroundStyle(.secondary) }
        }
        .scrollContentBackground(.hidden).background(UAMTheme.background)
        .navigationTitle("MCP connections").navigationBarTitleDisplayMode(.inline)
        .task(id: "\(model.activeConnectionID ?? ""):\(hostID)") { status = [:]; await load() }
        .refreshable { await load() }
        .disabled(saving)
        .confirmationDialog("Change MCP connection?", isPresented: Binding(get: { pending != nil }, set: { if !$0 { pending = nil } }), titleVisibility: .visible)
        {
            Button("Save for next session") { if let payload = pending { let connection = pendingConnectionID; Task { await save(payload, connectionID: connection) } }; pending = nil }
            Button("Cancel", role: .cancel) { pending = nil }
        } message: { Text("Running sessions keep their current setup. The next provider session uses the saved connection preference.") }
    }
    private func mcpToggle(_ name: String, provider: String, server: String, enabled: Bool) -> some View
    {
        Toggle(name, isOn: Binding(get: { enabled }, set: { value in pendingConnectionID = originID; pending = ["executionHostId": hostID, "providerId": provider, "serverId": server, "enabled": value] }))
            .disabled(!model.connected || !model.supports("setProviderMcpConfiguration") || originID != model.activeConnectionID || loading || saving)
    }
    private func load() async
    {
        guard model.supports("getProviderMcpStatus") else { error = "Update UAM on your computer to view MCP connections."; return }
        let connection = model.activeConnectionID
        let host = hostID
        let revision = UUID(); loadRevision = revision
        originID = connection; loading = true; error = nil; savedMessage = nil
        do
        {
            let next = try await model.request("getProviderMcpStatus", ["executionHostId": host])
            guard revision == loadRevision, host == hostID, connection == model.activeConnectionID, !Task.isCancelled else { return }
            status = next; loading = false
        }
        catch { if revision == loadRevision, host == hostID, connection == model.activeConnectionID { self.error = error.localizedDescription; loading = false } }
    }
    private func save(_ payload: WireObject, connectionID: String?) async
    {
        guard connectionID != nil, connectionID == model.activeConnectionID, originID == connectionID, payload.text("executionHostId") == hostID else { error = "The computer changed. Reload before editing connections."; return }
        loadRevision = UUID(); loading = false
        saving = true; error = nil
        defer { saving = false }
        do
        {
            status = try await model.request("setProviderMcpConfiguration", payload)
            savedMessage = "Saved for the next session"
            await model.refresh(force: true)
        }
        catch { self.error = error.localizedDescription }
    }
}

private extension Dictionary where Key == String, Value == Any
{
    var mcpProviderID: String { text("providerId") }
}

private func mcpStatusLabel(_ status: String) -> String
{
    switch status
    {
    case "disabled": "Disabled"
    case "missing": "Not configured"
    case "configured-unverified": "Configured, awaiting verification"
    case "failed": "Connection failed"
    case "handshake-confirmed": "Handshake confirmed"
    case "connected": "Connected"
    default: "Status unavailable"
    }
}

private struct ComputerHostsView: View
{
    @Environment(MobileAppModel.self) private var model
    var body: some View
    {
        List
        {
            Section
            {
                Label(model.connection?.name ?? "Main UAM computer", systemImage: "desktopcomputer")
                LabeledContent("Phone connection", value: model.connected ? "Connected" : "Disconnected")
                if let address = model.connection?.address { Text(address).font(.caption).foregroundStyle(.secondary).textSelection(.enabled) }
            } footer: { Text("Your phone connects to this main UAM instance. It manages downstream SSH execution.") }
            ForEach(model.settings.objects("executionHosts").filter { $0.text("transport") == "ssh" }, id: \.selfID) { host in
                Section(host.text("label", host.text("id")))
                {
                    LabeledContent("Transport", value: "SSH")
                    LabeledContent("Runner", value: host.text("runnerStatus", "Unknown"))
                    if !host.text("platform").isEmpty { LabeledContent("Platform", value: host.text("platform")) }
                    if !host.text("architecture").isEmpty { LabeledContent("Architecture", value: host.text("architecture")) }
                }
            }
            Section { Text("Configure SSH credentials, runner installation and execution routing in UAM on your computer.").font(.footnote).foregroundStyle(.secondary) }
        }.scrollContentBackground(.hidden).background(UAMTheme.background)
    }
}
