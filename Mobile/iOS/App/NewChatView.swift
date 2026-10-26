import SwiftUI

struct NewChatView: View
{
    @Environment(MobileAppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase
    var initialProjectID = ""
    @State private var selectedProjectID = ""
    @State private var search = ""
    @State private var providerID = ""
    @State private var modelID = ""
    @State private var draft = ComposerDraft()
    @State private var modelsOpen = false
    @State private var draftContext: DraftContext?
    @State private var draftSaveTask: Task<Void, Never>?
    @State private var draftSaveGeneration = UUID()

    private var folder: WireObject? { model.folders.first { $0.text("id") == selectedProjectID } }
    var body: some View
    {
        NavigationStack
        {
            Group
            {
                if let folder { compose(folder) }
                else { projectPicker }
            }
            .background(UAMTheme.background)
            .navigationTitle(folder == nil ? "Choose project" : "New chat").navigationBarTitleDisplayMode(.inline)
            .toolbar
            {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                if folder != nil { ToolbarItem(placement: .confirmationAction) { Button("Projects") { selectProject("") } } }
            }
            .sheet(isPresented: $modelsOpen) { ModelPickerView(providerID: $providerID, modelID: $modelID, folder: folder ?? [:]) }
            .task
            {
                selectProject(initialProjectID)
                chooseDefaultsIfNeeded()
            }
            .onChange(of: model.providers.count) { chooseDefaultsIfNeeded() }
            .onChange(of: model.settings.text("defaultNewChatProviderId")) { chooseDefaultsIfNeeded() }
            .onChange(of: scenePhase) { _, phase in if phase != .active { persistDraft() } }
            .onDisappear { persistDraft(); model.cancelPendingChatCreation() }
            .onChange(of: model.activeConnectionID) { _, _ in persistDraft(); dismiss() }
        }.presentationDragIndicator(.visible).presentationCornerRadius(34)
            .interactiveDismissDisabled(model.busyActions.contains("create") || model.isSending)
    }

    private var projectPicker: some View
    {
        ScrollView
        {
            LazyVStack(spacing: 12)
            {
                if model.folders.isEmpty
                {
                    EmptyContent(title: "No projects yet", message: "Add a workspace folder in UAM on your main computer. It will appear here automatically.", symbol: "folder.badge.plus")
                }
                ForEach(model.folders.filter { search.isEmpty || $0.text("title").localizedCaseInsensitiveContains(search) }, id: \.selfID) { folder in
                    Button { selectProject(folder.text("id")) } label: {
                        HStack(spacing: 15)
                        {
                            Image(systemName: "folder").font(.title3).foregroundStyle(.secondary)
                            VStack(alignment: .leading, spacing: 6)
                            {
                                Text(folder.text("title")).font(.headline).foregroundStyle(.primary)
                                Text(folder.text("directory")).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                            }
                            Spacer(minLength: 0)
                            Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary)
                        }.padding(22).frame(maxWidth: .infinity, alignment: .leading).background(UAMTheme.surface, in: .rect(cornerRadius: 26))
                    }.buttonStyle(.plain).disabled(folder.flag("missing")).accessibilityIdentifier("project-\(folder.text("id"))")
                }
            }.padding(20)
        }
        .safeAreaInset(edge: .bottom)
        {
            HStack(spacing: 10)
            {
                Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                TextField("Search projects", text: $search)
            }.padding(17).glassEffect(.regular, in: .capsule).padding(22)
        }
    }

    private func compose(_ folder: WireObject) -> some View
    {
        VStack(spacing: 20)
        {
            Text("What would you like to do\nin \(folder.text("title"))?")
                .font(.system(size: 25, weight: .medium)).multilineTextAlignment(.center).padding(.horizontal, 24).padding(.top, 40)
            Label(model.hostName(folder.text("executionHostId", "local")), systemImage: "laptopcomputer")
                .font(.subheadline).foregroundStyle(.secondary)
            Spacer(minLength: 20)
            if let error = model.error { ErrorStrip(message: error) { model.error = nil } }
            HStack
            {
                Label("\(folder.text("title"))", systemImage: "folder").lineLimit(1)
                Spacer()
                Text(providerName(providerID))
            }.font(.caption).foregroundStyle(.secondary).padding(.horizontal, 24)
        }
        .safeAreaInset(edge: .bottom)
        {
            VStack(spacing: 8)
            {
                if let context = draftContext, model.isDraftUnavailable(context)
                {
                    DraftRecoveryView(context: context, contextID: draftRecoveryContextID,
                                      isContextCurrent: { candidate, token in draftContext == candidate && draftRecoveryContextID == token },
                                      onRecovered: { value in cancelPendingDraftSave(); draft = value })
                        .id(context)
                }
                ComposerView(draft: $draft, modelName: modelID.isEmpty ? providerName(providerID) : modelID,
                         contextID: draftContextID, isContextCurrent: { $0 == draftContextID && draftContext?.connectionID == model.activeConnectionID }, forceExpanded: true,
                         sending: model.busyActions.contains("create") || model.isSending,
                         readOnly: model.isDraftUnavailable(draftContext),
                         onSend: {
                let submitted = draft
                let context = draftContext
                let submittedProvider = providerID
                let submittedModel = modelID
                persistDraft()
                Task {
                    guard let context, draftContext == context, context.connectionID == model.activeConnectionID else { return }
                    if await model.createChat(folder: folder, provider: submittedProvider, model: submittedModel, text: submitted.text, attachments: submitted.attachments)
                    {
                        // Settle the submitted revision only after any edits made while
                        // creating/sending have reached the original project draft.
                        persistDraft()
                        model.clearNewChatDraft(context: context, matchingRevision: submitted.revision)
                        draft = ComposerDraft(); draftContext = nil
                        dismiss()
                    }
                }
            }, onStop: {}, onModels: { modelsOpen = true }, onDraftChanged: { scheduleDraftSave() }, onError: { model.error = $0 })
            }
        }
    }

    private var draftRecoveryContextID: String { draftContextID + ":" + draftSaveGeneration.uuidString }
    private var draftContextID: String { "new-\(draftContext?.connectionID ?? "")-\(selectedProjectID)" }
    private func cancelPendingDraftSave()
    {
        draftSaveTask?.cancel()
        draftSaveTask = nil
        draftSaveGeneration = UUID()
    }
    private func scheduleDraftSave()
    {
        cancelPendingDraftSave()
        guard let context = draftContext, context.connectionID == model.activeConnectionID else { return }
        let value = draft
        let generation = draftSaveGeneration
        draftSaveTask = Task { @MainActor in
            do { try await Task.sleep(for: .milliseconds(350)) }
            catch { return }
            guard !Task.isCancelled, draftSaveGeneration == generation,
                  draftContext == context, context.connectionID == model.activeConnectionID else { return }
            model.saveNewChatDraft(value, context: context)
            draftSaveTask = nil
        }
    }
    private func persistDraft()
    {
        cancelPendingDraftSave()
        // A flush retains ownership of its original computer/project even if
        // connection navigation has just changed the active main computer.
        if let draftContext { model.saveNewChatDraft(draft, context: draftContext) }
    }
    private func selectProject(_ id: String)
    {
        if id != selectedProjectID { model.cancelPendingChatCreation(); providerID = ""; modelID = "" }
        persistDraft()
        selectedProjectID = id
        draftContext = id.isEmpty ? nil : model.newDraftContext(projectID: id)
        draft = draftContext.map { model.loadNewChatDraft(context: $0) } ?? ComposerDraft()
        chooseDefaultsIfNeeded()
    }

    private func chooseDefaultsIfNeeded()
    {
        guard providerID.isEmpty else { return }
        let hostID = folder?.text("executionHostId", "local") ?? "local"
        let scoped = model.settings.object("hostScopedSettingsValues").object(hostID)
        let selected = scoped.text("defaultNewChatProviderId", model.settings.text("defaultNewChatProviderId"))
        providerID = selected.isEmpty ? model.providers.first?.text("id") ?? "" : selected
        let defaults = scoped.object("providerChatDefaults").isEmpty ? model.settings.object("providerChatDefaults") : scoped.object("providerChatDefaults")
        modelID = defaults.object(providerID).text("modelId")
    }
}

struct ModelPickerView: View
{
    @Environment(MobileAppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @Binding var providerID: String
    @Binding var modelID: String
    var folder: WireObject = [:]
    var sourceChat: MobileChat?
    @State private var search = ""

    var body: some View
    {
        NavigationStack
        {
            List
            {
                ForEach(model.providers, id: \.selfID) { provider in
                    let id = provider.text("id")
                    Section
                    {
                        let available = models(id)
                        if available.isEmpty
                        {
                            Button { providerID = id; modelID = ""; dismiss() } label: {
                                HStack { Text("Provider default"); Spacer(); if id == providerID && modelID.isEmpty { Image(systemName: "checkmark") } }
                            }
                            Text("No model catalogue matches this computer and workspace. You can use the provider default.")
                                .font(.footnote).foregroundStyle(.secondary)
                        }
                        ForEach(available.filter { search.isEmpty || $0.text("name", $0.text("id")).localizedCaseInsensitiveContains(search) }, id: \.selfID) { item in
                            Button { providerID = id; modelID = item.text("id"); dismiss() } label: {
                                HStack
                                {
                                    Text(item.text("name", item.text("id"))).foregroundStyle(.primary)
                                    Spacer()
                                    if providerID == id && modelID == item.text("id") { Image(systemName: "checkmark").foregroundStyle(UAMTheme.accent) }
                                }.padding(.vertical, 5)
                            }
                        }
                    } header: { Label(provider.text("name", providerName(id)), systemImage: "circle.hexagongrid") }
                }
            }.scrollContentBackground(.hidden).background(UAMTheme.background)
                .searchable(text: $search, prompt: "Find a model")
                .navigationTitle("Choose model").navigationBarTitleDisplayMode(.inline)
                .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } } }
        }
    }

    private func models(_ provider: String) -> [WireObject]
    {
        var result: [WireObject] = []
        let source = sourceChat
        let hostID = source?.hostID ?? folder.text("executionHostId", "local")
        let workspace = source?.raw.text("workspaceDirectory") ?? folder.text("directory")
        if let chat = source, chat.providerID == provider, chat.hostID == hostID, chat.raw.text("workspaceDirectory") == workspace
        { result += chat.runtime.objects("availableModels") }
        result += model.catalogModels(providerID: provider, hostID: hostID, workspace: workspace)
        var seen = Set<String>()
        return result.filter { seen.insert($0.text("id")).inserted }
    }
}
