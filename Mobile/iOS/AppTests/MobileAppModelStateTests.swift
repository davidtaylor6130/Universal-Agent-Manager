import XCTest
import CryptoKit
import UAMKit
@testable import UAM

@MainActor final class MobileAppModelStateTests: XCTestCase
{
    @MainActor private final class Gate
    {
        struct Pending
        {
            let action: String
            let payload: WireObject
            let continuation: CheckedContinuation<Data, Never>
        }
        var calls: [Pending] = []
        var seenActions: [String] = []
        var heldActions: Set<String> = ["getChatMessages", "createSession", "renameSession"]

        func request(_ action: String, _ payload: WireObject) async -> WireObject
        {
            seenActions.append(action)
            if heldActions.contains(action)
            {
                let data: Data = await withCheckedContinuation { calls.append(.init(action: action, payload: payload, continuation: $0)) }
                return try! JSONSerialization.jsonObject(with: data) as! WireObject
            }
            if action == "getInitialState"
            { return ["chats": [["id": "a"], ["id": "b"]], "folders": [], "providers": [], "bootId": "test"] }
            return ["actions": []]
        }
        func release(_ index: Int, _ response: WireObject = [:])
        { calls[index].continuation.resume(returning: try! JSONSerialization.data(withJSONObject: response)) }
    }

    private func model(_ gate: Gate = Gate(), directory suppliedDirectory: URL? = nil, handler: MobileAppModel.RequestHandler? = nil) throws -> MobileAppModel
    {
        let suite = "uam-state-test-" + UUID().uuidString
        let preferences = try XCTUnwrap(UserDefaults(suiteName: suite))
        let directory = suppliedDirectory ?? FileManager.default.temporaryDirectory.appendingPathComponent(suite, isDirectory: true)
        addTeardownBlock { UserDefaults(suiteName: suite)?.removePersistentDomain(forName: suite); try? FileManager.default.removeItem(at: directory) }
        let model = MobileAppModel(preferences: preferences, draftStore: DraftStore(directory: directory), requestHandler: handler ?? gate.request)
        model.setForeground(false)
        model.activeConnectionID = "isolated-" + UUID().uuidString
        return model
    }

    private func waitForCalls(_ count: Int, _ gate: Gate) async
    {
        for _ in 0..<1000
        {
            if gate.calls.count >= count { return }
            await Task.yield()
        }
        XCTFail("Expected \(count) suspended local requests, observed \(gate.calls.count)")
    }

    private func page(_ id: String, _ text: String) -> WireObject
    { ["chatId": id, "startIndex": 0, "totalCount": 1, "messagesDigest": text, "messages": [["role": "assistant", "content": text]]] }

    @MainActor private final class RefreshFixture
    {
        var stateFailure: CompanionError?
        var capabilitiesFailure: CompanionError?
        var stateUnchanged = false
        var resourceCollections: [WireObject]?
        var capabilitiesCalls = 0
        func request(_ action: String, _ payload: WireObject) async throws -> WireObject
        {
            if action == "getCompanionCapabilities"
            {
                capabilitiesCalls += 1
                if let capabilitiesFailure { throw capabilitiesFailure }
                return ["actions": [["action": "renameSession", "chatScoped": true, "readOnly": false]]]
            }
            if let stateFailure { throw stateFailure }
            if stateUnchanged { return ["unchanged": true] }
            var state: WireObject = ["chats": [["id": "a", "title": "Confirmed snapshot"]], "folders": [], "bootId": "fixture", "stateRevision": 1]
            if let resourceCollections { state["resourceCollections"] = resourceCollections }
            return state
        }
    }

    func testRefreshFailureRecoveryDoesNotUseMutationErrorStorage() async throws
    {
        let fixture = RefreshFixture(), model = try model(handler: fixture.request)
        fixture.stateFailure = .network(code: -1004)
        await model.refresh(force: true)
        XCTAssertFalse(model.connected)
        XCTAssertNil(model.lastUpdated)
        XCTAssertNil(model.error, "A connection read failure must not become a chat mutation warning")
        let unreachable = try XCTUnwrap(model.connectionProblem)
        XCTAssertTrue(unreachable.contains("could not be reached"))
        XCTAssertFalse(unreachable.contains("Check the chat"))
        fixture.stateFailure = nil
        await model.refresh(force: true)
        XCTAssertTrue(model.connected)
        XCTAssertNil(model.connectionProblem)
        XCTAssertEqual(model.chats.first?.title, "Confirmed snapshot")
        let updated = model.lastUpdated
        XCTAssertNotNil(updated)
        fixture.stateFailure = .network(code: -1001)
        await model.refresh(force: true)
        XCTAssertFalse(model.connected)
        XCTAssertEqual(model.lastUpdated, updated, "A failed read must retain the last successful snapshot time")
        XCTAssertEqual(model.chats.first?.title, "Confirmed snapshot")
        XCTAssertNil(model.error)
        XCTAssertTrue(model.connectionProblem?.contains("did not respond in time") == true)
        fixture.stateFailure = nil
        fixture.stateUnchanged = true
        await model.refresh()
        XCTAssertTrue(model.connected)
        XCTAssertNil(model.connectionProblem, "An unchanged snapshot is also a successful reconnect")
    }

    func testSnapshotPreservesCollectionHierarchyAndResetsConnectionOwnership() async throws
    {
        let fixture = RefreshFixture(), model = try model(handler: fixture.request)
        let collections: [WireObject] = [
            ["id": "work", "name": "Work", "collapsed": true,
             "references": [["id": "ref-b", "type": "workspace-folder", "target": "folder-b", "label": "Second"],
                            ["id": "ref-a", "type": "workspace-folder", "target": "folder-a", "label": "First"]],
             "customIcon": ["kind": "emoji", "value": "📁"], "futureMetadata": ["preserve": true]],
            ["id": "personal", "name": "Personal", "collapsed": false, "references": []]
        ]
        fixture.resourceCollections = collections
        await model.refresh(force: true)
        let retained = try JSONSerialization.data(withJSONObject: model.resourceCollections, options: .sortedKeys)
        XCTAssertEqual(retained, try JSONSerialization.data(withJSONObject: collections, options: .sortedKeys))
        fixture.resourceCollections = []
        fixture.stateUnchanged = true
        await model.refresh()
        XCTAssertEqual(model.resourceCollections.map { $0.text("id") }, ["work", "personal"])
        fixture.stateUnchanged = false
        await model.refresh(force: true)
        XCTAssertTrue(model.resourceCollections.isEmpty, "An authoritative empty collection list removes old hierarchy")

        fixture.resourceCollections = collections
        await model.refresh(force: true)
        let nextID = "isolated-" + UUID().uuidString
        model.connections = [.init(id: nextID, name: "Other fixture", address: "https://other.invalid")]
        model.switchConnection(nextID)
        XCTAssertTrue(model.resourceCollections.isEmpty, "Collections belong to the original main computer")
        fixture.resourceCollections = nil // Older hosts omit this optional field.
        await model.refresh(force: true)
        XCTAssertTrue(model.resourceCollections.isEmpty)
    }

    func testSnapshotReadsPreserveUnrelatedMutationAndDraftErrors() async throws
    {
        let fixture = RefreshFixture(), model = try model(handler: fixture.request)
        for original in ["Uncertain send: Check the chat before trying again.", "Your saved draft could not be read on this phone."]
        {
            model.error = original
            fixture.stateFailure = .network(code: -1004)
            await model.refresh(force: true)
            XCTAssertEqual(model.error, original)
            XCTAssertNotNil(model.connectionProblem)
            fixture.stateFailure = nil
            await model.refresh(force: true)
            XCTAssertTrue(model.connected)
            XCTAssertEqual(model.error, original)
            XCTAssertNil(model.connectionProblem)
            fixture.stateUnchanged = true
            await model.refresh()
            XCTAssertEqual(model.error, original)
            fixture.stateUnchanged = false
        }
    }

    func testOptionalCapabilitiesFailureDoesNotInvalidateAcceptedSnapshotAndCanRetry() async throws
    {
        let fixture = RefreshFixture(), model = try model(handler: fixture.request)
        fixture.capabilitiesFailure = .network(code: -1004)
        await model.refresh(force: true)
        XCTAssertTrue(model.connected)
        XCTAssertEqual(model.chats.first?.title, "Confirmed snapshot")
        XCTAssertNil(model.error)
        XCTAssertEqual(fixture.capabilitiesCalls, 1)
        XCTAssertNil(model.connectionProblem)
        fixture.capabilitiesFailure = nil
        fixture.stateUnchanged = true
        await model.refresh()
        XCTAssertTrue(model.connected)
        XCTAssertEqual(fixture.capabilitiesCalls, 2)
        XCTAssertTrue(model.supports("renameSession"))
        XCTAssertNil(model.connectionProblem)
    }

    func testUnsupportedCapabilitiesDiscoveryIsNotRepeatedAfterSuccessfulSnapshots() async throws
    {
        let fixture = RefreshFixture(), model = try model(handler: fixture.request)
        fixture.capabilitiesFailure = .http(status: 403)
        await model.refresh(force: true)
        fixture.stateUnchanged = true
        await model.refresh()
        XCTAssertTrue(model.connected)
        XCTAssertEqual(fixture.capabilitiesCalls, 1)
        XCTAssertNil(model.error)
    }

    func testConnectionReadMessagesAreContextualAndNeverEchoResponseSecrets() async throws
    {
        let fixture = RefreshFixture(), model = try model(handler: fixture.request)
        let cases: [(CompanionError, String, Bool)] = [
            (.network(code: -1009), "iPhone is offline", false),
            (.network(code: -1004), "could not be reached", false),
            (.network(code: -1001), "did not respond in time", false),
            (.http(status: 401), "saved access token", true),
            (.invalidToken, "saved access token", true),
            (.invalidPairingCertificate, "Scan a fresh pairing code", true),
            (.invalidEndpoint, "HTTPS address", true),
            (.insecureEndpoint, "HTTPS address", true),
            (.redirectForbidden, "HTTPS address", true),
            (.http(status: 404), "HTTPS address", true),
            (.http(status: 503), "could not be loaded", false),
            (.invalidResponse("response-secret-do-not-echo"), "could not be loaded", false)
        ] + (-1206 ... -1200).map { (.network(code: $0), "Scan a fresh pairing code", true) }
        for (failure, expected, requiresPairing) in cases
        {
            fixture.stateFailure = failure
            await model.refresh(force: true)
            let message = try XCTUnwrap(model.connectionProblem)
            XCTAssertTrue(message.contains(expected))
            XCTAssertEqual(model.connectionRequiresPairing, requiresPairing)
            XCTAssertFalse(message.contains("response-secret-do-not-echo"))
            XCTAssertFalse(message.contains("Check the chat"))
            XCTAssertNil(model.error)
            fixture.stateFailure = nil
            fixture.stateUnchanged = true
            await model.refresh()
            XCTAssertFalse(model.connectionRequiresPairing, "A successful unchanged snapshot clears pairing recovery")
            XCTAssertNil(model.connectionProblem)
            fixture.stateUnchanged = false
        }
        // A new transient failure replaces recovery guidance rather than keeping
        // an old Pair again action latched across different failure types.
        for failure in [CompanionError.http(status: 401), .network(code: -1004)]
        {
            fixture.stateFailure = failure
            await model.refresh(force: true)
            XCTAssertEqual(model.connectionRequiresPairing, failure == .http(status: 401))
        }
        fixture.stateFailure = .invalidToken
        await model.refresh(force: true)
        XCTAssertTrue(model.connectionRequiresPairing)
        let otherID = "isolated-" + UUID().uuidString
        model.connections = [.init(id: otherID, name: "Other fixture", address: "https://other.invalid")]
        model.switchConnection(otherID)
        XCTAssertNil(model.connectionProblem, "Connection feedback belongs to the previous origin")
        XCTAssertFalse(model.connectionRequiresPairing)
    }

    func testOptionalDiscoveryCannotRequirePairingAfterAnAuthenticatedSnapshot() async throws
    {
        let failures: [CompanionError] = [
            .http(status: 401), .http(status: 404), .invalidPairingCertificate,
            .network(code: -1202), .network(code: -1004)
        ]
        for failure in failures
        {
            let fixture = RefreshFixture(), model = try model(handler: fixture.request)
            fixture.capabilitiesFailure = failure
            model.error = "Preserve the uncertain send and its draft."
            await model.refresh(force: true)
            XCTAssertTrue(model.connected)
            XCTAssertNil(model.connectionProblem)
            XCTAssertFalse(model.connectionRequiresPairing)
            XCTAssertEqual(model.error, "Preserve the uncertain send and its draft.")
        }
    }

    func testRestoredConnectionWithoutCredentialExplainsPairingRecovery() throws
    {
        let suite = "uam-missing-credential-" + UUID().uuidString
        let preferences = try XCTUnwrap(UserDefaults(suiteName: suite))
        addTeardownBlock { UserDefaults(suiteName: suite)?.removePersistentDomain(forName: suite) }
        let connection = MobileConnection(id: UUID().uuidString, name: "Restored computer", address: "https://example.invalid")
        preferences.set(try JSONEncoder().encode([connection]), forKey: "uam.connections")
        preferences.set(connection.id, forKey: "uam.activeConnection")
        let model = MobileAppModel(preferences: preferences)
        model.setForeground(false)
        XCTAssertEqual(model.connection?.id, connection.id)
        XCTAssertFalse(model.connected)
        XCTAssertTrue(model.connectionRequiresPairing)
        XCTAssertTrue(model.connectionProblem?.contains("pair it again") == true)
        XCTAssertNil(model.error)
    }

    func testSavedConnectionSetupFailuresKeepRepairAvailableWithoutAClient() async throws
    {
        let cases: [(String, String?, String?)] = [
            ("https://saved.invalid", nil, nil),
            ("https://[", "fixture", nil),
            ("http://saved.invalid", "fixture", nil),
            ("https://saved.invalid", "fixture", "invalid-public-certificate"),
            ("https://saved.invalid", "token with spaces", nil)
        ]
        for (address, token, certificate) in cases
        {
            let suite = "uam-setup-recovery-" + UUID().uuidString
            let preferences = try XCTUnwrap(UserDefaults(suiteName: suite))
            addTeardownBlock { UserDefaults(suiteName: suite)?.removePersistentDomain(forName: suite) }
            let saved = MobileConnection(id: "saved", name: "Saved fixture", address: address, pairingCertificate: certificate)
            preferences.set(try JSONEncoder().encode([saved]), forKey: "uam.connections")
            preferences.set(saved.id, forKey: "uam.activeConnection")
            var availableToken = token
            let model = MobileAppModel(preferences: preferences, credentialReader: { _ in availableToken })
            model.setForeground(false)
            let problem = try XCTUnwrap(model.connectionProblem)
            XCTAssertTrue(model.connectionRequiresPairing)
            XCTAssertFalse(model.connected)
            XCTAssertNil(model.error)
            model.error = "Keep the unrelated draft error"
            await model.refresh(force: true) // There is no client to poll.
            XCTAssertEqual(model.connectionProblem, problem)
            XCTAssertTrue(model.connectionRequiresPairing)
            XCTAssertEqual(model.error, "Keep the unrelated draft error")
            model.error = nil
            XCTAssertEqual(model.connectionProblem, problem)
            XCTAssertTrue(model.connectionRequiresPairing)

            // Reconfiguration succeeds without a network call, and clears only
            // setup feedback; the new client can later perform its own refresh.
            model.connections = [.init(id: "valid", name: "Valid fixture", address: "https://valid.invalid")]
            availableToken = "fixture"
            model.switchConnection("valid")
            XCTAssertNil(model.connectionProblem)
            XCTAssertFalse(model.connectionRequiresPairing)
        }
    }

    func testRemovingLastConnectionClearsEveryConnectionProjection() throws
    {
        let model = try model()
        let id = try XCTUnwrap(model.activeConnectionID)
        model.connections = [.init(id: id, name: "Fixture", address: "https://example.invalid")]
        model.chats = [.init(raw: ["id": "a"])]; model.activeChatID = "a"
        model.providers = [["id": "private-provider"]]; model.folders = [["id": "folder"]]
        model.resourceCollections = [["id": "private-collection", "references": [["target": "folder"]]]]
        model.catalogs = [["id": "catalog"]]; model.settings = ["workspace": "private"]
        model.capabilities = ["actions": ["renameSession"]]; model.appVersion = "old"
        model.lastUpdated = .now; model.turnElapsedMilliseconds = 1500; model.runtimeMetricsReceivedAt = .now
        model.historyStartIndex = 30; model.historyTotal = 60; model.loadingHistory = true; model.loadingOlder = true
        model.busyActions = ["send"]; model.draft = .init(text: "retained on disk")
        model.removeConnection(id)
        XCTAssertNil(model.activeConnectionID); XCTAssertNil(model.activeChatID)
        XCTAssertTrue(model.chats.isEmpty && model.providers.isEmpty && model.folders.isEmpty && model.catalogs.isEmpty)
        XCTAssertTrue(model.resourceCollections.isEmpty)
        XCTAssertTrue(model.settings.isEmpty && model.capabilities.isEmpty && model.messages.isEmpty)
        XCTAssertEqual(model.appVersion, ""); XCTAssertNil(model.lastUpdated)
        XCTAssertNil(model.turnElapsedMilliseconds); XCTAssertNil(model.runtimeMetricsReceivedAt)
        XCTAssertEqual(model.historyStartIndex, 0); XCTAssertEqual(model.historyTotal, 0)
        XCTAssertFalse(model.loadingHistory || model.loadingOlder || model.refreshing || model.connected)
        XCTAssertTrue(model.busyActions.isEmpty); XCTAssertEqual(model.draft.text, "")
    }

    func testNavigationLoadsNewChatWithoutWaitingForOldHistoryAndRejectsReturnRace() async throws
    {
        let gate = Gate(), model = try model(gate)
        let first = Task { await model.openChat("a") }
        await waitForCalls(1, gate)
        let second = Task { await model.openChat("b") }
        await waitForCalls(2, gate)
        let reopened = Task { await model.openChat("a") }
        await waitForCalls(3, gate)
        gate.release(2, page("a", "current a"))
        let reopenedResult = await reopened.value
        XCTAssertTrue(reopenedResult)
        gate.release(1, page("b", "stale b")); gate.release(0, page("a", "stale a"))
        let firstResult = await first.value, secondResult = await second.value
        XCTAssertFalse(firstResult); XCTAssertFalse(secondResult)
        XCTAssertEqual(model.activeChatID, "a")
        XCTAssertEqual(model.messages.map(\.content), ["current a"])
        XCTAssertFalse(model.loadingHistory)
    }

    func testOldActionCannotClearNewConnectionsBusyFlag() async throws
    {
        let gate = Gate(), model = try model(gate)
        let old = Task { await model.perform("renameSession", ["chatId": "a"]) }
        await waitForCalls(1, gate)
        let nextID = "isolated-" + UUID().uuidString
        model.connections.append(.init(id: nextID, name: "Next", address: "https://next.invalid"))
        model.lastUpdated = .now; model.turnElapsedMilliseconds = 1000
        model.switchConnection(nextID)
        XCTAssertNil(model.lastUpdated); XCTAssertNil(model.turnElapsedMilliseconds)
        let current = Task { await model.perform("renameSession", ["chatId": "b"]) }
        await waitForCalls(2, gate)
        gate.release(0)
        let oldResult = await old.value
        XCTAssertFalse(oldResult)
        XCTAssertTrue(model.busyActions.contains("renameSession"))
        gate.release(1)
        let currentResult = await current.value
        XCTAssertTrue(currentResult)
        XCTAssertFalse(model.busyActions.contains("renameSession"))
    }

    func testDismissedCreationNeverOpensOrPromptsCreatedChat() async throws
    {
        let gate = Gate(), model = try model(gate)
        let create = Task { await model.createChat(folder: ["id": "folder"], provider: "codex-cli", model: "", text: "retain this", attachments: []) }
        await waitForCalls(1, gate)
        model.cancelPendingChatCreation()
        gate.release(0, ["chatId": "created"])
        let createResult = await create.value
        XCTAssertFalse(createResult)
        XCTAssertNil(model.activeChatID)
        XCTAssertEqual(gate.calls.map(\.action), ["createSession"])
    }

    func testDelayedCreationCannotStealNavigationOrOverwriteAnotherDraft() async throws
    {
        let gate = Gate(), model = try model(gate)
        let create = Task { await model.createChat(folder: ["id": "folder"], provider: "codex-cli", model: "", text: "new chat text", attachments: []) }
        await waitForCalls(1, gate)
        let open = Task { await model.openChat("b") }
        await waitForCalls(2, gate)
        gate.release(1, page("b", "current b"))
        let opened = await open.value
        XCTAssertTrue(opened)
        model.draft.text = "keep b draft"
        gate.release(0, ["chatId": "created"])
        let created = await create.value
        XCTAssertFalse(created)
        XCTAssertEqual(model.activeChatID, "b")
        XCTAssertEqual(model.draft.text, "keep b draft")
        XCTAssertFalse(gate.seenActions.contains("sendAcpPrompt"))
    }

    func testSwitchWhileBackgroundedDoesNotStartPolling() async throws
    {
        let gate = Gate(), model = try model(gate)
        let nextID = "isolated-" + UUID().uuidString
        model.connections.append(.init(id: nextID, name: "Next", address: "https://next.invalid"))
        model.switchConnection(nextID)
        for _ in 0..<20 { await Task.yield() }
        XCTAssertTrue(gate.seenActions.isEmpty)
    }

    private func isolatedDraftStore() throws -> DraftStore
    {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        addTeardownBlock { try? FileManager.default.removeItem(at: directory) }
        return DraftStore(directory: directory)
    }

    private func corruptManifest(_ store: DraftStore, _ context: DraftContext) throws -> URL
    {
        try store.save(.init(text: "Preserve the unreadable original"), in: context)
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        let hash = SHA256.hash(data: try encoder.encode(context)).map { String(format: "%02x", $0) }.joined()
        let url = store.directory.appendingPathComponent(hash + ".json")
        try Data("{unreadable fixture".utf8).write(to: url)
        return url
    }

    func testUnreadableChatDraftBlocksWritesAndSendUntilSuccessfulReopen() async throws
    {
        let store = try isolatedDraftStore(), gate = Gate()
        gate.heldActions = []
        let model = try model(gate, directory: store.directory)
        let connection = try XCTUnwrap(model.activeConnectionID)
        let damaged = DraftContext.chat(connectionID: connection, chatID: "a")
        let other = DraftContext.chat(connectionID: connection, chatID: "b")
        let manifest = try corruptManifest(store, damaged)
        let original = try Data(contentsOf: manifest)
        _ = await model.openChat("a")
        XCTAssertTrue(model.draftUnavailable)
        XCTAssertTrue(model.error?.contains("could not be read") == true)
        model.draft.text = "Must not replace the original"
        model.saveDraftImmediately()
        model.addAttachment(.init(name: "blocked", mimeType: "text/plain", data: Data([1])))
        XCTAssertTrue(model.draft.attachments.isEmpty)
        await model.sendDraft()
        XCTAssertEqual(try Data(contentsOf: manifest), original)
        XCTAssertFalse(gate.seenActions.contains("sendAcpPrompt"))
        XCTAssertFalse(gate.seenActions.contains("stageChatAttachments"))
        _ = await model.openChat("b")
        XCTAssertFalse(model.draftUnavailable)
        model.draft.text = "Other context still saves"
        model.saveDraftImmediately()
        XCTAssertEqual(try store.load(other).text, "Other context still saves")
        XCTAssertTrue(model.isDraftUnavailable(damaged))
        XCTAssertEqual(try Data(contentsOf: manifest), original)
        try store.save(.init(text: "Recovered original"), in: damaged)
        _ = await model.openChat("a")
        XCTAssertFalse(model.draftUnavailable)
        XCTAssertEqual(model.draft.text, "Recovered original")
        model.draft.text = "Editing works again"
        model.saveDraftImmediately()
        XCTAssertEqual(try store.load(damaged).text, "Editing works again")
    }

    func testUnreadableNewChatDraftBlocksAutosaveSettlementAndCreateOnlyInItsContext() async throws
    {
        let store = try isolatedDraftStore(), gate = Gate()
        let model = try model(gate, directory: store.directory)
        let damaged = try XCTUnwrap(model.newDraftContext(projectID: "damaged"))
        let other = try XCTUnwrap(model.newDraftContext(projectID: "other"))
        let manifest = try corruptManifest(store, damaged)
        let original = try Data(contentsOf: manifest)
        let fallback = model.loadNewChatDraft(context: damaged)
        XCTAssertTrue(model.isDraftUnavailable(damaged))
        model.saveNewChatDraft(.init(text: "Must not overwrite"), context: damaged)
        model.clearNewChatDraft(context: damaged, matchingRevision: fallback.revision)
        let created = await model.createChat(folder: ["id": "damaged"], provider: "codex-cli", model: "", text: "Must not send", attachments: [])
        XCTAssertFalse(created)
        XCTAssertTrue(gate.seenActions.isEmpty)
        XCTAssertEqual(try Data(contentsOf: manifest), original)
        _ = model.loadNewChatDraft(context: other)
        model.saveNewChatDraft(.init(text: "Other project"), context: other)
        XCTAssertEqual(try store.load(other).text, "Other project")
        XCTAssertTrue(model.isDraftUnavailable(damaged))
        try store.save(.init(text: "Recovered project"), in: damaged)
        let recovered = model.loadNewChatDraft(context: damaged)
        XCTAssertFalse(model.isDraftUnavailable(damaged))
        XCTAssertEqual(recovered.text, "Recovered project")
        model.saveNewChatDraft(.init(text: "Usable again"), context: damaged)
        XCTAssertEqual(try store.load(damaged).text, "Usable again")
    }

    func testCreatedChatCannotOverwriteUnreadableDestinationDraft() async throws
    {
        let store = try isolatedDraftStore(), gate = Gate()
        gate.heldActions = ["createSession"]
        let model = try model(gate, directory: store.directory)
        let destination = DraftContext.chat(connectionID: try XCTUnwrap(model.activeConnectionID), chatID: "a")
        let manifest = try corruptManifest(store, destination)
        let original = try Data(contentsOf: manifest)
        let pending = Task { await model.createChat(folder: ["id": "project"], provider: "codex-cli", model: "", text: "New prompt", attachments: []) }
        await waitForCalls(1, gate)
        gate.release(0, ["chatId": "a"])
        let created = await pending.value
        XCTAssertFalse(created)
        XCTAssertTrue(model.draftUnavailable)
        XCTAssertEqual(try Data(contentsOf: manifest), original)
        XCTAssertFalse(gate.seenActions.contains("sendAcpPrompt"))
    }

    func testExplicitDiscardRefusesHealthyOrOtherConnectionAndResumesUserSend() async throws
    {
        let store = try isolatedDraftStore(), gate = Gate()
        gate.heldActions = ["sendAcpPrompt"]
        let model = try model(gate, directory: store.directory)
        let damaged = DraftContext.chat(connectionID: try XCTUnwrap(model.activeConnectionID), chatID: "a")
        let healthy = DraftContext.chat(connectionID: damaged.connectionID, chatID: "b")
        let foreign = DraftContext.chat(connectionID: "another-connection", chatID: "a")
        let healthyDraft = ComposerDraft(text: "Keep this healthy draft")
        try store.save(healthyDraft, in: healthy)
        _ = try corruptManifest(store, damaged)
        let foreignManifest = try corruptManifest(store, foreign)
        let foreignBytes = try Data(contentsOf: foreignManifest)
        _ = await model.openChat("a")
        _ = model.loadNewChatDraft(context: foreign)
        XCTAssertTrue(model.draftUnavailable)
        XCTAssertNil(model.discardUnreadableDraft(context: healthy))
        XCTAssertNil(model.discardUnreadableDraft(context: foreign))
        XCTAssertEqual(try store.load(healthy), healthyDraft)
        XCTAssertEqual(try Data(contentsOf: foreignManifest), foreignBytes)
        XCTAssertTrue(model.isDraftUnavailable(foreign))
        let discarded = try XCTUnwrap(model.discardUnreadableDraft(context: damaged))
        XCTAssertEqual(discarded.text, "")
        XCTAssertTrue(discarded.attachments.isEmpty)
        XCTAssertEqual(model.draft, discarded)
        XCTAssertFalse(model.draftUnavailable)
        XCTAssertNil(model.error)
        XCTAssertFalse(gate.seenActions.contains("sendAcpPrompt"))
        model.draft.text = "Send only after explicit user action"
        model.saveDraftImmediately()
        let send = Task { await model.sendDraft() }
        await waitForCalls(1, gate)
        XCTAssertEqual(gate.calls[0].action, "sendAcpPrompt")
        XCTAssertEqual(gate.calls[0].payload.text("chatId"), "a")
        XCTAssertEqual(gate.calls[0].payload.text("text"), "Send only after explicit user action")
        XCTAssertEqual(try store.load(damaged).text, "Send only after explicit user action")
        gate.release(0)
        await send.value
        XCTAssertEqual(model.draft.text, "")
        XCTAssertEqual(try store.load(damaged).text, "")
        XCTAssertEqual(gate.seenActions.filter { $0 == "sendAcpPrompt" }.count, 1)
        XCTAssertEqual(try store.load(healthy), healthyDraft)
        XCTAssertEqual(try Data(contentsOf: foreignManifest), foreignBytes)
    }

    func testExplicitDiscardWritesCanonicalEmptyWithoutResurrectingLegacyOrChangingActiveChat() async throws
    {
        let store = try isolatedDraftStore(), gate = Gate()
        gate.heldActions = []
        let model = try model(gate, directory: store.directory)
        let context = DraftContext.chat(connectionID: try XCTUnwrap(model.activeConnectionID), chatID: "a")
        _ = try corruptManifest(store, context)
        let identity = Data("\(context.connectionID):\(context.itemID)".utf8)
        let legacyKeys = [SHA256.hash(data: identity).map { String(format: "%02x", $0) }.joined(),
                          identity.base64EncodedString().replacingOccurrences(of: "/", with: "_")]
        let legacyBytes = try JSONEncoder().encode(ComposerDraft(text: "Old draft must never return"))
        for key in legacyKeys { try legacyBytes.write(to: store.directory.appendingPathComponent(key + ".json")) }
        _ = await model.openChat("b")
        model.draft.text = "Active b stays intact"
        let activeDraft = model.draft
        _ = model.loadNewChatDraft(context: context)
        let discarded = try XCTUnwrap(model.discardUnreadableDraft(context: context))
        XCTAssertEqual(discarded.text, "")
        XCTAssertEqual(model.draft, activeDraft)
        XCTAssertEqual(model.activeChatID, "b")
        XCTAssertEqual(try DraftStore(directory: store.directory).load(context).text, "")
        for key in legacyKeys
        { XCTAssertEqual(try Data(contentsOf: store.directory.appendingPathComponent(key + ".json")), legacyBytes) }
        _ = await model.openChat("a")
        XCTAssertEqual(model.draft.text, "")
        XCTAssertFalse(model.draftUnavailable)
    }

    func testExplicitDiscardOfNewChatDraftReturnsUsableEmptyWithoutChangingConversation() async throws
    {
        let store = try isolatedDraftStore(), gate = Gate()
        gate.heldActions = []
        let model = try model(gate, directory: store.directory)
        _ = await model.openChat("b")
        model.draft.text = "Conversation remains untouched"
        let activeDraft = model.draft
        let context = try XCTUnwrap(model.newDraftContext(projectID: "project"))
        _ = try corruptManifest(store, context)
        _ = model.loadNewChatDraft(context: context)
        var empty = try XCTUnwrap(model.discardUnreadableDraft(context: context))
        XCTAssertEqual(empty.text, "")
        XCTAssertFalse(model.isDraftUnavailable(context))
        XCTAssertEqual(model.draft, activeDraft)
        empty.text = "New project draft after confirmation"
        model.saveNewChatDraft(empty, context: context)
        XCTAssertEqual(try store.load(context), empty)
        XCTAssertEqual(model.draft, activeDraft)
    }

    func testExplicitReloadPreservesFailedFileAndOnlyUpdatesMatchingActiveDraft() async throws
    {
        let store = try isolatedDraftStore(), gate = Gate()
        gate.heldActions = []
        let model = try model(gate, directory: store.directory)
        let context = DraftContext.chat(connectionID: try XCTUnwrap(model.activeConnectionID), chatID: "a")
        let manifest = try corruptManifest(store, context)
        let unreadableBytes = try Data(contentsOf: manifest)
        _ = await model.openChat("a")
        model.draft.text = "Do not replace the editor on failed retry"
        let before = model.draft
        XCTAssertNil(model.reloadSavedDraft(context: context))
        XCTAssertTrue(model.draftUnavailable)
        XCTAssertEqual(model.draft, before)
        XCTAssertEqual(try Data(contentsOf: manifest), unreadableBytes)
        let restored = ComposerDraft(text: "Recovered from disk")
        try store.save(restored, in: context)
        XCTAssertEqual(model.reloadSavedDraft(context: context), restored)
        XCTAssertFalse(model.draftUnavailable)
        XCTAssertEqual(model.draft, restored)
        let other = DraftContext.chat(connectionID: context.connectionID, chatID: "b")
        let otherDraft = ComposerDraft(text: "Other context")
        try store.save(otherDraft, in: other)
        XCTAssertEqual(model.reloadSavedDraft(context: other), otherDraft)
        XCTAssertEqual(model.draft, restored)
        let foreign = DraftContext.chat(connectionID: "stale-connection", chatID: "a")
        try store.save(.init(text: "Foreign draft"), in: foreign)
        XCTAssertNil(model.reloadSavedDraft(context: foreign))
        XCTAssertEqual(model.draft, restored)
        XCTAssertEqual(try store.load(foreign).text, "Foreign draft")
    }

}
