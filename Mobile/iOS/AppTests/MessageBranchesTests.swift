import XCTest
@testable import UAM

final class MessageBranchesTests: XCTestCase
{
    private func chat(_ id: String, _ fields: WireObject = [:]) -> MobileChat
    {
        var raw: WireObject = ["id": id, "branchRootChatId": "root", "createdAt": "2026-10-18T12:00:00Z"]
        raw.merge(fields) { _, next in next }
        return MobileChat(raw: raw)
    }

    private func branch(_ id: String, parent: String = "root", index: Int = 4, created: String = "2026-10-18T12:01:00Z") -> MobileChat
    { chat(id, ["parentChatId": parent, "branchFromMessageIndex": index, "createdAt": created]) }

    func testOriginalAndSiblingAlternativesUseCreationOrderAndBoundedNavigation()
    {
        let root = chat("root")
        let edited = branch("edited")
        let resent = branch("resent", created: "2026-10-18T12:02:00Z")
        let catalogue = [resent, root, edited]
        let original = MessageBranchGroup.build(chat: root, messageIndex: 4, chats: catalogue)
        XCTAssertEqual(original?.parentID, "root")
        XCTAssertEqual(original?.messageIndex, 4)
        XCTAssertEqual(original?.orderedMembers.map(\.id), ["root", "edited", "resent"])
        XCTAssertEqual(original?.currentIndex, 0)
        XCTAssertEqual(original?.current, 1)
        XCTAssertEqual(original?.total, 3)
        XCTAssertNil(original?.previous)
        XCTAssertEqual(original?.next?.id, "edited")
        let middle = MessageBranchGroup.build(chat: edited, messageIndex: 4, chats: catalogue)
        XCTAssertEqual(middle?.currentIndex, 1)
        XCTAssertEqual(middle?.previous?.id, "root")
        XCTAssertEqual(middle?.next?.id, "resent")
        let last = MessageBranchGroup.build(chat: resent, messageIndex: 4, chats: catalogue)
        XCTAssertEqual(last?.previous?.id, "edited")
        XCTAssertNil(last?.next)
    }

    func testDeepForkUsesImmediateParentWithoutFlatteningFamily()
    {
        let root = chat("root"), a = branch("a"), b = branch("b")
        let c = branch("c", parent: "a", index: 9, created: "2026-10-18T12:02:00Z")
        let d = branch("d", parent: "a", index: 9, created: "2026-10-18T12:03:00Z")
        let e = branch("e", parent: "c", index: 15, created: "2026-10-18T12:04:00Z")
        let otherParent = branch("other", parent: "b", index: 9)
        let catalogue = [root, a, b, c, d, e, otherParent]
        let first = MessageBranchGroup.build(chat: a, messageIndex: 9, chats: catalogue)
        XCTAssertEqual(first?.orderedMembers.map(\.id), ["a", "c", "d"])
        let child = MessageBranchGroup.build(chat: c, messageIndex: 9, chats: catalogue)
        XCTAssertEqual(child?.parentID, "a")
        XCTAssertEqual(child?.orderedMembers.map(\.id), ["a", "c", "d"])
        XCTAssertEqual(child?.current, 2)
        let deeper = MessageBranchGroup.build(chat: c, messageIndex: 15, chats: catalogue)
        XCTAssertEqual(deeper?.parentID, "c")
        XCTAssertEqual(deeper?.orderedMembers.map(\.id), ["c", "e"])
        XCTAssertNil(MessageBranchGroup.build(chat: c, messageIndex: 4, chats: catalogue))
        XCTAssertNil(MessageBranchGroup.build(chat: root, messageIndex: 9, chats: catalogue))
    }

    func testSameFamilyChildrenAtAnotherMessageDoNotJoin()
    {
        let root = chat("root"), matching = branch("matching")
        let different = branch("different", index: 5)
        let grandchild = branch("grandchild", parent: "matching", index: 4)
        let result = MessageBranchGroup.build(chat: root, messageIndex: 4, chats: [root, different, grandchild, matching])
        XCTAssertEqual(result?.orderedMembers.map(\.id), ["root", "matching"])
    }

    func testTemporarySubagentAndForeignFamilyRowsCannotJoinBranches()
    {
        let root = chat("root"), real = branch("real")
        let temporary = chat("temporary", ["parentChatId": "root", "branchFromMessageIndex": 4, "temporaryParentChatId": "root"])
        let subagent = chat("subagent", ["parentChatId": "root", "branchFromMessageIndex": 4, "subagentParentChatId": "root"])
        let foreign = chat("foreign", ["parentChatId": "root", "branchFromMessageIndex": 4, "branchRootChatId": "other-root"])
        let unlinked = chat("unlinked", ["subagentParentChatId": "root"])
        let catalogue = [root, temporary, subagent, foreign, unlinked, real]
        XCTAssertEqual(MessageBranchGroup.build(chat: root, messageIndex: 4, chats: catalogue)?.orderedMembers.map(\.id), ["root", "real"])
        XCTAssertNil(MessageBranchGroup.build(chat: temporary, messageIndex: 4, chats: catalogue))
        XCTAssertNil(MessageBranchGroup.build(chat: subagent, messageIndex: 4, chats: catalogue))
    }

    func testMissingParentKeepsOnlyActualSiblingAlternatives()
    {
        let first = branch("first", parent: "deleted-parent")
        let second = branch("second", parent: "deleted-parent", created: "2026-10-18T12:02:00Z")
        XCTAssertNil(MessageBranchGroup.build(chat: first, messageIndex: 4, chats: [first]))
        let result = MessageBranchGroup.build(chat: first, messageIndex: 4, chats: [second, first])
        XCTAssertEqual(result?.parentID, "deleted-parent")
        XCTAssertEqual(result?.orderedMembers.map(\.id), ["first", "second"])
        XCTAssertEqual(result?.next?.id, "second")
    }

    func testInvalidForkMetadataCannotPretendToBeMessageZero()
    {
        let root = chat("root"), real = branch("real", index: 0)
        let invalid: [Any] = [true, false, -1, 0.5, "0", Int64.max, Double.nan, Double.infinity, NSNull()]
        let rows = invalid.enumerated().map { offset, value in
            chat("invalid-\(offset)", ["parentChatId": "root", "branchFromMessageIndex": value])
        }
        let missing = chat("missing-index", ["parentChatId": "root"])
        let result = MessageBranchGroup.build(chat: root, messageIndex: 0, chats: [root, real, missing] + rows)
        XCTAssertEqual(result?.orderedMembers.map(\.id), ["root", "real"])
        XCTAssertNil(MessageBranchGroup.build(chat: root, messageIndex: -1, chats: [root, real]))
        XCTAssertNil(MessageBranchGroup.build(chat: root, messageIndex: Int.max, chats: [root, real]))
    }

    func testForkIndexUsesAbsoluteTranscriptPositionAcrossHistoryPages()
    {
        let root = chat("root"), fork = branch("fork", index: 125)
        let result = MessageBranchGroup.build(chat: root, messageIndex: 125, chats: [root, fork])
        XCTAssertEqual(result?.messageIndex, 125)
        XCTAssertEqual(result?.next?.id, "fork")
        XCTAssertNil(MessageBranchGroup.build(chat: root, messageIndex: 5, chats: [root, fork]))
        XCTAssertEqual(MessageBranchGroup.build(chat: fork, messageIndex: 125, chats: [root, fork])?.previous?.id, "root")
    }

    func testEqualCreationTimesRetainStableCatalogueOrder()
    {
        let root = chat("root")
        let z = branch("z", created: "2026-10-18T12:00:00Z")
        let a = branch("a", created: "2026-10-18T12:00:00Z")
        for _ in 0..<3
        {
            let result = MessageBranchGroup.build(chat: z, messageIndex: 4, chats: [root, z, a])
            XCTAssertEqual(result?.orderedMembers.map(\.id), ["root", "z", "a"])
            XCTAssertEqual(result?.currentIndex, 1)
        }
    }

    func testReadOnlyHistoryRemainsNavigableWithoutInventingMissingCurrentChat()
    {
        let root = chat("root", ["importedReadOnly": true])
        let fork = chat("fork", ["parentChatId": "root", "branchFromMessageIndex": 4, "importedReadOnly": true])
        XCTAssertEqual(MessageBranchGroup.build(chat: fork, messageIndex: 4, chats: [root, fork])?.previous?.id, "root")
        XCTAssertNil(MessageBranchGroup.build(chat: fork, messageIndex: 4, chats: [root]))
        XCTAssertNil(MessageBranchGroup.build(chat: root, messageIndex: 4, chats: [fork, branch("other")]))
    }

    func testDuplicateIdentityDoesNotProduceDuplicateNavigationTargets()
    {
        let root = chat("root"), fork = branch("fork")
        let result = MessageBranchGroup.build(chat: root, messageIndex: 4, chats: [root, fork, fork, root])
        XCTAssertEqual(result?.orderedMembers.map(\.id), ["root", "fork"])
    }
}
