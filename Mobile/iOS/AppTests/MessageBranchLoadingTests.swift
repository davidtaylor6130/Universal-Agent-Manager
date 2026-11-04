import XCTest
import UAMKit
@testable import UAM

@MainActor final class MessageBranchLoadingTests: XCTestCase
{
    @MainActor private final class Fixture
    {
        enum OlderResponse { case normal, unchanged, failure }
        struct Read
        {
            let chatID: String
            let before: Int?
            let limit: Int
        }
        var reads: [Read] = []
        var olderResponse = OlderResponse.normal
        var holdNextOlder = false
        var waiting: CheckedContinuation<Data, Never>?

        func page(_ id: String, start: Int, end: Int) -> WireObject
        {
            ["chatId": id, "startIndex": start, "totalCount": id == "other" ? 1 : 180,
             "messagesDigest": "revision-\(id)",
             "messages": (start..<end).map { index in
                 ["role": index.isMultiple(of: 2) ? "user" : "assistant", "content": "\(id) message \(index)"]
             }]
        }

        func request(_ action: String, _ payload: WireObject) async throws -> WireObject
        {
            guard action == "getChatMessages" else { return [:] }
            let id = payload.text("chatId")
            let before = payload["before"] as? Int
            let limit = Int(payload.number("limit"))
            reads.append(Read(chatID: id, before: before, limit: limit))
            if id == "target", before != nil
            {
                if holdNextOlder
                {
                    holdNextOlder = false
                    let data: Data = await withCheckedContinuation { waiting = $0 }
                    return try JSONSerialization.jsonObject(with: data) as! WireObject
                }
                switch olderResponse
                {
                case .unchanged: return ["unchanged": true]
                case .failure: throw CompanionError.network(code: -1004)
                case .normal: break
                }
            }
            let total = id == "other" ? 1 : 180
            let end = min(before ?? total, total)
            return page(id, start: max(0, end - limit), end: end)
        }

        func releaseOlder()
        {
            let continuation = waiting
            waiting = nil
            continuation?.resume(returning: try! JSONSerialization.data(withJSONObject: page("target", start: 80, end: 130)))
        }
    }

    private func model(_ fixture: Fixture) throws -> MobileAppModel
    {
        let suite = "uam-message-branch-loading-" + UUID().uuidString
        let preferences = try XCTUnwrap(UserDefaults(suiteName: suite))
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(suite, isDirectory: true)
        addTeardownBlock
        {
            UserDefaults(suiteName: suite)?.removePersistentDomain(forName: suite)
            try? FileManager.default.removeItem(at: directory)
        }
        let model = MobileAppModel(preferences: preferences, draftStore: DraftStore(directory: directory), requestHandler: fixture.request)
        model.setForeground(false)
        model.activeConnectionID = "isolated-" + suite
        model.chats = ["source", "target", "other"].map { MobileChat(raw: ["id": $0, "attentionRevision": ""]) }
        return model
    }

    private func waitForOlder(_ fixture: Fixture) async
    {
        let deadline = ContinuousClock.now.advanced(by: .seconds(2))
        while fixture.waiting == nil, ContinuousClock.now < deadline { await Task.yield() }
        XCTAssertNotNil(fixture.waiting, "The target's older page must be suspended before changing navigation.")
    }

    func testLoadsBeyondTailPageToTheAbsoluteSourceMessage()
    async throws
    {
        let fixture = Fixture(), model = try model(fixture)
        _ = await model.openChat("source")
        await model.loadMessages("source", older: true)
        await model.loadMessages("source", older: true)
        XCTAssertEqual(model.messages.first(where: { $0.index == 34 })?.role, "user")
        model.draft.text = "Keep source draft"
        fixture.reads = []

        let loaded = await model.openMessageBranch("target", at: 34)
        XCTAssertTrue(loaded)
        XCTAssertEqual(model.activeChatID, "target")
        XCTAssertEqual(model.historyStartIndex, 30)
        XCTAssertEqual(model.historyTotal, 180)
        XCTAssertEqual(model.messages.map(\.index), Array(30..<180))
        XCTAssertEqual(model.messages.first(where: { $0.index == 34 })?.content, "target message 34")
        XCTAssertEqual(fixture.reads.map(\.chatID), ["target", "target", "target"])
        XCTAssertEqual(fixture.reads.map(\.before), [nil, 130, 80])
        XCTAssertTrue(fixture.reads.allSatisfy { $0.limit == 50 })
        _ = await model.openChat("source")
        XCTAssertEqual(model.draft.text, "Keep source draft")
    }

    func testAnchorAlreadyInTailDoesNotReadOlderPages() async throws
    {
        let fixture = Fixture(), model = try model(fixture)
        let loaded = await model.openMessageBranch("target", at: 150)
        XCTAssertTrue(loaded)
        XCTAssertEqual(fixture.reads.count, 1)
        XCTAssertNil(fixture.reads.first?.before)
        XCTAssertEqual(model.historyStartIndex, 130)
    }

    func testUnchangedOlderPageStopsWithoutRetryingOrInventingAnchor() async throws
    {
        let fixture = Fixture(), model = try model(fixture)
        fixture.olderResponse = .unchanged
        let loaded = await model.openMessageBranch("target", at: 34)
        XCTAssertFalse(loaded)
        XCTAssertEqual(fixture.reads.map(\.before), [nil, 130])
        XCTAssertEqual(model.historyStartIndex, 130)
        XCTAssertEqual(model.messages.map(\.index), Array(130..<180))
        XCTAssertFalse(model.loadingOlder)
    }

    func testFailedOlderPageStopsAndRetainsAcceptedTail() async throws
    {
        let fixture = Fixture(), model = try model(fixture)
        fixture.olderResponse = .failure
        let loaded = await model.openMessageBranch("target", at: 34)
        XCTAssertFalse(loaded)
        XCTAssertEqual(fixture.reads.map(\.before), [nil, 130])
        XCTAssertEqual(model.historyStartIndex, 130)
        XCTAssertEqual(model.messages.map(\.index), Array(130..<180))
        XCTAssertNotNil(model.error)
        XCTAssertFalse(model.loadingOlder)
    }

    func testNavigationDuringOlderReadPreservesOtherChatAndReturnsNoFocusSuccess() async throws
    {
        let fixture = Fixture(), model = try model(fixture)
        fixture.holdNextOlder = true
        let opening = Task { await model.openMessageBranch("target", at: 34) }
        await waitForOlder(fixture)
        _ = await model.openChat("other")
        let otherContext = model.draftContextID
        model.draft.text = "Keep other draft"
        fixture.releaseOlder()
        let loaded = await opening.value
        XCTAssertFalse(loaded, "An obsolete operation must not claim a scroll anchor in another chat.")
        XCTAssertEqual(model.activeChatID, "other")
        XCTAssertEqual(model.draftContextID, otherContext)
        XCTAssertEqual(model.messages.map(\.content), ["other message 0"])
        XCTAssertEqual(model.historyStartIndex, 0)
        XCTAssertEqual(model.draft.text, "Keep other draft")
        XCTAssertNil(model.error)
        XCTAssertEqual(fixture.reads.filter { $0.chatID == "target" }.count, 2)
    }

    func testLeavingAndReopeningSameChatDoesNotLetOldPageClaimNewNavigation() async throws
    {
        let fixture = Fixture(), model = try model(fixture)
        fixture.holdNextOlder = true
        let opening = Task { await model.openMessageBranch("target", at: 34) }
        await waitForOlder(fixture)
        let abandonedContext = model.draftContextID
        model.closeChat()
        _ = await model.openChat("target")
        let reopenedContext = model.draftContextID
        XCTAssertNotEqual(reopenedContext, abandonedContext)
        fixture.releaseOlder()
        let loaded = await opening.value
        XCTAssertFalse(loaded)
        XCTAssertEqual(model.activeChatID, "target")
        XCTAssertEqual(model.draftContextID, reopenedContext)
        XCTAssertEqual(model.historyStartIndex, 130)
        XCTAssertEqual(model.messages.map(\.index), Array(130..<180))
        XCTAssertNil(model.error)
    }

    func testCancellationStopsOlderLoadingWithoutPublishingLatePage() async throws
    {
        let fixture = Fixture(), model = try model(fixture)
        fixture.holdNextOlder = true
        let opening = Task { await model.openMessageBranch("target", at: 34) }
        await waitForOlder(fixture)
        opening.cancel()
        fixture.releaseOlder()
        let loaded = await opening.value
        XCTAssertFalse(loaded)
        XCTAssertEqual(model.historyStartIndex, 130)
        XCTAssertEqual(model.messages.map(\.index), Array(130..<180))
        XCTAssertEqual(fixture.reads.count, 2)
        XCTAssertNil(model.error)
    }

    func testInvalidOrUnavailableAnchorDoesNotInventSuccess() async throws
    {
        let fixture = Fixture(), model = try model(fixture)
        let invalid = await model.openMessageBranch("target", at: -1)
        XCTAssertFalse(invalid)
        XCTAssertTrue(fixture.reads.isEmpty)
        XCTAssertNil(model.activeChatID)
        let unavailable = await model.openMessageBranch("other", at: 34)
        XCTAssertFalse(unavailable)
        XCTAssertEqual(fixture.reads.count, 1)
        XCTAssertEqual(model.messages.map(\.content), ["other message 0"])
    }
}
