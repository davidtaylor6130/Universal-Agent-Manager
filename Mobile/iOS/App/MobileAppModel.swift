import Foundation
import Observation
import UAMKit

@MainActor @Observable
final class MobileAppModel
{
    var connections: [MobileConnection] = []
    var activeConnectionID: String?
    var chats: [MobileChat] = []
    var folders: [WireObject] = []
    var resourceCollections: [WireObject] = []
    var providers: [WireObject] = []
    var settings: WireObject = [:]
    var catalogs: [WireObject] = []
    var capabilities: WireObject = [:]
    var activeChatID: String?
    var messages: [MobileMessage] = []
    var historyStartIndex = 0
    var historyTotal = 0
    var connected = false
    var refreshing = false
    var loadingHistory = false
    var busyActions: Set<String> = []
    var error: String?
    private(set) var connectionProblem: String?
    private(set) var connectionRequiresPairing = false
    var lastUpdated: Date?
    var draft = ComposerDraft()
    var pendingPairingURL: URL?
    var appVersion = ""
    var turnElapsedMilliseconds: Double?
    var runtimeMetricsReceivedAt: Date?
    var loadingOlder = false
    private var unavailableDraftContexts: Set<DraftContext> = []
    private static let unavailableDraftMessage = "Your saved draft could not be read on this phone. Reopen it to retry; it has not been replaced."
    private static let discardDraftMessage = "Your unreadable draft could not be discarded on this phone. It has been kept."

    typealias RequestHandler = @MainActor (String, WireObject) async throws -> WireObject
    typealias PairingRequestHandler = @MainActor (CompanionClient) async throws -> WireObject
    @ObservationIgnored private let preferences: UserDefaults
    @ObservationIgnored private let draftStore: DraftStore
    @ObservationIgnored private let requestHandler: RequestHandler?
    @ObservationIgnored private let pairingRequestHandler: PairingRequestHandler?
    @ObservationIgnored private let credentialWriter: @MainActor (String, String) throws -> Void
    @ObservationIgnored private let credentialReader: @MainActor (String) -> String?
    @ObservationIgnored private var foregroundActive = true
    @ObservationIgnored private var draftNavigation = UUID()
    @ObservationIgnored private var creationEpoch = UUID()
    @ObservationIgnored private var discoveredCapabilities = false
    @ObservationIgnored private var client: CompanionClient?
    @ObservationIgnored private var generation = UUID()
    @ObservationIgnored private var messagesDigest = ""
    @ObservationIgnored private var bootID = ""
    @ObservationIgnored private var stateRevision: Double?
    @ObservationIgnored private var fullRefreshCountdown = 0
    @ObservationIgnored private var pollTask: Task<Void, Never>?
    @ObservationIgnored private var refreshInFlight = false
    @ObservationIgnored private var forcedRefreshPending = false
    @ObservationIgnored private var refreshWaiters: [CheckedContinuation<Void, Never>] = []
    @ObservationIgnored private var historyTask: Task<Void, Never>?
    @ObservationIgnored private var historyOwner: UUID?
    @ObservationIgnored private var historyRequestID: UUID?
    @ObservationIgnored private var draftWriteTask: Task<Void, Never>?

    var connection: MobileConnection? { connections.first { $0.id == activeConnectionID } }
    var chat: MobileChat? { chats.first { $0.id == activeChatID } }
    var isSending: Bool { busyActions.contains("send") }
    var draftContext: DraftContext?
    {
        guard let connectionID = activeConnectionID, let chatID = activeChatID else { return nil }
        return .chat(connectionID: connectionID, chatID: chatID)
    }
    var draftContextID: String { "\(generation):\(draftNavigation)" }
    var draftUnavailable: Bool { isDraftUnavailable(draftContext) }
    func isDraftUnavailable(_ context: DraftContext?) -> Bool
    { context.map { unavailableDraftContexts.contains($0) } ?? false }

    private func canWriteDraft(_ context: DraftContext) -> Bool
    {
        guard !unavailableDraftContexts.contains(context) else
        { error = Self.unavailableDraftMessage; return false }
        return true
    }

    init(preferences: UserDefaults = .standard, draftStore: DraftStore = DraftStore(), requestHandler: RequestHandler? = nil,
         pairingRequestHandler: PairingRequestHandler? = nil, credentialWriter: @escaping @MainActor (String, String) throws -> Void = { try ConnectionSecrets.write($0, for: $1) },
         credentialReader: @escaping @MainActor (String) -> String? = { ConnectionSecrets.read($0) })
    {
        self.preferences = preferences
        self.draftStore = draftStore
        self.requestHandler = requestHandler
        self.pairingRequestHandler = pairingRequestHandler
        self.credentialWriter = credentialWriter
        self.credentialReader = credentialReader
        if let data = preferences.data(forKey: "uam.connections"),
           let saved = try? JSONDecoder().decode([MobileConnection].self, from: data)
        { connections = saved }
        activeConnectionID = preferences.string(forKey: "uam.activeConnection")
        if !connections.contains(where: { $0.id == activeConnectionID }) { activeConnectionID = connections.first?.id }
        configureClient()
        #if DEBUG
        // Test injection uses the same real HTTP client; production has no fixture mode.
        if requestHandler == nil, let endpoint = ProcessInfo.processInfo.environment["UAM_TEST_ENDPOINT"],
           let token = ProcessInfo.processInfo.environment["UAM_TEST_TOKEN"]
        {
            let namespace = ProcessInfo.processInfo.environment["UAM_TEST_DRAFT_NAMESPACE"] ?? "shared"
            let test = MobileConnection(id: "ui-test-" + String(namespace.prefix(100)), name: ProcessInfo.processInfo.environment["UAM_TEST_CONNECTION_NAME"] ?? "Test Mac", address: endpoint)
            connections = [test]
            activeConnectionID = test.id
            // Unsigned simulator runners lack Keychain entitlements. The explicit
            // DEBUG fixture uses its disposable credential in memory only.
            if let url = URL(string: endpoint)
            {
                do { client = try CompanionClient(baseURL: url, token: token, allowLoopbackHTTP: true); connectionProblem = nil; connectionRequiresPairing = false }
                catch { client = nil; connectionProblem = Self.connectionReadProblem(error); connectionRequiresPairing = true }
            }
        }
        #endif
    }

    func folderName(_ id: String) -> String { folders.first { $0.text("id") == id }?.text("title") ?? "Workspace" }
    func hostName(_ id: String) -> String
    {
        settings.objects("executionHosts").first { $0.text("id") == id }?.text("label") ?? (id == "local" ? connection?.name ?? "Main computer" : id)
    }

    func supports(_ action: String) -> Bool
    {
        if let actions = capabilities["actions"] as? [WireObject]
        { return actions.contains { $0.text("action") == action } }
        guard let actions = capabilities["actions"] as? [String] ?? capabilities["supportedActions"] as? [String] else
        { return ["getInitialState", "getChatMessages", "createSession", "sendAcpPrompt", "stageChatAttachments", "setChatPinned", "setChatModel", "setChatProvider", "cancelAcpTurn", "resolveAcpPermission", "resolveAcpUserInput", "listUamAgents", "setChatUamAgent", "setChatUamControlEnabled", "setChatMemoryEnabled", "setChatCodexOptions", "setChatApprovalMode", "setAcpConfigOption"].contains(action) }
        return actions.contains(action)
    }

    func setForeground(_ active: Bool)
    {
        foregroundActive = active
        pollTask?.cancel()
        pollTask = nil
        if !active { saveDraftImmediately(); return }
        guard client != nil || requestHandler != nil else { return }
        pollTask = Task { [weak self] in
            while !Task.isCancelled
            {
                guard let self else { return }
                await self.refresh()
                let delay: Duration = self.chat?.processing == true ? .milliseconds(1200) : .seconds(3)
                do { try await Task.sleep(for: delay) } catch { return }
            }
        }
    }

    func addConnection(name: String, address: String, token: String, pairingCertificate: String? = nil, clearSavedCertificate: Bool = false) async throws
    {
        try Task.checkCancellation()
        let originGeneration = generation
        guard let url = URL(string: address.trimmingCharacters(in: .whitespacesAndNewlines)) else { throw MobileError.message("Enter a valid UAM address.") }
        let credential = token.trimmingCharacters(in: .whitespacesAndNewlines)
        let normalizedName = String(name.trimmingCharacters(in: .whitespacesAndNewlines).prefix(160))
        var item = try MobileConnection.paired(name: normalizedName.isEmpty ? url.host ?? "Main computer" : normalizedName, address: url, saved: connections, preferredID: activeConnectionID)
        if let pairingCertificate { item.pairingCertificate = pairingCertificate }
        else if clearSavedCertificate { item.pairingCertificate = nil }
        let candidate = try CompanionClient(baseURL: url, token: credential, pairingCertificate: item.pairingCertificate)
        do
        {
            let snapshot: WireObject
            if let pairingRequestHandler { snapshot = try await pairingRequestHandler(candidate) }
            else { snapshot = try wire(await candidate.request(action: "getInitialState", payload: [:])) }
            try Task.checkCancellation()
            guard generation == originGeneration else { throw CancellationError() }
            guard snapshot["chats"] is [WireObject], snapshot["folders"] is [WireObject] else { throw MobileError.message("This address did not return a UAM companion response.") }
            // No suspension between the final cancellation check and persistence/activation.
            // A cancelled check cannot install a credential when its response arrives later.
            try Task.checkCancellation()
            try credentialWriter(credential, item.id)
            if let index = connections.firstIndex(where: { $0.id == item.id }) { connections[index] = item }
            else { connections.append(item) }
            persistConnections()
            activateValidatedConnection(item.id, client: candidate, snapshot: snapshot)
        }
        catch { await candidate.disconnect(); throw error }
    }

    private func activateValidatedConnection(_ id: String, client candidate: CompanionClient, snapshot: WireObject)
    {
        saveDraftImmediately()
        pollTask?.cancel()
        let previousClient = client
        Task { await previousClient?.disconnect() }
        resetConnectionState()
        activeConnectionID = id
        preferences.set(id, forKey: "uam.activeConnection")
        client = candidate
        applySnapshot(snapshot)
        connected = true; lastUpdated = .now; fullRefreshCountdown = 12
        // The verified snapshot is the pairing result. Ordinary polling may later
        // report disconnection; it is not a second blocking pairing check.
        setForeground(foregroundActive)
    }

    func switchConnection(_ id: String)
    {
        saveDraftImmediately()
        pollTask?.cancel()
        let previousClient = client
        Task { await previousClient?.disconnect() }
        resetConnectionState()
        activeConnectionID = id
        preferences.set(id, forKey: "uam.activeConnection")
        configureClient()
        setForeground(foregroundActive)
    }

    func removeConnection(_ id: String)
    {
        ConnectionSecrets.remove(id)
        connections.removeAll { $0.id == id }
        persistConnections()
        if activeConnectionID == id
        {
            if let next = connections.first { switchConnection(next.id) }
            else
            {
                saveDraftImmediately()
                pollTask?.cancel(); pollTask = nil
                let previousClient = client
                Task { await previousClient?.disconnect() }
                client = nil; activeConnectionID = nil
                resetConnectionState()
                preferences.removeObject(forKey: "uam.activeConnection")
            }
        }
    }

    private func resetConnectionState()
    {
        generation = UUID(); draftNavigation = UUID(); creationEpoch = UUID(); discoveredCapabilities = false
        draftWriteTask?.cancel(); draftWriteTask = nil
        historyTask?.cancel(); historyTask = nil; historyOwner = nil; historyRequestID = nil
        chats = []; folders = []; resourceCollections = []; providers = []; catalogs = []; settings = [:]; capabilities = [:]
        activeChatID = nil; messages = []; draft = ComposerDraft(); bootID = ""; stateRevision = nil
        historyStartIndex = 0; historyTotal = 0; messagesDigest = ""; connected = false; error = nil; connectionProblem = nil; connectionRequiresPairing = false
        appVersion = ""; lastUpdated = nil; turnElapsedMilliseconds = nil; runtimeMetricsReceivedAt = nil
        refreshing = false; loadingHistory = false; loadingOlder = false; fullRefreshCountdown = 0
        busyActions = []
        // Existing requests still own their cleanup; never release refreshInFlight
        // until that request exits. Force a new snapshot when its cancellation settles.
        forcedRefreshPending = refreshInFlight && foregroundActive
        let waiters = refreshWaiters; refreshWaiters = []
        waiters.forEach { $0.resume() }
    }

    func refresh(force: Bool = false) async
    {
        guard client != nil || requestHandler != nil else
        {
            let waiters = refreshWaiters; refreshWaiters = []
            waiters.forEach { $0.resume() }
            return
        }
        if refreshInFlight
        {
            if force
            {
                forcedRefreshPending = true
                await withCheckedContinuation { refreshWaiters.append($0) }
            }
            return
        }
        refreshInFlight = true
        refreshing = force
        let stamp = generation
        defer
        {
            refreshInFlight = false; refreshing = false
            if forcedRefreshPending
            {
                forcedRefreshPending = false
                Task { [weak self] in await self?.refresh(force: true) }
            }
            else
            {
                let waiters = refreshWaiters; refreshWaiters = []
                waiters.forEach { $0.resume() }
            }
        }
        do
        {
            var payload: WireObject = [:]
            if !force, fullRefreshCountdown > 0, !bootID.isEmpty, let stateRevision
            { payload = ["knownBootId": bootID, "knownStateRevision": stateRevision] }
            let state = try await request("getInitialState", payload)
            guard generation == stamp else { return }
            if !state.flag("unchanged")
            {
                applySnapshot(state)
                fullRefreshCountdown = 12
            }
            else { fullRefreshCountdown -= 1 }
            connected = true; lastUpdated = .now; connectionProblem = nil; connectionRequiresPairing = false
            if !discoveredCapabilities
            {
                do
                {
                    let caps = try await request("getCompanionCapabilities")
                    guard generation == stamp else { return }
                    capabilities = caps; discoveredCapabilities = true
                }
                catch CompanionError.http(let status) where status == 403 || status == 404
                { guard generation == stamp else { return }; discoveredCapabilities = true }
                catch
                {
                    // Discovery is optional. Its failure cannot invalidate the
                    // snapshot already accepted above; transient failures retry
                    // on the next poll, while unsupported discovery stays cached.
                    guard generation == stamp, !Task.isCancelled else { return }
                }
            }
            if let id = activeChatID
            {
                await loadMessages(id, force: force)
                if generation == stamp, activeChatID == id, chat?.processing == true, supports("getChatRuntimeMetrics"),
                   let metrics = try? await request("getChatRuntimeMetrics", ["chatId": id]),
                   generation == stamp, activeChatID == id,
                   metrics.flag("processing"), metrics["elapsedMs"] is NSNumber
                {
                    turnElapsedMilliseconds = metrics.number("elapsedMs")
                    runtimeMetricsReceivedAt = .now
                }
            }
        }
        catch is CancellationError { }
        catch CompanionError.cancelled { }
        catch
        {
            guard generation == stamp else { return }
            connected = false
            connectionProblem = Self.connectionReadProblem(error)
            connectionRequiresPairing = Self.requiresPairing(error)
        }
    }

    private static func requiresPairing(_ error: any Error) -> Bool
    {
        guard let error = error as? CompanionError else { return false }
        switch error
        {
        case .http(401), .invalidToken, .network(-1206 ... -1200), .invalidPairingCertificate,
             .invalidEndpoint, .insecureEndpoint, .redirectForbidden, .http(404): return true
        default: return false
        }
    }

    private static func connectionReadProblem(_ error: any Error) -> String
    {
        guard let error = error as? CompanionError else
        { return "Universal Agent Manager could not be loaded. Check Phone Access on your computer and try again." }
        switch error
        {
        case .network(-1009):
            return "This iPhone is offline. Connect to a network that can reach your computer."
        case .network(-1001):
            return "Your computer did not respond in time. Check its current Phone Access address and network connection."
        case .network(-1206 ... -1200), .invalidPairingCertificate:
            return "The secure connection could not be verified. Scan a fresh pairing code from your computer’s Phone Access settings."
        case .network:
            return "Universal Agent Manager could not be reached. Check that it is running, Phone Access is enabled and this iPhone can reach your computer on the network."
        case .http(401), .invalidToken:
            return "This computer did not accept the saved access token. Pair it again from Settings → Environments → Add. Your local drafts will be kept."
        case .invalidEndpoint, .insecureEndpoint, .redirectForbidden, .http(404):
            return "Check the current HTTPS address in your computer’s Phone Access settings. Pair it again if that address has changed."
        default:
            return "Universal Agent Manager could not be loaded. Check Phone Access on your computer and try again."
        }
    }

    @discardableResult
    func openChat(_ id: String) async -> Bool
    {
        saveDraftImmediately()
        historyTask?.cancel()
        activeChatID = id; draftNavigation = UUID()
        turnElapsedMilliseconds = nil; runtimeMetricsReceivedAt = nil
        messages = []; messagesDigest = ""; historyStartIndex = 0; historyTotal = 0
        loadDraft()
        loadingHistory = true
        let navigation = draftNavigation
        let attentionRevision = chat?.raw.text("attentionRevision") ?? ""
        await loadMessages(id, force: true)
        guard draftNavigation == navigation else { return false }
        loadingHistory = false
        if historyTotal > 0, !attentionRevision.isEmpty, supports("acknowledgeChatAttention")
        {
            // Acknowledge the exact reply that was opened, never a newer completion.
            _ = try? await request("acknowledgeChatAttention", ["chatId": id, "attentionRevision": attentionRevision])
        }
        return draftNavigation == navigation
    }

    /// Preserve the source-message location while loading contiguous bounded pages.
    /// Navigation away cancels the owned read; no skipped history window is fabricated.
    @discardableResult
    func openMessageBranch(_ id: String, at messageIndex: Int) async -> Bool
    {
        guard messageIndex >= 0, !Task.isCancelled, await openChat(id) else { return false }
        let context = draftContextID
        while activeChatID == id, draftContextID == context, !Task.isCancelled,
              historyStartIndex > messageIndex
        {
            let previousStart = historyStartIndex
            await loadMessages(id, older: true)
            guard activeChatID == id, draftContextID == context, !Task.isCancelled else { return false }
            // Failed, changed or malformed pages cannot cause an automatic retry loop.
            guard historyStartIndex < previousStart else
            {
                if error == nil { error = "The branch opened, but its original message could not be loaded. Use Load earlier messages to try again." }
                return false
            }
        }
        guard activeChatID == id, draftContextID == context, !Task.isCancelled else { return false }
        let found = messages.contains(where: { $0.index == messageIndex })
        if !found, error == nil { error = "The branch opened, but its original message is unavailable in the saved history." }
        return found
    }

    func closeChat()
    {
        saveDraftImmediately()
        historyTask?.cancel(); historyTask = nil; historyOwner = nil; historyRequestID = nil
        draftWriteTask?.cancel(); draftWriteTask = nil
        loadingHistory = false; loadingOlder = false
        activeChatID = nil; draftNavigation = UUID(); messages = []; messagesDigest = ""; draft = ComposerDraft()
        turnElapsedMilliseconds = nil; runtimeMetricsReceivedAt = nil
    }

    func loadMessages(_ id: String, force: Bool = false, older: Bool = false) async
    {
        guard activeChatID == id, !Task.isCancelled else { return }
        let navigation = draftNavigation
        if historyOwner == navigation, let pending = historyTask
        {
            let pendingID = historyRequestID
            await pending.value
            guard draftNavigation == navigation, !Task.isCancelled else { return }
            if historyRequestID == pendingID { historyTask = nil; historyOwner = nil; historyRequestID = nil }
            // Explicit paging/forced reads run after the prior window, using its
            // current cursor; passive polls can reuse the request they awaited.
            if force || older { await loadMessages(id, force: force, older: older) }
            return
        }
        historyTask?.cancel()
        let requestID = UUID()
        let task = Task { [weak self] in
            guard let self else { return }
            await self.fetchMessages(id, force: force, older: older, navigation: navigation)
        }
        historyTask = task; historyOwner = navigation; historyRequestID = requestID
        await withTaskCancellationHandler { await task.value } onCancel: { task.cancel() }
        if historyRequestID == requestID { historyTask = nil; historyOwner = nil; historyRequestID = nil }
    }

    private func fetchMessages(_ id: String, force: Bool, older: Bool, navigation: UUID) async
    {
        guard activeChatID == id, draftNavigation == navigation, !Task.isCancelled else { return }
        if older { loadingOlder = true }
        let stamp = generation
        defer { if draftNavigation == navigation { loadingOlder = false } }
        do
        {
            var payload: WireObject = ["chatId": id, "limit": older ? 50 : max(50, min(200, messages.count)), "deferToolCallContent": true]
            if older { payload["before"] = historyStartIndex }
            else if !force, !messagesDigest.isEmpty { payload["messagesDigest"] = messagesDigest }
            var page = try await request("getChatMessages", payload)
            guard activeChatID == id, generation == stamp, draftNavigation == navigation, !Task.isCancelled else { return }
            guard !page.flag("unchanged") else { return }
            var start = Int(page.number("startIndex"))
            var loaded = page.objects("messages").enumerated().map { MobileMessage(index: start + $0.offset, raw: $0.element) }
            var merge = HistoryPageMerge(existing: messages, existingDigest: messagesDigest, loaded: loaded, pageDigest: page.text("messagesDigest"), older: older)
            if merge.needsTailRefresh
            {
                messagesDigest = ""
                page = try await request("getChatMessages", ["chatId": id, "limit": 50, "deferToolCallContent": true])
                guard activeChatID == id, generation == stamp, draftNavigation == navigation, !Task.isCancelled else { return }
                start = Int(page.number("startIndex"))
                loaded = page.objects("messages").enumerated().map { MobileMessage(index: start + $0.offset, raw: $0.element) }
                merge = HistoryPageMerge(existing: [], existingDigest: "", loaded: loaded, pageDigest: page.text("messagesDigest"), older: false)
            }
            messages = merge.messages
            historyStartIndex = messages.first?.index ?? start
            historyTotal = Int(page.number("totalCount"))
            messagesDigest = merge.digest
        }
        catch is CancellationError { }
        catch CompanionError.cancelled { }
        catch { if generation == stamp, activeChatID == id, draftNavigation == navigation { self.error = error.localizedDescription } }
    }

    var displayedMessages: [MobileMessage]
    {
        guard let chat, chat.processing else { return messages }
        let runtime = chat.runtime
        let events = runtime.objects("turnEvents")
        let text = events.filter { ["assistant_text", "context_compaction_summary"].contains($0.text("type")) }.map { $0.text("text") }.joined()
        let tools = runtime.objects("toolCalls")
        guard !text.isEmpty || !tools.isEmpty else { return messages }
        let index = Int(runtime.number("turnAssistantMessageIndex"))
        guard index >= 0 else { return messages }
        let stream = MobileMessage(index: index, raw: ["role": "assistant", "content": text, "toolCalls": tools, "blocks": events])
        if messages.contains(where: { $0.index == index }) { return messages.map { $0.index == index ? stream : $0 } }
        return messages + [stream]
    }

    func sendDraft() async
    {
        guard let context = draftContext, canWriteDraft(context) else { return }
        let sendingClient = client
        guard let id = activeChatID, let origin = draftContext, sendingClient != nil || requestHandler != nil, !isSending,
              !draft.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || !draft.attachments.isEmpty else { return }
        let sentDraft = draft
        let stamp = generation
        busyActions.insert("send")
        defer { if generation == stamp { busyActions.remove("send") } }
        do
        {
            // Persist before making a request; errors and ambiguous acknowledgements retain it.
            try draftStore.save(sentDraft, in: origin)
            var staged: [WireObject] = []
            if !sentDraft.attachments.isEmpty
            {
                let items: [WireObject] = sentDraft.attachments.map { ["name": $0.name, "kind": $0.isImage ? "image" : "file", "mimeType": $0.mimeType, "dataBase64": $0.data.base64EncodedString()] }
                staged = try await request("stageChatAttachments", ["chatId": id, "items": items], using: sendingClient).objects("attachments")
                guard generation == stamp else { return }
            }
            guard canWriteDraft(origin) else { return }
            _ = try await request("sendAcpPrompt", ["chatId": id, "text": sentDraft.text, "attachments": staged, "markdownStoreFiles": [], "steerNow": false], using: sendingClient)
            // A failed reload while sending must not replace the preserved disk draft.
            guard canWriteDraft(origin) else { return }
            // Acknowledgement belongs to the captured origin, even after chat navigation.
            if draftContext == origin { try draftStore.save(draft, in: origin) }
            let settled: ComposerDraft
            do { settled = try draftStore.settle(origin, sentRevision: sentDraft.revision) }
            catch { unavailableDraftContexts.insert(origin); throw error }
            if draftContext == origin { draft = settled }
            guard generation == stamp else { return }
            error = nil
            await refresh(force: true)
        }
        catch { if generation == stamp { self.error = error.localizedDescription } }
    }

    @discardableResult
    func perform(_ action: String, _ payload: WireObject = [:]) async -> Bool
    {
        guard !busyActions.contains(action) else { return false }
        let stamp = generation
        busyActions.insert(action)
        defer { if generation == stamp { busyActions.remove(action) } }
        do
        {
            _ = try await request(action, payload)
            guard generation == stamp else { return false }
            await refresh(force: true)
            return generation == stamp
        }
        catch { if generation == stamp { self.error = error.localizedDescription }; return false }
    }

    func cancelPendingChatCreation() { creationEpoch = UUID() }

    func createChat(folder: WireObject, provider: String, model: String, text: String, attachments: [ComposerAttachment]) async -> Bool
    {
        guard !busyActions.contains("create") else { return false }
        guard let source = newDraftContext(projectID: folder.text("id")), canWriteDraft(source) else { return false }
        let stamp = generation
        let navigation = draftNavigation
        let creation = creationEpoch
        busyActions.insert("create")
        defer { if generation == stamp { busyActions.remove("create") } }
        do
        {
            var payload: WireObject = ["folderId": folder.text("id"), "providerId": provider, "executionHostId": folder.text("executionHostId", "local"), "workspaceDirectory": folder.text("directory"), "selectChat": false]
            if !model.isEmpty { payload["defaults"] = ["modelId": model] }
            let result = try await request("createSession", payload)
            guard generation == stamp, draftNavigation == navigation, creationEpoch == creation else { return false }
            let id = result.text("chatId", result.text("id"))
            guard !id.isEmpty else { throw MobileError.message("UAM created no chat identifier. Refresh before trying again.") }
            await refresh(force: true)
            guard generation == stamp, draftNavigation == navigation, creationEpoch == creation else { return false }
            guard canWriteDraft(source) else { return false }
            guard await openChat(id), generation == stamp, activeChatID == id, creationEpoch == creation else { return false }
            guard canWriteDraft(source), let destination = draftContext, canWriteDraft(destination) else { return false }
            draft = ComposerDraft(text: text, attachments: attachments)
            saveDraftImmediately()
            if !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || !attachments.isEmpty { await sendDraft() }
            return true
        }
        catch { if generation == stamp, draftNavigation == navigation, creationEpoch == creation { self.error = error.localizedDescription }; return false }
    }

    func request(_ action: String, _ payload: WireObject = [:]) async throws -> WireObject
    {
        let stamp = generation
        let response: WireObject
        if let requestHandler { response = try await requestHandler(action, payload) }
        else
        {
            guard let client else { throw MobileError.message("Connect to your UAM computer first.") }
            response = try await request(action, payload, using: client)
        }
        guard generation == stamp else { throw CancellationError() }
        return response
    }

    private func request(_ action: String, _ payload: WireObject, using client: CompanionClient?) async throws -> WireObject
    {
        // The injected handler follows the same captured-origin send path in tests.
        if client == nil, let requestHandler { return try await requestHandler(action, payload) }
        guard let client else { throw MobileError.message("Connect to your UAM computer first.") }
        let value = try JSONDecoder().decode(JSONValue.self, from: JSONSerialization.data(withJSONObject: payload))
        return try wire(await client.request(action: action, payload: value))
    }

    private func wire(_ value: JSONValue) throws -> WireObject
    {
        if value == .null { return [:] }
        let object = try JSONSerialization.jsonObject(with: JSONEncoder().encode(value))
        if let dictionary = object as? WireObject { return dictionary }
        if let array = object as? [WireObject] { return ["items": array] }
        throw MobileError.message("UAM returned an unexpected response.")
    }

    func draftChanged()
    {
        draftWriteTask?.cancel()
        draftWriteTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(350))
            guard !Task.isCancelled else { return }
            self?.saveDraftImmediately()
        }
    }

    func addAttachment(_ item: ComposerAttachment)
    {
        guard let context = draftContext, canWriteDraft(context) else { return }
        guard item.data.count <= 20 * 1024 * 1024,
              draft.attachments.reduce(0, { $0 + $1.data.count }) + item.data.count <= 20 * 1024 * 1024,
              draft.attachments.count < 8 else { error = "Attach up to eight files, totalling 20 MB per message."; return }
        draft.attachments.append(item); saveDraftImmediately()
    }

    func saveDraftImmediately()
    {
        guard let context = draftContext, canWriteDraft(context) else { return }
        do { try draftStore.save(draft, in: context) }
        catch { self.error = "Your draft could not be saved on this phone." }
    }

    private func loadDraft()
    {
        guard let context = draftContext else { draft = ComposerDraft(); return }
        draft = loadStoredDraft(context)
    }

    private func loadStoredDraft(_ context: DraftContext) -> ComposerDraft
    {
        do
        {
            let loaded = try draftStore.load(context)
            unavailableDraftContexts.remove(context)
            if error == Self.unavailableDraftMessage || error == Self.discardDraftMessage { error = nil }
            return loaded
        }
        catch
        {
            unavailableDraftContexts.insert(context)
            self.error = Self.unavailableDraftMessage
            return ComposerDraft()
        }
    }

    /// Explicit recovery: failure preserves both the canonical file and the current editor.
    func reloadSavedDraft(context: DraftContext) -> ComposerDraft?
    {
        guard context.connectionID == activeConnectionID else { return nil }
        let loaded = loadStoredDraft(context)
        guard !isDraftUnavailable(context) else { return nil }
        if draftContext == context
        {
            draftWriteTask?.cancel(); draftWriteTask = nil
            draft = loaded
        }
        return loaded
    }

    /// The caller must obtain explicit confirmation; healthy and stale contexts are refused.
    func discardUnreadableDraft(context: DraftContext) -> ComposerDraft?
    {
        guard context.connectionID == activeConnectionID, isDraftUnavailable(context) else { return nil }
        let empty = ComposerDraft()
        do
        {
            // A canonical empty draft supersedes historical copies; deleting the file
            // would resurrect a legacy draft on the next read.
            try draftStore.save(empty, in: context)
            unavailableDraftContexts.remove(context)
            if draftContext == context
            {
                draftWriteTask?.cancel(); draftWriteTask = nil
                draft = empty
            }
            error = nil
            return empty
        }
        catch
        {
            self.error = Self.discardDraftMessage
            return nil
        }
    }

    func newDraftContext(projectID: String) -> DraftContext?
    {
        activeConnectionID.map { .newChat(connectionID: $0, projectID: projectID) }
    }

    func loadNewChatDraft(projectID: String) -> ComposerDraft
    {
        guard let context = newDraftContext(projectID: projectID) else { return ComposerDraft() }
        return loadNewChatDraft(context: context)
    }

    func loadNewChatDraft(context: DraftContext) -> ComposerDraft
    {
        loadStoredDraft(context)
    }

    func saveNewChatDraft(_ value: ComposerDraft, projectID: String)
    {
        guard let context = newDraftContext(projectID: projectID) else { return }
        saveNewChatDraft(value, context: context)
    }

    func saveNewChatDraft(_ value: ComposerDraft, context: DraftContext)
    {
        guard canWriteDraft(context) else { return }
        do { try draftStore.save(value, in: context) }
        catch { self.error = "Your draft could not be saved on this phone." }
    }

    func clearNewChatDraft(projectID: String, matchingRevision: UUID)
    {
        guard let context = newDraftContext(projectID: projectID) else { return }
        clearNewChatDraft(context: context, matchingRevision: matchingRevision)
    }

    func clearNewChatDraft(context: DraftContext, matchingRevision: UUID)
    {
        guard canWriteDraft(context) else { return }
        do { try draftStore.settle(context, sentRevision: matchingRevision) }
        catch { unavailableDraftContexts.insert(context); self.error = Self.unavailableDraftMessage }
    }

    private func configureClient()
    {
        if requestHandler != nil { client = nil; return }
        guard let connection else { client = nil; connectionProblem = nil; connectionRequiresPairing = false; return }
        guard let token = credentialReader(connection.id) else
        {
            client = nil
            connectionProblem = "This phone’s saved connection credential is unavailable; pair it again from Settings → Environments → Add. Its local drafts will be kept."
            connectionRequiresPairing = true
            return
        }
        do
        {
            guard let url = URL(string: connection.address) else { throw CompanionError.invalidEndpoint }
            #if DEBUG
            let testLoopback = ProcessInfo.processInfo.environment["UAM_TEST_ENDPOINT"] == connection.address
            client = try CompanionClient(baseURL: url, token: token, allowLoopbackHTTP: testLoopback, pairingCertificate: connection.pairingCertificate)
            #else
            client = try CompanionClient(baseURL: url, token: token, pairingCertificate: connection.pairingCertificate)
            #endif
            connectionProblem = nil; connectionRequiresPairing = false
        }
        catch { connectionProblem = Self.connectionReadProblem(error); connectionRequiresPairing = true; client = nil }
    }

    private func persistConnections()
    {
        if let data = try? JSONEncoder().encode(connections) { preferences.set(data, forKey: "uam.connections") }
    }

    private func applySnapshot(_ state: WireObject)
    {
        if !bootID.isEmpty, bootID != state.text("bootId")
        {
            capabilities = [:]; discoveredCapabilities = false; messagesDigest = ""
        }
        chats = state.objects("chats").map(MobileChat.init(raw:))
        folders = state.objects("folders"); providers = state.objects("providers")
        resourceCollections = state.objects("resourceCollections")
        settings = state.object("settings"); catalogs = state.objects("providerModelCatalogs")
        appVersion = state.text("appVersion")
        bootID = state.text("bootId"); stateRevision = state["stateRevision"] == nil ? nil : state.number("stateRevision")
        if let id = activeChatID, !chats.contains(where: { $0.id == id }) { closeChat() }
    }
}

enum MobileError: LocalizedError
{
    case message(String)
    var errorDescription: String? { switch self { case .message(let text): return text } }
}
