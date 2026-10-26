import SwiftUI
import UAMKit

struct ConversationView: View
{
    @Environment(MobileAppModel.self) private var model
    @State private var settingsOpen = false
    @State private var renameOpen = false
    @State private var deleteOpen = false
    @State private var renameText = ""
    @State private var atBottom = true
    @State private var firstScroll = true
    @State private var terminalOpen = false
    @State private var managedChildrenOpen = false
    @State private var queueSelection: QueueSelection?
    @State private var actionOrigin: QueueSelection?
    @State private var branchFocus: (chatID: String, index: Int)?
    @State private var branchRequestID: UUID?
    @State private var branchTask: Task<Void, Never>?

    var body: some View
    {
        @Bindable var model = model
        VStack(spacing: 0)
        {
            header.padding(.horizontal, 16).padding(.top, 6).padding(.bottom, 10)
                .frame(maxWidth: 720).frame(maxWidth: .infinity)
            Divider().opacity(0.4)
            if !model.connected
            {
                Label("Reconnecting · Your draft is saved on this phone", systemImage: "wifi.slash")
                    .font(.caption).foregroundStyle(.secondary).padding(10)
            }
            if let error = model.error { ErrorStrip(message: error) { model.error = nil } }
            transcript
        }
        .safeAreaInset(edge: .bottom, spacing: 6)
        {
            VStack(spacing: 8)
            {
                if let chat = model.chat
                {
                    if chat.needsInput
                    {
                        PendingInputView(chat: chat)
                            .id(chat.id + chat.runtime.object("pendingPermission").text("requestId") + chat.runtime.object("pendingUserInput").text("requestId"))
                            .padding(.horizontal, 14)
                    }
                    let queueCount = chat.runtime.objects("queuedPrompts").count
                    let admissionCount = chat.runtime.objects("pendingDirectPrompts").count + chat.runtime.objects("unsentDirectPrompts").count
                    if queueCount + admissionCount > 0
                    {
                        Button
                        { queueSelection = QueueSelection(chatID: chat.id, contextID: model.draftContextID) } label: {
                            Label(queueCount > 0 ? "\(queueCount) queued · View messages" : "\(admissionCount) pending · View messages", systemImage: "text.bubble")
                                .font(.caption).padding(.vertical, 8)
                        }.buttonStyle(.plain).accessibilityIdentifier("queuedMessages")
                    }
                    if chat.processing
                    {
                        HStack(spacing: 9)
                        {
                            ProgressView().controlSize(.mini).tint(UAMTheme.gold)
                            TimelineView(.periodic(from: .now, by: 1)) { timeline in
                                Text(workingLabel(at: timeline.date)).font(.caption.weight(.medium)).foregroundStyle(UAMTheme.gold)
                            }
                        }.fixedSize(horizontal: true, vertical: false)
                            .padding(.horizontal, 16).padding(.vertical, 11).glassEffect(.regular, in: .capsule)
                    }
                    if let context = model.draftContext, model.isDraftUnavailable(context)
                    {
                        let navigation = model.draftContextID
                        DraftRecoveryView(context: context, contextID: navigation,
                                          isContextCurrent: { candidate, token in model.draftContext == candidate && model.draftContextID == token })
                            .id(context)
                    }
                    let composerContext = model.draftContextID
                    ComposerView(draft: $model.draft,
                                 modelName: chat.runtime.text("currentModelId", chat.modelID),
                                 contextID: model.draftContextID, isContextCurrent: { $0 == model.draftContextID },
                                 processing: chat.processing, sending: model.isSending, readOnly: chat.readOnly || model.draftUnavailable,
                                 onSend: { Task { if model.draftContextID == composerContext { await model.sendDraft() } } },
                                 onStop: { Task { if model.draftContextID == composerContext { await model.perform("cancelAcpTurn", ["chatId": chat.id]) } } },
                                 onModels: { settingsOpen = true }, onDraftChanged: { model.draftChanged() },
                                 onError: { model.error = $0 })
                        .id(chat.id)
                }
            }.padding(.top, 7).frame(maxWidth: 720).frame(maxWidth: .infinity)
        }
        .overlay(alignment: .top)
        {
            if branchRequestID != nil
            {
                HStack(spacing: 10)
                {
                    ProgressView().controlSize(.small)
                    Text("Opening message branch…").font(.caption)
                    Button("Cancel") { cancelBranchOpening() }
                        .font(.caption.weight(.semibold)).frame(minWidth: 44, minHeight: 44)
                        .accessibilityLabel("Cancel opening branch").accessibilityIdentifier("cancelBranchOpening")
                }.padding(.horizontal, 12).background(UAMTheme.surface, in: .rect(cornerRadius: 16))
                    .padding(.top, 58)
            }
        }
        .onDisappear { cancelBranchOpening() }
        .sheet(isPresented: $settingsOpen) { ChatSettingsView() }
        .sheet(isPresented: $terminalOpen) { TerminalView() }
        .sheet(isPresented: $managedChildrenOpen)
        { if let chat = model.chat { ManagedChildrenView(chatID: chat.id, contextID: model.draftContextID, parentName: chat.title) } }
        .sheet(item: $queueSelection) { selection in QueuedMessagesView(chatID: selection.chatID, contextID: selection.contextID) }
        .alert("Rename chat", isPresented: $renameOpen)
        {
            TextField("Chat name", text: $renameText)
            Button("Cancel", role: .cancel) { }
            Button("Save")
            {
                guard let origin = actionOrigin else { return }
                let title = renameText
                Task { if model.draftContextID == origin.contextID { await model.perform("renameSession", ["chatId": origin.chatID, "title": title]) } }
            }
        }
        .confirmationDialog("Delete this chat?", isPresented: $deleteOpen, titleVisibility: .visible)
        {
            Button("Delete chat", role: .destructive)
            {
                guard let origin = actionOrigin else { return }
                Task
                {
                    guard model.draftContextID == origin.contextID else { return }
                    if await model.perform("deleteSession", ["chatId": origin.chatID]), model.draftContextID == origin.contextID { model.closeChat() }
                }
            }
        } message: { Text("This deletes the chat from your main computer too.") }
        .onChange(of: model.draftContextID)
        {
            renameOpen = false; deleteOpen = false; actionOrigin = nil
            settingsOpen = false; terminalOpen = false; managedChildrenOpen = false; queueSelection = nil
            if branchFocus?.chatID != model.activeChatID { cancelBranchOpening() }
            firstScroll = branchFocus == nil
        }
    }

    private var header: some View
    {
        HStack(spacing: 11)
        {
            RoundButton(title: "Back to chats", symbol: "chevron.left") { model.closeChat() }
            Button { settingsOpen = true } label: {
                VStack(alignment: .leading, spacing: 2)
                {
                    Text(model.chat?.title ?? "Chat").font(.headline).lineLimit(1)
                    Text(model.folderName(model.chat?.folderID ?? "")).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }.frame(maxWidth: .infinity, alignment: .leading)
            }.buttonStyle(.plain).accessibilityLabel("Chat settings")
            HStack(spacing: 0)
            {
                Menu
                {
                    Button("Chat settings", systemImage: "slider.horizontal.3") { settingsOpen = true }
                    if model.supports("listManagedSubagents")
                    { Button("Subagents", systemImage: "person.2") { managedChildrenOpen = true }.accessibilityIdentifier("managedSubagents") }
                    if let parent = agentParent
                    {
                        let context = model.draftContextID
                        Button("Return to parent chat", systemImage: "arrow.turn.up.backward")
                        { Task { if model.draftContextID == context { await model.openChat(parent.id) } } }
                            .accessibilityIdentifier("returnToParentChat")
                    }
                    if let chat = model.chat
                    {
                        let context = model.draftContextID
                        if chat.processing, !chat.readOnly, model.supports("cancelAcpTurn")
                        {
                            Button("Stop response", systemImage: "stop.circle", role: .destructive)
                            {
                                let context = model.draftContextID
                                Task { if model.draftContextID == context { await model.perform("cancelAcpTurn", ["chatId": chat.id]) } }
                            }
                        }
                        Button(chat.pinned ? "Unpin chat" : "Pin chat", systemImage: "pin") { Task { if model.draftContextID == context { await model.perform("setChatPinned", ["chatId": chat.id, "pinned": !chat.pinned]) } } }
                        if model.supports("renameSession") { Button("Rename", systemImage: "pencil") { actionOrigin = QueueSelection(chatID: chat.id, contextID: model.draftContextID); renameText = chat.title; renameOpen = true } }
                        if model.supports("setChatSnoozedUntil")
                        { Button("Snooze for one hour", systemImage: "moon") { Task { if model.draftContextID == context { await model.perform("setChatSnoozedUntil", ["chatId": chat.id, "untilMs": Int(Date.now.addingTimeInterval(3600).timeIntervalSince1970 * 1000)]) } } } }
                        if chat.raw.object("cliTerminal").flag("running") { Button("CLI view", systemImage: "terminal") { terminalOpen = true } }
                        if model.supports("deleteSession") { Button("Delete chat", systemImage: "trash", role: .destructive) { actionOrigin = QueueSelection(chatID: chat.id, contextID: model.draftContextID); deleteOpen = true } }
                    }
                } label: { Image(systemName: "ellipsis").font(.system(size: 21)).frame(width: 46, height: 46) }
                .accessibilityLabel("Chat actions")
            }.buttonStyle(.plain).glassEffect(.regular.interactive(), in: .capsule)
        }
    }

    private var transcript: some View
    {
        ScrollViewReader { scroll in
            ScrollView
            {
                LazyVStack(alignment: .leading, spacing: 24)
                {
                    if model.historyStartIndex > 0
                    {
                        Button { Task { await model.loadMessages(model.activeChatID ?? "", older: true) } } label: {
                            if model.loadingOlder { ProgressView() }
                            else { Label("Load earlier messages", systemImage: "arrow.up") }
                        }.font(.footnote).frame(maxWidth: .infinity).disabled(model.loadingOlder)
                    }
                    if model.loadingHistory { ProgressView("Loading conversation…").frame(maxWidth: .infinity).padding(.top, 50) }
                    else if model.displayedMessages.isEmpty
                    {
                        EmptyContent(title: "Start the conversation", message: "Send a message to \(providerName(model.chat?.providerID ?? "")) in this workspace.")
                            .padding(.top, 60)
                    }
                    if let chatID = model.activeChatID
                    {
                        ForEach(model.displayedMessages) { message in
                            MessageContent(message: message, chatID: chatID, contextID: model.draftContextID,
                                           branchesBusy: branchRequestID != nil, onSelectBranch: { target in
                                    guard branchRequestID == nil else { return }
                                    let origin = model.draftContextID
                                    let requestID = UUID()
                                    branchFocus = (target.id, message.index)
                                    branchRequestID = requestID
                                    branchTask = Task
                                    {
                                        defer { if branchRequestID == requestID { branchRequestID = nil; branchTask = nil } }
                                        guard model.draftContextID == origin else { return }
                                        let opened = await model.openMessageBranch(target.id, at: message.index)
                                        guard branchRequestID == requestID else { return }
                                        if opened, let focus = branchFocus, focus.chatID == model.activeChatID
                                        {
                                            // The observed context/cursor/count normally handles this;
                                            // also settle a coalesced render after the completed read.
                                            await Task.yield()
                                            guard branchRequestID == requestID, model.activeChatID == target.id else { return }
                                            scroll.scrollTo(model.draftContextID + ":" + String(focus.index), anchor: .top)
                                            firstScroll = false
                                        }
                                        branchFocus = nil
                                    }
                                })
                                .id(model.draftContextID + ":" + String(message.id))
                        }
                    }
                    Color.clear.frame(height: 2).id("latest")
                        .onAppear { atBottom = true }.onDisappear { atBottom = false }
                }.padding(.horizontal, 20).padding(.vertical, 20)
                    .frame(maxWidth: 720).frame(maxWidth: .infinity)
            }
            .scrollDismissesKeyboard(.interactively)
            .defaultScrollAnchor(.bottom)
            .onChange(of: model.draftContextID + ":" + String(model.historyStartIndex) + ":" + String(model.messages.count)) { _, _ in
                if let focus = branchFocus, focus.chatID == model.activeChatID
                {
                    if model.messages.contains(where: { $0.index == focus.index })
                    {
                        scroll.scrollTo(model.draftContextID + ":" + String(focus.index), anchor: .top)
                        branchFocus = nil; firstScroll = false
                    }
                }
                else if firstScroll || atBottom { scroll.scrollTo("latest", anchor: .bottom); firstScroll = false }
            }
            .onChange(of: model.chat?.runtime.objects("turnEvents").count) { _, _ in if atBottom { scroll.scrollTo("latest", anchor: .bottom) } }
            .overlay(alignment: .bottomTrailing)
            {
                if !atBottom, !model.messages.isEmpty
                { RoundButton(title: "Jump to latest", symbol: "chevron.down") { branchFocus = nil; withAnimation { scroll.scrollTo("latest", anchor: .bottom) } }.padding(12) }
            }
        }
    }

    private func cancelBranchOpening()
    {
        branchTask?.cancel(); branchTask = nil
        branchFocus = nil; branchRequestID = nil
    }

    private var agentParent: MobileChat?
    {
        guard let chat = model.chat else { return nil }
        let explicitParent = [chat.raw.text("temporaryParentChatId"), chat.raw.text("subagentParentChatId")].first { !$0.isEmpty } ?? ""
        let isMessageBranch = chat.raw["branchFromMessageIndex"] != nil && chat.raw.number("branchFromMessageIndex") >= 0
        let id = explicitParent.isEmpty && !isMessageBranch ? chat.parentID : explicitParent
        return model.chats.first { $0.id == id && $0.id != chat.id }
    }

    private func workingLabel(at date: Date) -> String
    {
        guard let elapsed = model.turnElapsedMilliseconds, let received = model.runtimeMetricsReceivedAt else { return "Working" }
        let seconds = max(0, Int(elapsed / 1000 + date.timeIntervalSince(received)))
        return "Working \(Duration.seconds(seconds).formatted(.units(allowed: [.hours, .minutes, .seconds], width: .narrow)))"
    }

}

struct MessageContent: View
{
    @Environment(MobileAppModel.self) private var model
    let message: MobileMessage
    let chatID: String
    let contextID: String
    let branchesBusy: Bool
    let onSelectBranch: (MobileChat) -> Void
    var storedChild = false
    @State private var openingSubagentID: String?
    @State private var subagentErrorID = ""
    @State private var subagentError = ""
    @State private var expandedToolGroups = Set<Int>()
    @State private var selectedTool: ToolSelection?

    var body: some View
    {
        VStack(alignment: message.role == "user" ? .trailing : .leading, spacing: 13)
        {
            if message.role == "user"
            {
                HStack
                {
                    Spacer(minLength: 35)
                    Text(message.content).font(.system(size: 16)).lineSpacing(4).textSelection(.enabled)
                        .padding(15).background(UAMTheme.surface, in: .rect(cornerRadius: 22))
                }
            }
            else
            {
                ForEach(TranscriptSegments.build(message)) { segment in timelineSegment(segment) }
            }
            if storedChild
            {
                ForEach(message.attachments, id: \.selfID) { attachment in
                    Label(attachment.text("name", "Stored attachment"), systemImage: "paperclip").font(.footnote).foregroundStyle(.secondary)
                }
            }
            else if !message.attachments.isEmpty
            {
                ForEach(message.attachments, id: \.selfID) { attachment in
                    MessageAttachmentView(attachment: attachment, messageIndex: message.index, chatID: chatID, contextID: contextID)
                }
            }
            if message.role == "user", !message.tools.isEmpty
            {
                if message.tools.count > 2
                {
                    toolGroup(message.tools, key: -1)
                }
                else { ForEach(message.tools, id: \.selfID) { tool in toolRow(tool) } }
            }
            if !storedChild, message.role == "user", let chat = model.chat,
               let branches = MessageBranchGroup.build(chat: chat, messageIndex: message.index, chats: model.chats)
            {
                HStack(spacing: 3)
                {
                    Button { if originIsCurrent, let previous = branches.previous { onSelectBranch(previous) } } label: {
                        Image(systemName: "chevron.backward").frame(width: 44, height: 44).contentShape(Rectangle())
                    }.disabled(branchesBusy || branches.previous == nil || !originIsCurrent)
                        .accessibilityLabel("Previous message branch")
                        .accessibilityIdentifier("previous-message-branch-\(message.index)")
                    Text("\(branches.current) / \(branches.total)").font(.caption).monospacedDigit()
                        .accessibilityLabel("Branch \(branches.current) of \(branches.total)")
                    Button { if originIsCurrent, let next = branches.next { onSelectBranch(next) } } label: {
                        Image(systemName: "chevron.forward").frame(width: 44, height: 44).contentShape(Rectangle())
                    }.disabled(branchesBusy || branches.next == nil || !originIsCurrent)
                        .accessibilityLabel("Next message branch")
                        .accessibilityIdentifier("next-message-branch-\(message.index)")
                }.buttonStyle(.plain).foregroundStyle(.secondary)
                    .accessibilityElement(children: .contain)
                    .accessibilityValue("\(branches.current)/\(branches.total)")
                    .accessibilityIdentifier("message-branch-\(message.index)")
                    .frame(maxWidth: .infinity, alignment: .trailing)
            }
            if message.raw.flag("acpPromptNotSent")
            { Label("Not sent · Your computer has retained this message", systemImage: "exclamationmark.circle").font(.caption).foregroundStyle(.orange) }
            if message.raw.flag("interrupted") { Text("Response stopped").font(.caption).foregroundStyle(.secondary) }
            if message.raw.number("processingTimeMs") > 0
            {
                Text("Worked for \(Duration.milliseconds(Int64(message.raw.number("processingTimeMs"))).formatted(.units(allowed: [.hours, .minutes, .seconds], width: .abbreviated)))")
                    .font(.caption).foregroundStyle(UAMTheme.gold)
            }
            if !message.content.isEmpty
            {
                HStack(spacing: 14)
                {
                    Button { UIPasteboard.general.string = message.content } label: { Image(systemName: "document.on.document").frame(width: 44, height: 36) }.accessibilityLabel("Copy message")
                    if let date = parseWireDate(message.raw.text("createdAt")) { Text(date, style: .time).font(.caption2) }
                }.font(.caption).foregroundStyle(.tertiary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .contextMenu
        {
            Button("Copy message", systemImage: "document.on.document") { UIPasteboard.general.string = message.content }
            if !storedChild, message.role == "user", model.supports("branchFromMessage"), model.chat?.readOnly == false
            {
                Button("Branch from here", systemImage: "arrow.triangle.branch") { Task { await branch() } }
            }
        }
        .sheet(item: $selectedTool) { item in ToolDetailView(tool: item.tool, messageIndex: item.messageIndex, chatID: item.chatID, contextID: item.contextID) }
    }

    @ViewBuilder private func timelineSegment(_ segment: TranscriptSegment) -> some View
    {
        switch segment.kind
        {
        case .text:
            if !segment.text.isEmpty { MarkdownContent(text: segment.text).frame(maxWidth: .infinity, alignment: .leading) }
        case .thought:
            DisclosureGroup("Thinking") { Text(segment.text).font(.subheadline).textSelection(.enabled) }.font(.footnote).foregroundStyle(.secondary)
        case .compaction:
            Label(segment.text, systemImage: "arrow.trianglehead.2.clockwise").font(.caption).foregroundStyle(.secondary)
        case .plan:
            DisclosureGroup("Plan")
            {
                if !segment.text.isEmpty { Text(segment.text).font(.subheadline) }
                ForEach(Array(message.raw.objects("planEntries").enumerated()), id: \.offset) { _, entry in
                    Label(entry.text("content"), systemImage: entry.text("status") == "completed" ? "checkmark.circle" : "circle").font(.subheadline)
                }
            }.font(.footnote).foregroundStyle(.secondary)
        case .tools:
            if segment.toolIDs.count > 2
            {
                toolGroup(segment.toolIDs.map(tool), key: segment.id)
            }
            else { ForEach(segment.toolIDs, id: \.self) { id in toolRow(tool(id)) } }
        }
    }

    private func toolGroup(_ tools: [WireObject], key: Int) -> some View
    {
        VStack(alignment: .leading, spacing: 0)
        {
            Button
            {
                if expandedToolGroups.contains(key) { expandedToolGroups.remove(key) }
                else { expandedToolGroups.insert(key) }
            } label: {
                HStack
                {
                    Text("\(tools.count) tool calls")
                    Spacer()
                    Image(systemName: expandedToolGroups.contains(key) ? "chevron.down" : "chevron.right")
                }.font(.footnote).foregroundStyle(.secondary).frame(minHeight: 44).contentShape(Rectangle())
            }.buttonStyle(.plain).accessibilityValue(expandedToolGroups.contains(key) ? "Expanded" : "Collapsed")
            if expandedToolGroups.contains(key) { ForEach(tools, id: \.selfID) { tool in toolRow(tool) } }
        }
    }

    private func tool(_ id: String) -> WireObject
    { message.tools.first { $0.text("id") == id } ?? ["id": id, "title": "Tool activity", "status": "pending"] }

    private func branch() async
    {
        guard originIsCurrent else { return }
        let sourceID = chatID
        let connection = model.activeConnectionID
        do
        {
            let response = try await model.request("branchFromMessage", ["chatId": sourceID, "messageIndex": message.index, "operationId": UUID().uuidString, "regenerate": false])
            guard connection == model.activeConnectionID, originIsCurrent else { return }
            await model.refresh(force: true)
            guard originIsCurrent, !response.text("chatId").isEmpty else { return }
            await model.openChat(response.text("chatId"))
        }
        catch { if connection == model.activeConnectionID { model.error = error.localizedDescription } }
    }

    private var originIsCurrent: Bool
    { model.activeChatID == chatID && model.draftContextID == contextID }

    private func openTool(_ tool: WireObject)
    {
        guard originIsCurrent else { return }
        selectedTool = ToolSelection(tool: tool, messageIndex: message.index, chatID: chatID, contextID: contextID)
    }

    private func openSubagent(_ tool: WireObject) async
    {
        guard originIsCurrent, openingSubagentID == nil else { return }
        let nativeID = tool.text("subAgentId").trimmingCharacters(in: .whitespacesAndNewlines)
        guard !nativeID.isEmpty else { openTool(tool); return }
        openingSubagentID = tool.text("id")
        subagentError = ""; subagentErrorID = ""
        defer { openingSubagentID = nil }
        // UAM-created children already have a durable UAM chat ID.
        if let child = model.chats.first(where: { $0.id == nativeID })
        { await model.openChat(child.id); return }
        do
        {
            // Provider session IDs are resolved by the shared backend in the source
            // chat's provider/workspace/host, never interpreted as UAM chat IDs.
            let result = try await model.request("openNativeSessionChat", ["chatId": chatID,
                "nativeSessionId": nativeID, "title": tool.text("subAgentTitle", tool.text("title")),
                "selectChat": false, "refreshHistory": true])
            guard originIsCurrent, !Task.isCancelled else { return }
            let targetID = result.text("chatId").trimmingCharacters(in: .whitespacesAndNewlines)
            guard !targetID.isEmpty else { throw MobileError.message("The provider returned no subagent conversation. You can still view its tool details.") }
            await model.refresh(force: true)
            guard originIsCurrent, !Task.isCancelled else { return }
            guard model.chats.contains(where: { $0.id == targetID }) else
            { throw MobileError.message("The subagent conversation is still loading. Tap it again after the chat list refreshes.") }
            await model.openChat(targetID)
        }
        catch
        {
            guard originIsCurrent, !Task.isCancelled else { return }
            subagentErrorID = tool.text("id")
            subagentError = "Subagent history is unavailable. \(error.localizedDescription)"
        }
    }

    private func toolRow(_ tool: WireObject) -> some View
    {
        VStack(alignment: .leading, spacing: 8)
        {
            if storedChild
            {
                DisclosureGroup(ToolSummary(tool).displayTitle)
                {
                    Text(tool.text("content").isEmpty ? "Full details are unavailable in this stored conversation." : tool.text("content"))
                        .font(.footnote).textSelection(.enabled)
                }.font(.footnote).foregroundStyle(.secondary).frame(minHeight: 44)
            }
            else
            {
            Button
            {
                if tool.flag("isSubAgent") { Task { await openSubagent(tool) } }
                else { openTool(tool) }
            } label: {
                HStack(alignment: .top, spacing: 10)
                {
                    Image(systemName: tool.flag("isSubAgent") ? "person.2" : tool.text("kind") == "execute" ? "terminal" : "hammer")
                        .frame(width: 18).padding(.top, 2)
                    VStack(alignment: .leading, spacing: 5)
                    {
                        Text(ToolSummary(tool).displayTitle).lineLimit(2).multilineTextAlignment(.leading)
                        if tool.flag("isSubAgent") { Text(tool.text("currentActivity", tool.text("status"))).font(.caption2).lineLimit(1) }
                    }
                    Spacer(minLength: 0)
                    if openingSubagentID == tool.text("id") { ProgressView().controlSize(.mini).accessibilityLabel("Loading subagent conversation") }
                    else { Image(systemName: "chevron.forward").font(.caption2).padding(.top, 4) }
                }.font(.system(size: 13)).foregroundStyle(.secondary).padding(.vertical, 7).frame(maxWidth: .infinity, minHeight: 44, alignment: .leading).contentShape(Rectangle())
                    .padding(.horizontal, tool.flag("isSubAgent") ? 13 : 0)
                    .overlay { if tool.flag("isSubAgent") { RoundedRectangle(cornerRadius: 14).stroke(.quaternary) } }
            }.buttonStyle(.plain)
                .disabled(openingSubagentID != nil)
                .accessibilityLabel(tool.flag("isSubAgent") ? "Open subagent: \(ToolSummary(tool).displayTitle)" : "Open tool: \(ToolSummary(tool).displayTitle)")
                .accessibilityIdentifier((tool.flag("isSubAgent") ? "openSubagent-" : "openTool-") + tool.text("id"))
                .contextMenu { Button("View tool details", systemImage: "terminal") { openTool(tool) } }
            }
            if subagentErrorID == tool.text("id"), !subagentError.isEmpty
            {
                Text(subagentError).font(.caption).foregroundStyle(.orange).accessibilityIdentifier("subagentError-" + tool.text("id"))
                Button("View tool details") { openTool(tool) }.font(.caption)
            }
        }
    }
}

private struct ToolSelection: Identifiable
{
    let id = UUID()
    let tool: WireObject
    let messageIndex: Int
    let chatID: String
    let contextID: String
}

private struct ToolDetailView: View
{
    @Environment(MobileAppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let tool: WireObject
    let messageIndex: Int
    let chatID: String
    let contextID: String
    @State private var content = ""
    @State private var error: String?
    @State private var loading = false
    @State private var nextOffset = 0
    @State private var hasMore = false

    var body: some View
    {
        NavigationStack
        {
            ScrollView
            {
                VStack(alignment: .leading, spacing: 16)
                {
                    Text(tool.text("status")).font(.caption).foregroundStyle(.secondary)
                    if tool.flag("isSubAgent"), tool.text("subAgentId").isEmpty
                    { Text("This provider did not expose a child session ID. These are the original tool details, rather than a separate conversation.").font(.footnote).foregroundStyle(.secondary) }
                    Text(content).font(.system(size: 12, design: .monospaced)).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
                    if loading { ProgressView() }
                    if hasMore { Button("Load more output") { Task { await load() } }.disabled(loading) }
                    if let error { Text(error).font(.footnote).foregroundStyle(.orange); Button("Retry") { Task { await load() } } }
                }.padding(20)
            }.background(UAMTheme.background).navigationTitle(detailTitle).navigationBarTitleDisplayMode(.inline)
                .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
                .task { content = tool.text("content"); if tool.flag("contentDeferred") || content.isEmpty { await load() } }
        }
    }

    private var detailTitle: String
    {
        var source = tool
        if !content.isEmpty { source["content"] = content }
        return ToolSummary(source).displayTitle
    }

    private func load() async
    {
        guard !loading else { return }
        guard model.activeChatID == chatID, model.draftContextID == contextID else
        { error = "The conversation changed. Close this tool and open it again from its original chat."; return }
        loading = true; error = nil
        defer { loading = false }
        do
        {
            let page = try await model.request("getToolCallContent", ["chatId": chatID, "messageIndex": messageIndex, "toolCallId": tool.text("id"), "offset": nextOffset])
            guard model.activeChatID == chatID, model.draftContextID == contextID, !Task.isCancelled else { return }
            content += page.text("content"); nextOffset = Int(page.number("nextOffset")); hasMore = page.flag("hasMore")
        }
        catch { if model.draftContextID == contextID, !Task.isCancelled { self.error = error.localizedDescription } }
    }
}

private struct MessageAttachmentView: View
{
    @Environment(MobileAppModel.self) private var model
    let attachment: WireObject
    let messageIndex: Int
    let chatID: String
    let contextID: String
    @State private var preview: UIImage?
    @State private var fullScreen = false
    @State private var status = ""
    var body: some View
    {
        VStack(alignment: .leading, spacing: 7)
        {
            if let preview
            { Button { fullScreen = true } label: { Image(uiImage: preview).resizable().scaledToFit().frame(maxHeight: 240).clipShape(.rect(cornerRadius: 16)) }.buttonStyle(.plain).accessibilityLabel("View \(attachment.text("name"))") }
            Label(attachment.text("name", "Attachment"), systemImage: "paperclip").font(.caption).foregroundStyle(.secondary)
            if !status.isEmpty { Text(status).font(.caption2).foregroundStyle(.secondary) }
        }
        .task(id: contextID + ":" + String(messageIndex) + ":" + attachment.text("id"))
        {
            preview = nil; status = ""
            guard model.activeChatID == chatID, model.draftContextID == contextID,
                  attachment.text("type").hasPrefix("image") || attachment.text("mimeType").hasPrefix("image/") else { return }
            do
            {
                let response = try await model.request("getAttachmentImagePreview", ["chatId": chatID, "messageIndex": messageIndex, "attachmentId": attachment.text("id")])
                guard model.activeChatID == chatID, model.draftContextID == contextID, !Task.isCancelled else { return }
                if let data = Data(base64Encoded: response.text("dataBase64")), data.count <= 524288 { preview = UIImage(data: data) }
                if preview == nil { status = "Preview unavailable · Original remains on your computer" }
            }
            catch { if model.draftContextID == contextID, !Task.isCancelled { status = "Preview unavailable" } }
        }
        .sheet(isPresented: $fullScreen)
        {
            NavigationStack
            {
                if let preview { Image(uiImage: preview).resizable().scaledToFit().padding().toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { fullScreen = false } } } }
            }
        }
    }
}


private struct QueueSelection: Identifiable
{
    let id = UUID()
    let chatID: String
    let contextID: String
}

private struct QueueRemoval: Identifiable
{
    let id: String
    let index: Int
    let revision: Int
}

private struct QueuedMessagesView: View
{
    @Environment(MobileAppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let chatID: String
    let contextID: String
    @State private var pending = false
    @State private var error: String?
    @State private var removal: QueueRemoval?

    private var originIsCurrent: Bool { model.activeChatID == chatID && model.draftContextID == contextID }
    private var chat: MobileChat? { originIsCurrent ? model.chats.first { $0.id == chatID } : nil }

    var body: some View
    {
        NavigationStack
        {
            List
            {
                if let chat
                {
                    let queued = chat.runtime.objects("queuedPrompts")
                    if !queued.isEmpty
                    {
                        Section
                        {
                            ForEach(Array(queued.enumerated()), id: \.offset) { index, prompt in
                                VStack(alignment: .leading, spacing: 12)
                                {
                                    promptContent(prompt)
                                    HStack(spacing: 18)
                                    {
                                        Button("Steer now", systemImage: "arrow.turn.up.right")
                                        { Task { await manage("steer", index: index, id: prompt.text("id"), revision: Int(prompt.number("revision"))) } }
                                            .accessibilityIdentifier("steerQueued-" + prompt.text("id"))
                                            .disabled(chat.runtime.flag("queueHeld"))
                                        Spacer(minLength: 0)
                                        Button("Remove", systemImage: "trash", role: .destructive)
                                        { removal = QueueRemoval(id: prompt.text("id"), index: index, revision: Int(prompt.number("revision"))) }
                                            .accessibilityIdentifier("removeQueued-" + prompt.text("id"))
                                    }.font(.subheadline)
                                        .disabled(pending || chat.readOnly || prompt.flag("prepared") || prompt.text("id").isEmpty || prompt.number("revision") < 1 || !model.supports("manageQueuedAcpPrompt"))
                                }.padding(.vertical, 6)
                            }
                        } header: { Text(chat.runtime.flag("queueHeld") ? "Queue held" : "Queued on your computer") }
                        footer: {
                            Text(chat.runtime.flag("queueHeld")
                                 ? "Resume this held queue on your main computer. You can remove queued messages here."
                                 : "Queued messages follow the current response. Steer now explicitly redirects the active turn when the provider supports it.")
                        }
                    }
                    let pendingDirect = chat.runtime.objects("pendingDirectPrompts")
                    if !pendingDirect.isEmpty
                    {
                        Section
                        { ForEach(pendingDirect, id: \.selfID) { prompt in promptContent(prompt) } }
                        header: { Text("Sending or awaiting confirmation") }
                        footer: { Text("Delivery is owned by your computer. These messages are separate from the queue and are never automatically replayed by this phone.") }
                    }
                    let unsent = chat.runtime.objects("unsentDirectPrompts")
                    if !unsent.isEmpty
                    {
                        Section
                        { ForEach(unsent, id: \.selfID) { prompt in promptContent(prompt) } }
                        header: { Text("Unsent") }
                        footer: { Text("Your computer has retained these messages. Review and retry them there; this phone does not infer whether delivery is safe.") }
                    }
                    if queued.isEmpty && pendingDirect.isEmpty && unsent.isEmpty
                    { Text("No pending messages.").foregroundStyle(.secondary) }
                    if !queued.isEmpty, !model.supports("manageQueuedAcpPrompt")
                    { Text("Queue controls are unavailable through this computer’s companion interface.").font(.footnote).foregroundStyle(.secondary) }
                }
                else { Text("The conversation changed. Close this view and open its messages again.").foregroundStyle(.secondary) }
                if pending { ProgressView("Updating queue…") }
                if let error { Text(error).font(.footnote).foregroundStyle(.orange).accessibilityIdentifier("queuedMessagesError") }
            }.scrollContentBackground(.hidden).background(UAMTheme.background)
                .navigationTitle("Pending messages").navigationBarTitleDisplayMode(.inline)
                .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
                .confirmationDialog("Remove this queued message?", isPresented: Binding(get: { removal != nil }, set: { if !$0 { removal = nil } }), titleVisibility: .visible)
                {
                    if let selected = removal
                    {
                        Button("Remove message", role: .destructive)
                        { removal = nil; Task { await manage("remove", index: selected.index, id: selected.id, revision: selected.revision) } }
                    }
                    Button("Cancel", role: .cancel) { removal = nil }
                }
        }
    }

    private func promptContent(_ prompt: WireObject) -> some View
    {
        VStack(alignment: .leading, spacing: 6)
        {
            if !prompt.text("text").isEmpty { Text(prompt.text("text")).textSelection(.enabled) }
            ForEach(prompt.objects("attachments"), id: \.selfID) { attachment in
                Label(attachment.text("name", "Attachment"), systemImage: "paperclip").font(.caption).foregroundStyle(.secondary)
            }
            if let paths = prompt["markdownStoreFiles"] as? [String]
            { ForEach(paths, id: \.self) { path in Label(URL(fileURLWithPath: path).lastPathComponent, systemImage: "doc.text").font(.caption).foregroundStyle(.secondary) } }
            if !prompt.text("uamAgentId").isEmpty { Text("Agent: " + prompt.text("uamAgentId")).font(.caption).foregroundStyle(.secondary) }
        }
    }

    private func manage(_ operation: String, index: Int, id: String, revision: Int) async
    {
        guard originIsCurrent, !pending, let chat, !chat.readOnly,
              model.supports("manageQueuedAcpPrompt"), !id.isEmpty, revision > 0 else { return }
        let queued = chat.runtime.objects("queuedPrompts")
        guard queued.indices.contains(index), queued[index].text("id") == id,
              Int(queued[index].number("revision")) == revision, !queued[index].flag("prepared"),
              operation != "steer" || !chat.runtime.flag("queueHeld") else
        { error = "The queue changed. Review its current messages before trying again."; return }
        pending = true; error = nil
        defer { pending = false }
        do
        {
            _ = try await model.request("manageQueuedAcpPrompt", ["chatId": chatID, "operation": operation,
                "index": index, "expected": ["id": id, "revision": revision]])
            guard originIsCurrent else { return }
            await model.refresh(force: true)
        }
        catch { if originIsCurrent { self.error = error.localizedDescription } }
    }
}
