import XCTest
import UAMKit
@testable import UAM

/// Exercises the actual phone adapter, connection generations and parent draft store.
/// Every companion response is injected; no network, provider, Keychain or real profile is used.
@MainActor final class ManagedChildAppTests: XCTestCase
{
    private static let firstRun = "11111111-1111-4111-8111-111111111111"
    private static let secondRun = "22222222-2222-4222-8222-222222222222"
    private static let firstTurn = "33333333-3333-4333-8333-333333333333"
    private static let nextTurn = "44444444-4444-4444-8444-444444444444"

    @MainActor private final class CompanionFixture
    {
        static let actions = ["listManagedSubagents", "getManagedAgentTranscript", "cancelManagedSubagent", "submitManagedSubagentFollowup", "getChatMessages"]
        var calls: [(String, WireObject)] = []
        var scopeNumber = 0
        var stopped: Set<String> = []
        var historical = false
        var failReads = false
        var foreignTranscript = false
        var loseNextFollowupReply = false
        var receipt: WireObject?
        var originalFollowup: WireObject?
        var heldAction: String?
        private var pending: (Data, CheckedContinuation<Data, any Error>)?
        var isHolding: Bool { pending != nil }

        func root(_ id: String = "root", readOnly: Bool = false) -> MobileChat
        {
            MobileChat(raw: ["id": id, "title": "Synthetic parent", "createdAt": "2026-10-23T12:00:00Z", "providerId": "codex-cli",
                             "executionHostId": "local", "folderId": "fixture-project", "workspaceDirectory": "/synthetic/task0891",
                             "modelId": "OpenAI/ExactCase:V2", "importedReadOnly": readOnly])
        }

        private func row(_ runID: String) -> WireObject
        {
            let advanced = runID == ManagedChildAppTests.firstRun && receipt != nil
            return ["runId": runID, "transcriptChatId": "transcript-" + runID, "providerId": "codex-cli", "modelId": "OpenAI/ExactCase:V2",
                    "taskTitle": runID == ManagedChildAppTests.firstRun ? "First child" : "Second child", "summary": "Synthetic stored work",
                    "status": stopped.contains(runID) ? "cancelling" : advanced ? "running" : "completed", "canCancel": !stopped.contains(runID), "lastReported": historical,
                    "followup": ["canSend": !advanced, "code": advanced ? "busy" : "none", "currentTurnId": advanced ? ManagedChildAppTests.nextTurn : ManagedChildAppTests.firstTurn,
                                 "currentTurnNumber": advanced ? 2 : 1]]
        }

        func request(_ action: String, _ payload: WireObject) async throws -> WireObject
        {
            calls.append((action, payload))
            let response: WireObject
            switch action
            {
            case "getChatMessages":
                response = ["chatId": payload.text("chatId"), "startIndex": 0, "totalCount": 1, "messagesDigest": "fixture-history",
                            "messages": [["role": "assistant", "content": "Parent history remains here", "createdAt": "2026-10-23T12:00:00Z"]]]
            case "listManagedSubagents":
                if failReads { throw CompanionError.http(status: 409) }
                scopeNumber += 1
                let rows = [ManagedChildAppTests.firstRun, ManagedChildAppTests.secondRun]
                    .filter { payload.text("runId").isEmpty || payload.text("runId") == $0 }.map(row)
                response = ["chatId": payload.text("chatId"), "controlScopeId": "scope-\(scopeNumber)", "offset": 0,
                            "nextOffset": NSNull(), "totalCount": rows.count, "runs": rows]
            case "getManagedAgentTranscript":
                let run = payload.text("transcriptChatId").replacingOccurrences(of: "transcript-", with: "")
                response = ["runId": foreignTranscript ? "foreign-run" : run, "providerId": "codex-cli", "title": "Stored child", "lastReported": historical,
                            "messages": [["role": "assistant", "content": "Stored history for " + run, "createdAt": "2026-10-23T12:00:00Z"]]]
            case "cancelManagedSubagent":
                guard payload.text("controlScopeId") == "scope-\(scopeNumber)" else { throw CompanionError.http(status: 409) }
                stopped.insert(payload.text("runId"))
                response = ["ok": true, "runId": payload.text("runId")]
            case "submitManagedSubagentFollowup":
                guard payload.text("controlScopeId") == "scope-\(scopeNumber)" else { throw CompanionError.http(status: 409) }
                if let originalFollowup, let receipt
                {
                    guard ["chatId", "runId", "afterTurnId", "clientRequestId", "text"].allSatisfy({ originalFollowup.text($0) == payload.text($0) }) else
                    { throw CompanionError.invalidRequest("A retry changed the original request.") }
                    response = ["followup": ["accepted": true, "replayed": true, "code": "none", "acceptance": receipt]]
                }
                else
                {
                    originalFollowup = payload
                    let admitted: WireObject = ["rootChatId": payload.text("chatId"), "runId": payload.text("runId"),
                        "transcriptChatId": "transcript-" + payload.text("runId"), "clientRequestId": payload.text("clientRequestId"),
                        "turnId": ManagedChildAppTests.nextTurn, "turnNumber": 2, "statusAtAdmission": "queued"]
                    receipt = admitted
                    if loseNextFollowupReply { loseNextFollowupReply = false; throw CompanionError.network(code: -1001) }
                    response = ["followup": ["accepted": true, "replayed": false, "code": "none", "acceptance": admitted]]
                }
            default: throw CompanionError.invalidRequest("Unexpected action in isolated application test: " + action)
            }
            if heldAction == action
            {
                let data = try JSONSerialization.data(withJSONObject: response)
                let held: Data = try await withCheckedThrowingContinuation { pending = (data, $0) }
                return try JSONSerialization.jsonObject(with: held) as! WireObject
            }
            return response
        }

        func release()
        {
            let saved = pending; pending = nil
            saved?.1.resume(returning: saved!.0)
        }
        var mutations: [(String, WireObject)] { calls.filter { ["cancelManagedSubagent", "submitManagedSubagentFollowup"].contains($0.0) } }
    }

    private func app(_ fixture: CompanionFixture, readOnly: Bool = false) throws -> MobileAppModel
    {
        let suite = "uam-managed-app-test-" + UUID().uuidString
        let preferences = try XCTUnwrap(UserDefaults(suiteName: suite))
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(suite, isDirectory: true)
        addTeardownBlock { UserDefaults(suiteName: suite)?.removePersistentDomain(forName: suite); try? FileManager.default.removeItem(at: directory) }
        let model = MobileAppModel(preferences: preferences, draftStore: DraftStore(directory: directory), requestHandler: fixture.request,
                                   credentialWriter: { _, _ in XCTFail("Injected app tests cannot write credentials") }, credentialReader: { _ in nil })
        model.setForeground(false)
        model.activeConnectionID = "synthetic-main"
        model.connected = true
        model.capabilities = ["actions": CompanionFixture.actions]
        model.chats = [fixture.root(readOnly: readOnly), fixture.root("other")]
        return model
    }

    private func openParent(_ model: MobileAppModel) async
    { let opened = await model.openChat("root"); XCTAssertTrue(opened) }

    private func reader(_ model: MobileAppModel) async throws -> ManagedChildSession
    {
        let session = try XCTUnwrap(model.managedChildrenSession(chatID: "root", contextID: model.draftContextID))
        session.activate(); await session.refresh()
        return session
    }

    private func openFirst(_ session: ManagedChildSession) async throws -> ManagedChildRun
    {
        session.select(try XCTUnwrap(session.runs.first)); await session.refresh()
        return try XCTUnwrap(session.selected)
    }

    private func waitForHeld(_ fixture: CompanionFixture) async throws
    {
        let deadline = ContinuousClock.now.advanced(by: .seconds(5))
        while !fixture.isHolding, ContinuousClock.now < deadline { try await Task.sleep(for: .milliseconds(10)) }
        XCTAssertTrue(fixture.isHolding, "The injected reply must be held before navigation changes")
    }

    func testUnknownStopsProtectReaderCacheAndExplicitRecoveryReleasesCapacity() async throws
    {
        let fixture = CompanionFixture(), model = try app(fixture)
        let roots = (0...8).map { "cache-root-\($0)" }
        model.chats = roots.map { fixture.root($0) }
        var readers: [ManagedChildSession] = []
        for id in roots.prefix(8)
        {
            fixture.stopped = []
            let opened = await model.openChat(id); XCTAssertTrue(opened)
            let session = try XCTUnwrap(model.managedChildrenSession(chatID: id, contextID: model.draftContextID))
            session.activate(); await session.refresh()
            let child = try XCTUnwrap(session.runs.first)
            // Each synthetic root has independent controller work, despite
            // intentionally reusing child IDs to exercise root-scoped identity.
            fixture.heldAction = "cancelManagedSubagent"
            let pending = Task { await session.stop(child) }; try await waitForHeld(fixture)
            session.deactivate(); fixture.release(); await pending.value
            fixture.heldAction = nil
            XCTAssertEqual(session.stops[session.key(child)], .uncertain)
            readers.append(session)
        }
        let ninthOpened = await model.openChat(roots[8]); XCTAssertTrue(ninthOpened)
        XCTAssertNil(model.managedChildrenSession(chatID: roots[8], contextID: model.draftContextID),
                     "A ninth reader must not silently evict an unconfirmed Stop")
        let capacityMessage = try XCTUnwrap(model.managedChildReaderError)
        XCTAssertTrue(capacityMessage.contains("pending or unconfirmed requests"))
        XCTAssertTrue(capacityMessage.contains("Synthetic parent"), "Name an available parent without exposing opaque identities")
        XCTAssertFalse(capacityMessage.contains(roots[0]))
        for (index, id) in roots.prefix(8).enumerated()
        {
            let opened = await model.openChat(id); XCTAssertTrue(opened)
            XCTAssertTrue(model.managedChildrenSession(chatID: id, contextID: model.draftContextID) === readers[index],
                          "Returning to an existing root must restore its original uncertainty")
            XCTAssertNil(model.managedChildReaderError)
        }
        XCTAssertEqual(fixture.mutations.count, 8, "Navigation and cache reads cannot repeat Stop")

        let returned = await model.openChat(roots[0]); XCTAssertTrue(returned)
        fixture.stopped = []
        let recovered = readers[0]; recovered.activate(); await recovered.refresh()
        await recovered.stop(recovered.runs[0]); recovered.deactivate()
        XCTAssertEqual(fixture.mutations.count, 9, "Only explicit recovery sends another scoped Stop")
        let reopened = await model.openChat(roots[8]); XCTAssertTrue(reopened)
        XCTAssertNotNil(model.managedChildrenSession(chatID: roots[8], contextID: model.draftContextID),
                        "An acknowledged reader can release capacity without discarding unknown work")
    }

    func testListOpenBackPreservesParentDraftHistoryAndSelection() async throws
    {
        let fixture = CompanionFixture(), model = try app(fixture)
        await openParent(model); model.draft.text = "Independent parent input"
        let context = model.draftContextID, originalMessages = model.messages.map(\.content)
        let session = try await reader(model), child = try await openFirst(session)
        XCTAssertEqual(child.modelID, "OpenAI/ExactCase:V2")
        XCTAssertEqual(session.transcript?.messages.first?["content"]?.stringValue, "Stored history for " + Self.firstRun)
        XCTAssertEqual(fixture.calls.last?.1.text("transcriptChatId"), "transcript-" + Self.firstRun)
        session.showList(); await session.refresh(); session.deactivate()
        XCTAssertEqual(model.activeChatID, "root"); XCTAssertEqual(model.draftContextID, context)
        XCTAssertEqual(model.draft.text, "Independent parent input"); XCTAssertEqual(model.messages.map(\.content), originalMessages)
        XCTAssertTrue(fixture.mutations.isEmpty)
        XCTAssertFalse(fixture.calls.contains { ["openNativeSessionChat", "sendAcpPrompt", "selectSession"].contains($0.0) })
    }

    func testStopUsesOnlyCurrentRootRunScopeAndLeavesOtherChildControllable() async throws
    {
        let fixture = CompanionFixture(), model = try app(fixture)
        await openParent(model)
        let session = try await reader(model), child = try XCTUnwrap(session.runs.first)
        let scope = "scope-\(fixture.scopeNumber)"
        await session.stop(child)
        let call = try XCTUnwrap(fixture.mutations.last)
        XCTAssertEqual(call.0, "cancelManagedSubagent")
        XCTAssertEqual(Set(call.1.keys), Set(["chatId", "runId", "controlScopeId"]))
        XCTAssertEqual(call.1.text("chatId"), "root"); XCTAssertEqual(call.1.text("runId"), Self.firstRun); XCTAssertEqual(call.1.text("controlScopeId"), scope)
        XCTAssertEqual(session.stops[session.key(child)], .accepted, "Raw ok/runId receipt must survive the actual adapter")
        await session.refresh()
        XCTAssertFalse(session.canStop(session.runs[0])); XCTAssertTrue(session.canStop(session.runs[1]))
        XCTAssertEqual(fixture.stopped, [Self.firstRun]); XCTAssertEqual(model.activeChatID, "root")
    }

    func testAcceptedStopDoesNotDisableStopForNewFollowupTurn() async throws
    {
        let fixture = CompanionFixture(), model = try app(fixture)
        await openParent(model)
        let session = try await reader(model), child = try await openFirst(session)
        await session.stop(child)
        XCTAssertEqual(session.stops[session.key(child)], .accepted)
        fixture.stopped.remove(Self.firstRun)
        await session.refresh()
        let settled = try XCTUnwrap(session.selected)
        let draft = try XCTUnwrap(session.beginFollowup(settled)); draft.edit("Next owned child turn")
        await session.sendFollowup(settled)
        await session.refresh()
        let next = try XCTUnwrap(session.selected)
        XCTAssertEqual(next.followup?.currentTurnID, Self.nextTurn)
        XCTAssertTrue(next.canCancel)
        XCTAssertTrue(session.canStop(next), "An acknowledged Stop cannot permanently disable a new current child turn")
        await session.stop(next)
        XCTAssertEqual(fixture.calls.filter { $0.0 == "cancelManagedSubagent" }.count, 2)
    }

    func testReturnedHintsCannotBypassAdvertisedActionCapabilities() async throws
    {
        let fixture = CompanionFixture(), model = try app(fixture)
        await openParent(model)
        let session = try await reader(model), child = try await openFirst(session)
        XCTAssertTrue(session.canFollowup(child))
        model.capabilities = ["actions": ["listManagedSubagents", "getManagedAgentTranscript"]]
        XCTAssertFalse(session.canFollowup(child)); XCTAssertFalse(session.canStop(child))
        XCTAssertNil(session.beginFollowup(child)); await session.sendFollowup(child); await session.stop(child)
        XCTAssertTrue(fixture.mutations.isEmpty)
        model.capabilities = ["actions": []]
        XCTAssertNil(model.managedChildrenSession(chatID: "root", contextID: model.draftContextID))
    }

    func testLostFollowupReplyReadbackAndSameRequestRetryPreserveNewerDraft() async throws
    {
        let fixture = CompanionFixture(), model = try app(fixture)
        await openParent(model); model.draft.text = "Parent remains independent"
        let session = try await reader(model), child = try await openFirst(session)
        let draft = try XCTUnwrap(session.beginFollowup(child)); draft.edit("Original child follow-up")
        fixture.loseNextFollowupReply = true; await session.sendFollowup(child)
        let original = try XCTUnwrap(draft.request)
        XCTAssertEqual(draft.phase, .uncertain); draft.edit("Newer child draft")
        session.deactivate(); model.closeChat(); await openParent(model)
        let reopened = try await reader(model)
        XCTAssertTrue(reopened === session)
        let current = try XCTUnwrap(reopened.selected)
        XCTAssertFalse(current.followup!.canSend, "Readback has advanced to the busy accepted turn")
        XCTAssertEqual(current.followup?.currentTurnID, Self.nextTurn)
        XCTAssertEqual(draft.request, original); XCTAssertEqual(fixture.mutations.count, 1, "Reads and reopening cannot resend input")
        XCTAssertTrue(reopened.canFollowup(current, retry: true))
        await reopened.sendFollowup(current, retry: true)
        XCTAssertEqual(fixture.mutations.count, 2)
        let first = fixture.mutations[0].1, retry = fixture.mutations[1].1
        for field in ["chatId", "runId", "afterTurnId", "clientRequestId", "text"] { XCTAssertEqual(first.text(field), retry.text(field), field) }
        XCTAssertNotEqual(first.text("controlScopeId"), retry.text("controlScopeId"), "Explicit retry takes only the refreshed scope")
        XCTAssertEqual(draft.phase, .idle); XCTAssertNil(draft.request); XCTAssertEqual(draft.text, "Newer child draft")
        XCTAssertEqual(model.draft.text, "Parent remains independent")
    }

    func testChildDraftsRemainIndependentAcrossParentAndOtherRootNavigation() async throws
    {
        let fixture = CompanionFixture(), model = try app(fixture)
        await openParent(model); model.draft.text = "Root draft"
        let session = try await reader(model), first = try await openFirst(session)
        let firstDraft = try XCTUnwrap(session.beginFollowup(first)); firstDraft.edit("First child draft")
        session.showList(); await session.refresh(); session.select(session.runs[1]); await session.refresh()
        let second = try XCTUnwrap(session.selected), secondDraft = try XCTUnwrap(session.beginFollowup(second)); secondDraft.edit("Second child draft")
        session.deactivate(); _ = await model.openChat("other"); model.draft.text = "Other root draft"
        await openParent(model)
        let same = try await reader(model)
        XCTAssertTrue(same === session); XCTAssertEqual(model.draft.text, "Root draft")
        XCTAssertEqual(firstDraft.text, "First child draft"); XCTAssertEqual(secondDraft.text, "Second child draft")
        same.showList(); await same.refresh(); same.select(same.runs[0]); await same.refresh()
        XCTAssertTrue(same.draft(try XCTUnwrap(same.selected)) === firstDraft)
        XCTAssertTrue(fixture.mutations.isEmpty)
    }

    func testConnectionGenerationRejectsHeldListReplyAndKeepsNewContext() async throws
    {
        let fixture = CompanionFixture(), model = try app(fixture)
        await openParent(model)
        let original = try XCTUnwrap(model.managedChildrenSession(chatID: "root", contextID: model.draftContextID)); original.activate()
        fixture.heldAction = "listManagedSubagents"
        let pending = Task { await original.refresh() }; try await waitForHeld(fixture)
        model.switchConnection("synthetic-second"); model.connected = true
        model.capabilities = ["actions": CompanionFixture.actions]; model.chats = [fixture.root()]
        await openParent(model); model.draft.text = "Second connection draft"
        fixture.heldAction = nil; fixture.release(); await pending.value
        XCTAssertTrue(original.runs.isEmpty); XCTAssertTrue(original.displayLastReported)
        XCTAssertEqual(model.activeConnectionID, "synthetic-second"); XCTAssertEqual(model.draft.text, "Second connection draft")
        let fresh = try await reader(model)
        XCTAssertFalse(fresh === original); XCTAssertEqual(fresh.runs.count, 2)
        XCTAssertTrue(fixture.mutations.isEmpty)
    }

    func testReadOnlyAndHistoricalBoundariesKeepHistoryWithoutControlAuthority() async throws
    {
        for boundary in ["chat", "connection", "history"]
        {
            let fixture = CompanionFixture(), model = try app(fixture, readOnly: boundary == "chat")
            if boundary == "connection" { model.capabilities["readOnly"] = true }
            if boundary == "history" { fixture.historical = true }
            await openParent(model)
            let session = try await reader(model), child = try await openFirst(session)
            XCTAssertNotNil(session.transcript, boundary)
            XCTAssertFalse(session.canStop(child), boundary); XCTAssertFalse(session.canFollowup(child), boundary)
            await session.stop(child); await session.sendFollowup(child)
            XCTAssertTrue(fixture.mutations.isEmpty, boundary)
        }
    }

    func testFailedOrForeignReadRetainsAcceptedHistoryAndRevokesControls() async throws
    {
        let fixture = CompanionFixture(), model = try app(fixture)
        await openParent(model)
        let session = try await reader(model), child = try await openFirst(session)
        let content = session.transcript?.messages
        fixture.foreignTranscript = true; await session.refresh()
        XCTAssertEqual(session.transcript?.messages, content); XCTAssertTrue(session.stale)
        XCTAssertFalse(session.canFollowup(child)); XCTAssertFalse(session.canStop(child))
        fixture.foreignTranscript = false; await session.refresh()
        XCTAssertTrue(session.canFollowup(try XCTUnwrap(session.selected)))
        fixture.failReads = true; await session.refresh()
        XCTAssertEqual(session.transcript?.messages, content); XCTAssertNotNil(session.error)
        XCTAssertFalse(session.canFollowup(try XCTUnwrap(session.selected)))
        XCTAssertTrue(fixture.mutations.isEmpty)
    }

    func testLateFollowupReceiptCannotSettleAnotherRootsDraft() async throws
    {
        let fixture = CompanionFixture(), model = try app(fixture)
        await openParent(model)
        let session = try await reader(model), child = try await openFirst(session)
        let draft = try XCTUnwrap(session.beginFollowup(child)); draft.edit("Original owned follow-up")
        fixture.heldAction = "submitManagedSubagentFollowup"
        let pending = Task { await session.sendFollowup(child) }; try await waitForHeld(fixture)
        _ = await model.openChat("other"); model.draft.text = "Unrelated current draft"
        fixture.heldAction = nil; fixture.release(); await pending.value
        XCTAssertEqual(model.activeChatID, "other"); XCTAssertEqual(model.draft.text, "Unrelated current draft")
        XCTAssertEqual(draft.phase, .uncertain); XCTAssertEqual(draft.request?.text, "Original owned follow-up")
        XCTAssertEqual(fixture.mutations.count, 1)
    }
}
