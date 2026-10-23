import XCTest
@testable import UAM

final class ActiveParityTests: XCTestCase
{
    private let now = Date(timeIntervalSince1970: 1_792_324_800) // 2026-10-18 12:00 UTC
    private func chat(_ id: String, _ fields: WireObject = [:]) -> MobileChat
    {
        var raw: WireObject = [
            "id": id, "createdAt": "2026-10-01T12:00:00Z",
            "updatedAt": "2026-10-18T12:00:00Z", "interactionAt": "2026-10-01T12:00:00Z",
            "attentionRevision": ""
        ]
        raw.merge(fields) { _, new in new }
        return MobileChat(raw: raw)
    }
    private func groups(_ chats: [MobileChat]) -> [MobileActivityGroup]
    { MobileActivityGroup.build(chats, now: now) }

    func testSettledUnreadReplyIsExcludedWithoutAcknowledgingIt()
    {
        let chat = MobileChat(raw: [
            "id": "settled", "interactionAt": "2026-10-17T12:00:00Z",
            "updatedAt": "2026-10-17T12:00:00Z", "settledAt": "2026-10-17T12:01:00Z",
            "attentionRevision": "unacknowledged-reply",
            "acpSession": ["readySinceLastSelect": true]
        ])
        XCTAssertTrue(chat.unseen)
        XCTAssertTrue(MobileActivityGroup.build([chat]).isEmpty)
        XCTAssertTrue(chat.unseen)
    }

    func testOnlyTwoWorkingFamiliesRemainAmongSettledUnreadChats()
    {
        let quiet = (0..<20).map { chat("settled-\($0)", [
            "settledAt": "2026-10-18T12:00:00Z", "attentionRevision": "unread-\($0)",
            "acpSession": ["readySinceLastSelect": true]
        ]) }
        let working = chat("home-assistance", ["acpSession": ["processing": true]])
        let question = chat("social-workflow", ["acpSession": ["attentionKind": "question"]])
        XCTAssertEqual(Set(groups(quiet + [working, question]).map(\.id)), ["home-assistance", "social-workflow"])
        XCTAssertTrue(quiet.allSatisfy(\.unseen))
    }

    func testRecentInteractionExpiresAndSettlementMustPrecedeNewActivity()
    {
        let recent = chat("recent", ["interactionAt": "2026-10-17T12:00:00Z"])
        let settled = chat("settled", ["interactionAt": "2026-10-17T12:00:00Z", "settledAt": "2026-10-17T12:00:00Z"])
        let newActivity = chat("new", ["interactionAt": "2026-10-17T12:00:01Z", "settledAt": "2026-10-17T12:00:00Z"])
        let boundary = chat("boundary", ["interactionAt": "2026-10-15T12:00:00Z"])
        let expired = chat("expired", ["interactionAt": "2026-10-15T11:59:59Z"])
        XCTAssertEqual(Set(groups([recent, settled, newActivity, boundary, expired]).map(\.id)), ["recent", "new", "boundary"])
        XCTAssertTrue(recent.isActive(at: now))
        XCTAssertFalse(settled.isActive(at: now))
    }

    func testOldUnacknowledgedCompletionSurvivesWithoutSettlement()
    {
        let unread = chat("unread", ["attentionRevision": "old-reply", "acpSession": ["readySinceLastSelect": true]])
        let settled = chat("settled", ["attentionRevision": "old-reply", "settledAt": "2026-10-02T12:00:00Z", "acpSession": ["readySinceLastSelect": true]])
        XCTAssertEqual(groups([unread, settled]).map(\.id), ["unread"])
        XCTAssertTrue(unread.unseen)
        XCTAssertTrue(settled.unseen)
    }

    func testAttentionIdentityDoesNotInventRuntimeCompletion()
    {
        let revisionOnly = chat("revision-only", ["attentionRevision": "unread", "acpSession": ["readySinceLastSelect": false]])
        let acknowledged = chat("acknowledged", ["attentionRevision": "", "acpSession": ["readySinceLastSelect": true]])
        let cliReady = chat("cli-ready", ["attentionRevision": "reply", "cliTerminal": ["readySinceLastSelect": true]])
        XCTAssertTrue(revisionOnly.unseen)
        XCTAssertFalse(acknowledged.unseen)
        XCTAssertEqual(groups([revisionOnly, acknowledged, cliReady]).map(\.id), ["cli-ready"])
    }

    func testMissingLegacyInteractionFallsBackButExplicitEmptyHistoryDoesNot()
    {
        let legacy = MobileChat(raw: ["id": "legacy", "updatedAt": "2026-10-18T12:00:00Z", "attentionRevision": ""])
        let imported = chat("imported", ["interactionAt": ""])
        let invalid = chat("invalid", ["interactionAt": "invalid"])
        XCTAssertEqual(groups([legacy, imported, invalid]).map(\.id), ["legacy"])
        XCTAssertNil(imported.interactionAt)
        XCTAssertEqual(invalid.interactionAt, parseWireDate("2026-10-01T12:00:00Z"))
    }

    func testWorkingAndInputOverrideSettlementUsingRealRuntimeEvidence()
    {
        let runtimes: [WireObject] = [
            ["acpSession": ["processing": true]],
            ["acpSession": ["backgroundTaskCount": 1]],
            ["acpSession": ["lifecycleState": "starting"]],
            ["acpSession": ["lifecycleState": "waitingPermission"]],
            ["acpSession": ["attentionKind": "question"]],
            ["acpSession": ["pendingPermission": ["requestId": "permission"]]],
            ["acpSession": ["pendingUserInput": ["requestId": "question"]]],
            ["cliTerminal": ["processing": true]],
            ["cliTerminal": ["lifecycleState": "starting"]],
            ["cliTerminal": ["lifecycleState": "busy"]],
            ["cliTerminal": ["lifecycleState": "shuttingDown"]]
        ]
        for (index, runtime) in runtimes.enumerated()
        {
            var fields = runtime
            fields["settledAt"] = "2026-10-18T12:00:00Z"
            XCTAssertEqual(groups([chat("runtime-\(index)", fields)]).count, 1, "runtime \(index)")
        }
        XCTAssertTrue(groups([chat("connected", ["acpSession": ["running": true, "lifecycleState": "ready"]])]).isEmpty)
        XCTAssertTrue(groups([chat("error", ["acpSession": ["attentionKind": "error"]])]).isEmpty)
    }

    func testRootSettlementUsesLatestFamilyInteraction()
    {
        let root = chat("root", ["settledAt": "2026-10-17T12:00:00Z"])
        let oldChild = chat("child", ["branchRootChatId": "root", "interactionAt": "2026-10-17T11:00:00Z", "attentionRevision": "unread"])
        XCTAssertTrue(groups([root, oldChild]).isEmpty)
        let newChild = chat("child", ["branchRootChatId": "root", "interactionAt": "2026-10-17T13:00:00Z", "settledAt": "2026-10-18T12:00:00Z"])
        let family = groups([root, newChild])
        XCTAssertEqual(family.map(\.id), ["root"])
        XCTAssertEqual(family.first?.destination.id, "child")
    }

    func testRootSnoozeParksWorkingFamilyButInputRaisesIt()
    {
        let future = now.addingTimeInterval(3600).timeIntervalSince1970 * 1000
        let root = chat("root", ["snoozedUntilMs": future])
        let working = chat("child", ["branchRootChatId": "root", "acpSession": ["processing": true]])
        XCTAssertTrue(groups([root, working]).isEmpty)
        let question = chat("question", ["branchRootChatId": "root", "acpSession": ["attentionKind": "question"]])
        let raised = groups([root, working, question])
        XCTAssertEqual(raised.first?.root.id, "root")
        XCTAssertEqual(raised.first?.destination.id, "question")
        XCTAssertEqual(raised.first?.members.count, 3)
        let error = chat("error", ["branchRootChatId": "root", "acpSession": ["attentionKind": "error"]])
        XCTAssertTrue(groups([root, error]).isEmpty)
        let staleInput = chat("stale", ["branchRootChatId": "root", "acpSession": ["attentionKind": "error", "pendingUserInput": ["requestId": "old"]]])
        XCTAssertTrue(groups([root, staleInput]).isEmpty)
    }

    func testExpiredSnoozeReactivatesUntilSettledAfterWake()
    {
        let wake = now.addingTimeInterval(-3600).timeIntervalSince1970 * 1000
        let awake = chat("awake", ["snoozedUntilMs": wake, "settledAt": "2026-10-02T12:00:00Z"])
        let settled = chat("settled", ["snoozedUntilMs": wake, "settledAt": "2026-10-18T12:00:00Z"])
        let expired = chat("expired", ["snoozedUntilMs": now.addingTimeInterval(-4 * 86400).timeIntervalSince1970 * 1000])
        XCTAssertEqual(groups([awake, settled, expired]).map(\.id), ["awake"])
    }

    func testInteractionOrderIgnoresStreamUpdatesAndIsStableForTies()
    {
        let earlier = chat("earlier", ["interactionAt": "2026-10-17T12:00:00Z", "updatedAt": "2026-10-19T12:00:00Z"])
        let later = chat("later", ["interactionAt": "2026-10-17T13:00:00Z"])
        XCTAssertEqual(groups([earlier, later]).map(\.id), ["later", "earlier"])
        let tiedA = chat("a", ["interactionAt": "2026-10-17T13:00:00Z"])
        let tiedB = chat("b", ["interactionAt": "2026-10-17T13:00:00Z"])
        XCTAssertEqual(groups([tiedB, tiedA]).map(\.id), ["a", "b"])
        XCTAssertEqual(groups([tiedA, tiedB]).map(\.id), ["a", "b"])
    }

    func testTemporaryChatsAndMissingRootsCannotCreateActiveRows()
    {
        let temporary = chat("temporary", ["temporaryParentChatId": "root", "acpSession": ["processing": true]])
        let orphan = chat("child", ["branchRootChatId": "missing", "acpSession": ["processing": true]])
        XCTAssertTrue(groups([temporary, orphan]).isEmpty)
    }

    func testMatchingOnlyChildRetainsRootAndFamilyPolicy()
    {
        let root = chat("root", ["settledAt": "2026-10-18T12:00:00Z"])
        let question = chat("child", ["branchRootChatId": "root", "acpSession": ["attentionKind": "question"]])
        let matches: Set<String> = ["child"]
        let result = MobileActivityGroup.build([root, question], now: now, matchingIDs: matches)
        XCTAssertEqual(result.map(\.id), ["root"])
        XCTAssertEqual(result.first?.root.id, "root")
        XCTAssertEqual(result.first?.destination.id, "child")
        XCTAssertEqual(result.first?.members.count, 2)
        XCTAssertTrue(MobileActivityGroup.build([root, question], now: now, matchingIDs: []).isEmpty)
        XCTAssertTrue(MobileActivityGroup.build([question], now: now, matchingIDs: matches).isEmpty)

        let unread = chat("child", ["branchRootChatId": "root", "attentionRevision": "reply", "acpSession": ["readySinceLastSelect": true]])
        XCTAssertTrue(MobileActivityGroup.build([root, unread], now: now, matchingIDs: matches).isEmpty)
        let parkedRoot = chat("root", ["snoozedUntilMs": now.addingTimeInterval(3600).timeIntervalSince1970 * 1000])
        let working = chat("child", ["branchRootChatId": "root", "acpSession": ["processing": true]])
        XCTAssertTrue(MobileActivityGroup.build([parkedRoot, working], now: now, matchingIDs: matches).isEmpty)
        XCTAssertEqual(MobileActivityGroup.build([parkedRoot, question], now: now, matchingIDs: matches).first?.destination.id, "child")
    }

    func testWireDatesRetainWholeFractionalAndOffsetAcceptance()
    {
        let whole = parseWireDate("2026-10-18T12:00:00Z")
        XCTAssertNotNil(whole)
        for text in ["2026-10-18T14:00:00+02:00", "2026-10-18T08:00:00-04:00", "2026-10-18T14:00:00+0200", " 2026-10-18T12:00:00Z"]
        { XCTAssertEqual(parseWireDate(text), whole) }
        for text in ["2026-10-18T12:00:00.125Z", "2026-10-18T14:00:00.125+02:00"]
        { XCTAssertEqual(parseWireDate(text)?.timeIntervalSince1970 ?? 0, whole!.timeIntervalSince1970 + 0.125, accuracy: 0.000001) }
        for text in ["2026-10-18T12:00:00.123456Z", "2026-10-18T12:00:00.123456789Z", "2026-10-18T12:00:00.123456789123Z"]
        { XCTAssertNotNil(parseWireDate(text)) }
        XCTAssertNil(parseWireDate(""))
        XCTAssertNil(parseWireDate("not-a-date"))
    }

    func test1277ChatConstructionSortingAndGroupingFinishesWithinOneSecond()
    {
        // Distinct timestamps, mixed fractional precision/offsets, and real five-member families.
        let rows: [WireObject] = (0..<1277).map { index in
            let stamp = String(format: "2026-10-%02dT%02d:%02d:%02d", 15 + index % 3, index % 24, (index / 24) % 60, index % 60) +
                (index.isMultiple(of: 2) ? ".125Z" : "+02:00")
            return [
                "id": "chat-\(index)", "branchRootChatId": "chat-\((index / 5) * 5)",
                "folderId": "folder-\(index % 45)", "createdAt": "2026-10-01T12:00:00Z",
                "updatedAt": stamp, "interactionAt": stamp, "attentionRevision": "",
                "acpSession": ["processing": index % 5 == 1]
            ]
        }
        let start = ContinuousClock.now
        let chats = rows.map { MobileChat(raw: $0) }
        let sorted = chats.sorted { $0.updatedAt > $1.updatedAt }
        let result = MobileActivityGroup.build(sorted, now: now)
        let elapsed = start.duration(to: .now)
        XCTAssertEqual(result.count, 256)
        XCTAssertEqual(result.reduce(0) { $0 + $1.members.count }, 1277)
        XCTAssertLessThan(elapsed, .seconds(1), "Initial chat parsing, sorting and family grouping must stay responsive.")
    }
}
