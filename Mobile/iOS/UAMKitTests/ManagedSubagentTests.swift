import Foundation
import XCTest
@testable import UAMKit

private let runID = "11111111-1111-4111-8111-111111111111"
private let otherRunID = "22222222-2222-4222-8222-222222222222"
private let turnID = "33333333-3333-4333-8333-333333333333"

private func managedRunFixture(_ id: String = runID, lastReported: Bool = false) -> JSONValue
{
    ["runId": .string(id), "transcriptChatId": .string("transcript-" + id), "providerId": "codex-cli", "modelId": "OpenAI/ExactCase:V2",
     "taskTitle": "Review the parser", "summary": "Owned task", "status": "completed", "canCancel": true, "lastReported": .bool(lastReported),
     "followup": ["canSend": true, "code": "none", "currentTurnId": .string(turnID), "currentTurnNumber": 1]]
}
private func page(_ rows: [JSONValue] = [managedRunFixture()], offset: Int = 0, total: Int? = nil, scope: String = "scope") -> JSONValue
{
    ["chatId": "root", "controlScopeId": .string(scope), "runs": .array(rows), "offset": .integer(Int64(offset)),
     "nextOffset": .null, "totalCount": .integer(Int64(total ?? (offset + rows.count)))]
}
private func transcript(_ id: String = runID, lastReported: Bool = false) -> JSONValue
{
    ["runId": .string(id), "providerId": "codex-cli", "title": "Child history", "lastReported": .bool(lastReported),
     "messages": [["role": "assistant", "content": "Owned stored history", "createdAt": "2026-10-23T10:00:00Z"]]]
}
private func accepted(_ payload: JSONValue) -> JSONValue
{
    ["followup": ["accepted": true, "replayed": false, "code": "none", "acceptance": ["rootChatId": "root", "runId": payload["runId"]!,
        "transcriptChatId": .string("transcript-" + payload.string("runId")!), "clientRequestId": payload["clientRequestId"]!,
        "turnId": "44444444-4444-4444-8444-444444444444", "turnNumber": 2, "statusAtAdmission": "queued"]]]
}

final class ManagedSubagentTests: XCTestCase
{
    func testListingRejectsForeignDuplicateAndNoncontiguousPages() throws
    {
        XCTAssertEqual(try ManagedChildPage(json: page(), rootID: "root", offset: 0).runs[0].modelID, "OpenAI/ExactCase:V2")
        XCTAssertThrowsError(try ManagedChildPage(json: page(), rootID: "foreign", offset: 0))
        XCTAssertThrowsError(try ManagedChildPage(json: page([managedRunFixture(), managedRunFixture()]), rootID: "root", offset: 0))
        XCTAssertThrowsError(try ManagedChildPage(json: page([managedRunFixture(otherRunID)]), rootID: "root", offset: 0, runID: runID))
        var invalid = page(total: 5); invalid["nextOffset"] = 3
        XCTAssertThrowsError(try ManagedChildPage(json: invalid, rootID: "root", offset: 0))
        invalid = page(); invalid["controlScopeId"] = "bad\0scope"
        XCTAssertThrowsError(try ManagedChildPage(json: invalid, rootID: "root", offset: 0))
    }

    func testLastReportedAndMalformedHintsCannotAuthorizeInput() throws
    {
        let stored = try ManagedChildPage(json: page([managedRunFixture(lastReported: true)]), rootID: "root", offset: 0).runs[0]
        XCTAssertFalse(stored.canCancel); XCTAssertNil(stored.followup); XCTAssertFalse(stored.active)
        var row = managedRunFixture(); row["followup"] = ["canSend": true, "code": "none", "currentTurnId": "foreign", "currentTurnNumber": 1]
        XCTAssertNil(try ManagedChildPage(json: page([row]), rootID: "root", offset: 0).runs[0].followup)
        let live = try ManagedChildPage(json: page(), rootID: "root", offset: 0).runs[0]
        XCTAssertThrowsError(try ManagedChildTranscript(json: transcript(otherRunID), run: live))
        var foreign = transcript(); foreign["providerId"] = "claude-cli"
        XCTAssertThrowsError(try ManagedChildTranscript(json: foreign, run: live))
    }

    func testFollowupReceiptBindsExactIdentityAndTurn() throws
    {
        let live = try ManagedChildPage(json: page(), rootID: "root", offset: 0).runs[0]
        let key = ManagedChildDraftKey(connectionID: "mac", rootID: "root", runID: runID, transcriptID: live.transcriptID)
        let request = try ManagedFollowupRequest(key: key, state: live.followup!, text: "Keep ExactCase\n", draftRevision: UUID())
        let payload = try request.payload(controlScopeID: "scope")
        XCTAssertEqual(Set(payload.objectValue!.keys), Set(["chatId", "runId", "controlScopeId", "afterTurnId", "clientRequestId", "text"]))
        XCTAssertEqual(payload.string("text"), "Keep ExactCase\n")
        XCTAssertEqual(request.outcome(accepted(payload)), .accepted)
        for field in ["rootChatId", "runId", "transcriptChatId", "clientRequestId", "turnId"]
        {
            var reply = accepted(payload), body = reply["followup"]!, receipt = body["acceptance"]!
            receipt[field] = "foreign"; body["acceptance"] = receipt; reply["followup"] = body
            XCTAssertNil(request.outcome(reply), field)
        }
        XCTAssertNil(request.outcome(["followup": ["accepted": false, "replayed": false, "code": "delivery_unknown", "acceptance": .null]]))
        XCTAssertFalse(ManagedFollowupRequest.validText(String(repeating: "é", count: 16385)))
        XCTAssertFalse(ManagedFollowupRequest.validText("  \n")); XCTAssertFalse(ManagedFollowupRequest.validText("text\0"))
        XCTAssertEqual(try CompanionProtocol.unwrap(["ok": true, "runId": .string(runID)]), ["ok": true, "runId": .string(runID)])
    }

    func testHistoricalOrStaleTimingNeverAdvances() throws
    {
        var row = managedRunFixture(); row["status"] = "running"; row["startedAtEpochMs"] = 100000
        let live = try ManagedChildPage(json: page([row]), rootID: "root", offset: 0).runs[0]
        XCTAssertEqual(live.elapsed(at: Date(timeIntervalSince1970: 110), stale: false), "10s")
        XCTAssertEqual(live.elapsed(at: Date(timeIntervalSince1970: 120), stale: true), "Timing unavailable until refresh")
        row["lastReported"] = true
        let stored = try ManagedChildPage(json: page([row]), rootID: "root", offset: 0).runs[0]
        XCTAssertEqual(stored.elapsed(at: Date(timeIntervalSince1970: 130), stale: false), "Timing unavailable until refresh")
        row["finishedAtEpochMs"] = 108000
        let finished = try ManagedChildPage(json: page([row]), rootID: "root", offset: 0).runs[0]
        XCTAssertEqual(finished.elapsed(at: Date(timeIntervalSince1970: 140), stale: true), "8s")
    }
}

@MainActor final class ManagedChildSessionTests: XCTestCase
{
    @MainActor private final class Fixture
    {
        var requests: [(String, JSONValue)] = []
        var rows = [managedRunFixture(), managedRunFixture(otherRunID)]
        var readFails = false
        var submitUnknown = false
        var transcriptLastReported = false
        var heldAction: String?
        var held: CheckedContinuation<JSONValue, any Error>?
        var supports = true
        var originCurrent = true
        var writable = true
        func request(_ action: String, _ payload: JSONValue) async throws -> JSONValue
        {
            requests.append((action, payload))
            if heldAction == action { return try await withCheckedThrowingContinuation { held = $0 } }
            if action == "listManagedSubagents"
            {
                if readFails { throw CompanionError.http(status: 409) }
                return page(rows.filter { payload.string("runId") == nil || $0.string("runId") == payload.string("runId") })
            }
            if action == "getManagedAgentTranscript" { return transcript(payload.string("transcriptChatId")!.replacingOccurrences(of: "transcript-", with: ""), lastReported: transcriptLastReported) }
            if action == "submitManagedSubagentFollowup"
            {
                if submitUnknown { throw CompanionError.network(code: -1001) }
                return accepted(payload)
            }
            return ["ok": true, "runId": payload["runId"]!]
        }
        func session(store: ManagedChildDraftStore = ManagedChildDraftStore(), connection: String = "mac") -> ManagedChildSession
        {
            ManagedChildSession(connectionID: connection, rootID: "root", drafts: store, request: request,
                                supports: { _ in self.supports }, originCurrent: { self.originCurrent }, writable: { self.writable })
        }
    }
    private func opened(_ fixture: Fixture, store: ManagedChildDraftStore = ManagedChildDraftStore()) async throws -> ManagedChildSession
    {
        let session = fixture.session(store: store); session.activate(); await session.refresh()
        session.select(try XCTUnwrap(session.runs.first)); await session.refresh()
        return session
    }
    private func waitForHeld(_ fixture: Fixture) async
    {
        for _ in 0..<1000 { if fixture.held != nil { return }; await Task.yield() }
        XCTFail("Expected a held request")
    }

    func testOpenUsesExactRunListingAndStoredReadWithoutRuntimeSelection() async throws
    {
        let fixture = Fixture(), session = try await opened(fixture)
        XCTAssertEqual(fixture.requests.map(\.0), ["listManagedSubagents", "listManagedSubagents", "getManagedAgentTranscript"])
        XCTAssertEqual(fixture.requests[1].1.string("runId"), runID)
        XCTAssertEqual(fixture.requests[2].1, ["chatId": "root", "transcriptChatId": .string("transcript-" + runID)])
        XCTAssertNotNil(session.transcript); XCTAssertTrue(session.canFollowup(session.selected!))
        session.showList(); await session.refresh()
        XCTAssertNil(session.selected)
        XCTAssertFalse(fixture.requests.contains { ["sendAcpPrompt", "openNativeSessionChat", "setSelectedChat"].contains($0.0) })
    }

    func testScopedStopAndReadFailureRetainRowsWithoutAuthority() async throws
    {
        let fixture = Fixture(), session = fixture.session(); session.activate(); await session.refresh()
        let selected = try XCTUnwrap(session.runs.first)
        await session.stop(selected)
        XCTAssertEqual(fixture.requests.last?.0, "cancelManagedSubagent")
        XCTAssertEqual(fixture.requests.last?.1, ["chatId": "root", "runId": .string(runID), "controlScopeId": "scope"])
        XCTAssertEqual(session.stops[session.key(selected)], .accepted)
        fixture.readFails = true; await session.refresh()
        XCTAssertEqual(session.runs.count, 2); XCTAssertTrue(session.stale); XCTAssertFalse(session.canStop(selected))
        let count = fixture.requests.count; await session.stop(selected)
        XCTAssertEqual(fixture.requests.count, count)
    }

    func testReadOnlyLastReportedAndCapabilitiesRefuseControls() async throws
    {
        let fixture = Fixture(), session = try await opened(fixture), child = try XCTUnwrap(session.selected)
        fixture.writable = false
        XCTAssertFalse(session.canStop(child)); XCTAssertFalse(session.canFollowup(child))
        fixture.writable = true; fixture.transcriptLastReported = true; await session.refresh()
        XCTAssertNotNil(session.transcript); XCTAssertFalse(session.canStop(session.selected!)); XCTAssertFalse(session.canFollowup(session.selected!))
        fixture.supports = false
        let before = fixture.requests.count; await session.stop(child); await session.sendFollowup(child)
        XCTAssertEqual(fixture.requests.count, before)
    }

    func testRetiredReadCannotPublishIntoReopenedSession() async throws
    {
        let fixture = Fixture(), session = fixture.session(); session.activate(); fixture.heldAction = "listManagedSubagents"
        let pending = Task { await session.refresh() }; await waitForHeld(fixture)
        session.deactivate(); session.activate()
        await session.refresh()
        XCTAssertEqual(fixture.requests.count, 1, "Retiring a view cannot release a physical read slot")
        fixture.held?.resume(returning: page()); await pending.value
        XCTAssertTrue(session.runs.isEmpty); XCTAssertTrue(session.stale)
        fixture.heldAction = nil; await session.refresh(); XCTAssertEqual(session.runs.count, 2)
    }

    func testRetiredPresentationCannotDeactivateNewReader() async throws
    {
        let fixture = Fixture(), session = fixture.session()
        let previous = session.activate(), current = session.activate()
        session.deactivate(previous); await session.refresh()
        XCTAssertEqual(session.runs.count, 2)
        XCTAssertFalse(session.displayLastReported)
        session.deactivate(current)
        let count = fixture.requests.count; await session.refresh()
        XCTAssertEqual(fixture.requests.count, count); XCTAssertTrue(session.displayLastReported)
    }

    func testUncertainFollowupRetainsOriginalEnvelopeAndIndependentEditedDraft() async throws
    {
        let fixture = Fixture(), session = try await opened(fixture), child = try XCTUnwrap(session.selected)
        let entry = try XCTUnwrap(session.beginFollowup(child)); entry.edit("Original work")
        fixture.submitUnknown = true; await session.sendFollowup(child)
        let original = try XCTUnwrap(entry.request)
        XCTAssertEqual(entry.phase, .uncertain); entry.edit("New independent edits")
        session.deactivate(); session.activate(); await session.refresh()
        XCTAssertEqual(entry.request, original); XCTAssertEqual(entry.text, "New independent edits")
        XCTAssertEqual(fixture.requests.filter { $0.0 == "submitManagedSubagentFollowup" }.count, 1, "Reopening must not retry input")
        fixture.submitUnknown = false; await session.sendFollowup(session.selected!, retry: true)
        let sends = fixture.requests.filter { $0.0 == "submitManagedSubagentFollowup" }
        XCTAssertEqual(sends.count, 2); XCTAssertEqual(sends[0].1, sends[1].1)
        XCTAssertEqual(entry.phase, .idle); XCTAssertNil(entry.request); XCTAssertEqual(entry.text, "New independent edits")
    }

    func testLateFollowupReceiptRetainsOriginalRequestAndCannotTouchOtherChild() async throws
    {
        let fixture = Fixture(), session = try await opened(fixture), child = try XCTUnwrap(session.selected)
        let entry = try XCTUnwrap(session.beginFollowup(child)); entry.edit("Original")
        fixture.heldAction = "submitManagedSubagentFollowup"
        let pending = Task { await session.sendFollowup(child) }; await waitForHeld(fixture)
        let payload = fixture.requests.last!.1
        session.showList(); session.select(session.runs[1])
        fixture.held?.resume(returning: accepted(payload)); await pending.value
        XCTAssertEqual(session.selected?.id, otherRunID); XCTAssertEqual(entry.phase, .uncertain)
        XCTAssertEqual(entry.request?.text, "Original"); XCTAssertNil(session.draft(session.selected!))
    }

    func testDraftCapacityNeverEvictsInputOrUnknownRequests() async throws
    {
        let store = ManagedChildDraftStore(capacity: 1), first = ManagedChildDraftKey(connectionID: "a", rootID: "r", runID: "run", transcriptID: "t")
        let entry = try XCTUnwrap(store.acquire(first)); entry.edit("Keep this input")
        XCTAssertNil(store.acquire(.init(connectionID: "b", rootID: "r", runID: "run", transcriptID: "t")))
        XCTAssertTrue(store.existing(first) === entry)
        entry.edit(""); XCTAssertNotNil(store.acquire(.init(connectionID: "b", rootID: "r", runID: "run", transcriptID: "t")))
    }

    func testAcceptedEmptyDraftCanBeRecycledWhileValidationFeedbackStaysProtected() async throws
    {
        let store = ManagedChildDraftStore(capacity: 1), fixture = Fixture()
        let session = try await opened(fixture, store: store), child = try XCTUnwrap(session.selected)
        let entry = try XCTUnwrap(session.beginFollowup(child)); entry.edit("Owned follow-up")
        await session.sendFollowup(child)
        XCTAssertEqual(entry.phase, .idle); XCTAssertNil(entry.request); XCTAssertEqual(entry.text, "")
        XCTAssertFalse(entry.feedback.isEmpty, "Acceptance remains visible until this empty entry is reused")
        let another = ManagedChildDraftKey(connectionID: "mac", rootID: "other", runID: otherRunID, transcriptID: "other-transcript")
        XCTAssertNotNil(store.acquire(another), "A confirmed empty draft must not permanently consume protected capacity")

        let validationStore = ManagedChildDraftStore(capacity: 1)
        let invalid = try XCTUnwrap(validationStore.acquire(session.key(child))); invalid.edit("\0")
        XCTAssertEqual(invalid.text, ""); XCTAssertFalse(invalid.feedback.isEmpty)
        XCTAssertNil(validationStore.acquire(another), "Actionable validation feedback must remain protected even without text")
    }

    func testChangedCapabilityRejectsCapturedRunAndUncertainStopRequiresExplicitRetry() async throws
    {
        let fixture = Fixture(), session = fixture.session(); session.activate(); await session.refresh()
        let captured = try XCTUnwrap(session.runs.first)
        fixture.rows[0]["canCancel"] = false; await session.refresh()
        XCTAssertFalse(session.canStop(captured), "An old row cannot supply a current capability")
        fixture.rows[0]["canCancel"] = true; await session.refresh()
        fixture.heldAction = "cancelManagedSubagent"
        let pending = Task { await session.stop(session.runs[0]) }; await waitForHeld(fixture)
        session.deactivate(); session.activate()
        fixture.held?.resume(returning: ["ok": true, "runId": .string(runID)]); await pending.value
        XCTAssertEqual(session.stops[session.key(captured)], .uncertain)
        fixture.heldAction = nil; await session.refresh()
        XCTAssertEqual(fixture.requests.filter { $0.0 == "cancelManagedSubagent" }.count, 1)
        await session.stop(session.runs[0])
        XCTAssertEqual(fixture.requests.filter { $0.0 == "cancelManagedSubagent" }.count, 2)
        XCTAssertEqual(session.stops[session.key(captured)], .accepted)
    }

    func testUncertainStopProtectsInactiveReaderUntilExplicitRecovery() async throws
    {
        let fixture = Fixture(), session = fixture.session()
        session.activate(); await session.refresh()
        let child = try XCTUnwrap(session.runs.first)
        fixture.heldAction = "cancelManagedSubagent"
        let pending = Task { await session.stop(child) }; await waitForHeld(fixture)
        XCTAssertFalse(session.canRetire, "An outstanding Stop transport owns this reader")
        session.deactivate()
        fixture.held?.resume(returning: ["ok": true, "runId": .string(child.id)])
        await pending.value
        XCTAssertEqual(session.stops[session.key(child)], .uncertain)
        XCTAssertFalse(session.canRetire, "Evicting this inactive reader would discard the unconfirmed Stop")

        fixture.heldAction = nil; session.activate(); await session.refresh()
        XCTAssertEqual(fixture.requests.filter { $0.0 == "cancelManagedSubagent" }.count, 1, "Reading cannot replay Stop")
        await session.stop(session.runs[0]); session.deactivate()
        XCTAssertEqual(session.stops[session.key(child)], .accepted)
        XCTAssertTrue(session.canRetire, "Explicitly acknowledged work no longer protects reader capacity")
    }

    func testEditsWhilePendingAreNotClearedByAcceptance() async throws
    {
        let fixture = Fixture(), session = try await opened(fixture), child = try XCTUnwrap(session.selected)
        let entry = try XCTUnwrap(session.beginFollowup(child)); entry.edit("Submitted")
        fixture.heldAction = "submitManagedSubagentFollowup"
        let pending = Task { await session.sendFollowup(child) }; await waitForHeld(fixture)
        let sent = fixture.requests.last!.1
        entry.edit("Edited while pending")
        fixture.held?.resume(returning: accepted(sent)); await pending.value
        XCTAssertEqual(entry.text, "Edited while pending"); XCTAssertEqual(entry.phase, .idle); XCTAssertNil(entry.request)
    }

    func testChildAndConnectionDraftsStayIndependentAcrossParentReturn() async throws
    {
        let fixture = Fixture(), store = ManagedChildDraftStore(), session = try await opened(fixture, store: store)
        let first = try XCTUnwrap(session.selected), firstDraft = try XCTUnwrap(session.beginFollowup(first)); firstDraft.edit("First child")
        session.showList(); session.select(session.runs[1]); await session.refresh()
        let second = try XCTUnwrap(session.selected), secondDraft = try XCTUnwrap(session.beginFollowup(second)); secondDraft.edit("Second child")
        session.deactivate(); session.activate(); session.showList(); await session.refresh(); session.select(session.runs[0]); await session.refresh()
        XCTAssertTrue(session.draft(session.selected!) === firstDraft)
        XCTAssertEqual(firstDraft.text, "First child"); XCTAssertEqual(secondDraft.text, "Second child")
        let foreign = fixture.session(store: store, connection: "other-mac"); foreign.activate(); await foreign.refresh(); foreign.select(foreign.runs[0]); await foreign.refresh()
        XCTAssertNil(foreign.draft(foreign.selected!))
        XCTAssertEqual(firstDraft.text, "First child")
    }
}
