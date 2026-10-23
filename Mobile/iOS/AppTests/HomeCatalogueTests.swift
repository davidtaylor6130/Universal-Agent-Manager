import XCTest
@testable import UAM

final class HomeCatalogueTests: XCTestCase
{
    private let now = Date(timeIntervalSince1970: 1_792_324_800)

    private func chat(_ id: String, _ fields: WireObject = [:]) -> MobileChat
    {
        var raw: WireObject = [
            "id": id, "createdAt": "2026-10-01T12:00:00Z", "updatedAt": "2026-10-18T12:00:00Z",
            "interactionAt": "2026-10-17T12:00:00Z", "attentionRevision": ""
        ]
        raw.merge(fields) { _, next in next }
        return MobileChat(raw: raw)
    }

    private func folder(_ id: String) -> WireObject
    { ["id": id, "title": "Workspace \(id)", "collapsed": false] }

    private func collection(_ id: String, _ folders: [String]) -> WireObject
    {
        ["id": id, "name": "Collection \(id)", "collapsed": false,
         "references": folders.enumerated().map { ["id": "\(id)-\($0.offset)", "type": "workspace-folder", "target": $0.element, "label": $0.element] }]
    }

    private func build(_ chats: [MobileChat], folders: [WireObject] = [], collections: [WireObject] = [], projectID: String = "", matchingIDs: Set<String>? = nil) -> HomeCatalogue
    { HomeCatalogue.build(chats: chats, folders: folders, collections: collections, projectID: projectID, matchingIDs: matchingIDs, now: now) }

    func testPinnedIsFlatAndAllRetainsPinnedAndSnoozedRoots()
    {
        let root = chat("pinned", ["folderId": "work", "pinned": true, "snoozedUntilMs": now.addingTimeInterval(3600).timeIntervalSince1970 * 1000])
        let other = chat("other", ["folderId": "work"])
        let result = build([root, other], folders: [folder("work")], collections: [collection("group", ["work"])])
        XCTAssertEqual(result.pinned.map(\.id), ["pinned"])
        XCTAssertEqual(result.snoozed.map(\.id), ["pinned"])
        XCTAssertEqual(Set(result.roots.map(\.id)), ["pinned", "other"])
        XCTAssertEqual(Set(result.collections[0].workspaces[0].chats.map(\.id)), ["pinned", "other"])
        XCTAssertEqual(result.collections[0].chatCount, 2)
        XCTAssertTrue(result.workspaces.isEmpty)
    }

    func testCollectionsClaimValidFoldersOnceInReferenceOrder()
    {
        let roots = [chat("a", ["folderId": "a"]), chat("b", ["folderId": "b"]), chat("c", ["folderId": "c"])]
        let result = build(roots, folders: [folder("a"), folder("b"), folder("c"), folder("empty")], collections: [
            collection("first", ["missing", "b", "a", "b"]),
            collection("second", ["a", "c"]), collection("empty", [])
        ])
        XCTAssertEqual(result.collections.map(\.id), ["first", "second", "empty"])
        XCTAssertEqual(result.collections[0].workspaces.map(\.id), ["b", "a"])
        XCTAssertEqual(result.collections[1].workspaces.map(\.id), ["c"])
        XCTAssertTrue(result.collections[2].workspaces.isEmpty)
        XCTAssertEqual(result.workspaces.map(\.id), ["empty"])
        XCTAssertTrue(result.workspaces[0].chats.isEmpty)
        XCTAssertTrue(result.unsorted.isEmpty)
    }

    func testPassiveResourcesAndOrganisationMetadataRemainAvailable()
    {
        var workspace = folder("a")
        workspace["collapsed"] = true
        workspace["customIcon"] = ["kind": "emoji", "value": "folder"]
        var group = collection("group", ["a"])
        group["collapsed"] = true
        var references = group.objects("references")
        references.append(["id": "app", "type": "desktop-app", "target": "example", "label": "Desktop tool"])
        references.append(["id": "chat-ref", "type": "chat", "target": "chat", "label": "Not a second chat owner"])
        group["references"] = references
        let result = build([chat("chat", ["folderId": "a"])], folders: [workspace], collections: [group])
        XCTAssertTrue(result.collections[0].collapsed)
        XCTAssertTrue(result.collections[0].workspaces[0].collapsed)
        XCTAssertEqual(result.collections[0].name, "Collection group")
        XCTAssertEqual(result.collections[0].workspaces[0].title, "Workspace a")
        XCTAssertEqual(result.collections[0].raw.objects("references").map { $0.text("id") }, ["group-0", "app", "chat-ref"])
        XCTAssertFalse(result.collections[0].workspaces[0].raw.object("customIcon").isEmpty)
        XCTAssertEqual(result.collections[0].chatCount, 1)
    }

    func testUnsortedIncludesUnfiledAndUnknownFoldersWithoutLosingRoots()
    {
        let result = build([
            chat("unfiled"), chat("unknown", ["folderId": "missing"]), chat("known", ["folderId": "known"])
        ], folders: [folder("known")])
        XCTAssertEqual(Set(result.unsorted.map(\.id)), ["unfiled", "unknown"])
        XCTAssertEqual(result.workspaces[0].chats.map(\.id), ["known"])
        XCTAssertEqual(result.workspaces[0].chatCount + result.unsorted.count, result.roots.count)
    }

    func testUnsortedPlacesUnfiledFirstThenUnknownFolderBuckets()
    {
        let result = build([
            chat("unfiled-old", ["interactionAt": "2026-10-15T12:00:00Z"]),
            chat("unknown-a-new", ["folderId": "missing-a", "interactionAt": "2026-10-18T12:00:00Z"]),
            chat("unknown-b", ["folderId": "missing-b", "interactionAt": "2026-10-17T12:00:00Z"]),
            chat("unknown-a-old", ["folderId": "missing-a", "interactionAt": "2026-10-16T12:00:00Z"]),
            chat("unfiled-new", ["interactionAt": "2026-10-16T13:00:00Z"])
        ])
        XCTAssertEqual(result.roots.map(\.id), ["unknown-a-new", "unknown-b", "unfiled-new", "unknown-a-old", "unfiled-old"])
        XCTAssertEqual(result.unsorted.map(\.id), ["unfiled-new", "unfiled-old", "unknown-a-new", "unknown-a-old", "unknown-b"])
    }

    func testChildSearchRetainsAncestorsAndProjectUsesRootMetadata()
    {
        let root = chat("root", ["folderId": "a", "pinned": true])
        let child = chat("child", ["branchRootChatId": "root", "folderId": "b", "acpSession": ["attentionKind": "question"]])
        let folders = [folder("a"), folder("b"), folder("empty")]
        let collections = [collection("group", ["a"]), collection("unrelated", [])]
        let result = build([root, child], folders: folders, collections: collections, projectID: "a", matchingIDs: ["child"])
        XCTAssertEqual(result.roots.map(\.id), ["root"])
        XCTAssertEqual(result.pinned.map(\.id), ["root"])
        XCTAssertEqual(result.membersByRootID["root"]?.map(\.id), ["root", "child"])
        XCTAssertEqual(result.collections.map(\.id), ["group"])
        XCTAssertEqual(result.collections[0].workspaces[0].chats.map(\.id), ["root"])
        XCTAssertTrue(result.workspaces.isEmpty)
        XCTAssertTrue(build([root, child], folders: folders, collections: collections, projectID: "b", matchingIDs: ["child"]).roots.isEmpty)
        XCTAssertTrue(build([root, child], folders: folders, collections: collections, matchingIDs: []).collections.isEmpty)
        XCTAssertTrue(build([root, child], folders: folders, matchingIDs: []).workspaces.isEmpty)
    }

    func testProjectNarrowingHidesUnrelatedGroupsButCanShowItsEmptyWorkspace()
    {
        let result = build([], folders: [folder("a"), folder("b")], collections: [collection("one", ["a"]), collection("two", ["b"])], projectID: "b")
        XCTAssertEqual(result.collections.map(\.id), ["two"])
        XCTAssertEqual(result.collections[0].workspaces.map(\.id), ["b"])
        XCTAssertEqual(result.collections[0].chatCount, 0)
        XCTAssertTrue(result.workspaces.isEmpty)
    }

    func testTransientOrOrphanChildrenNeverBecomeStandaloneRows()
    {
        let root = chat("root")
        let child = chat("child", ["branchRootChatId": "root", "pinned": true])
        let temporary = chat("temporary", ["temporaryParentChatId": "root", "pinned": true])
        let orphan = chat("orphan", ["branchRootChatId": "missing", "pinned": true])
        let result = build([root, child, temporary, orphan])
        XCTAssertEqual(result.roots.map(\.id), ["root"])
        XCTAssertEqual(result.unsorted.map(\.id), ["root"])
        XCTAssertTrue(result.pinned.isEmpty)
        XCTAssertEqual(result.membersByRootID["root"]?.map(\.id), ["root", "child"])
        XCTAssertTrue(build([root, child], matchingIDs: ["temporary"]).roots.isEmpty)
    }

    func testSnoozedListUsesRootAndFamilyInputRatherThanWorkingState()
    {
        let future = now.addingTimeInterval(3600).timeIntervalSince1970 * 1000
        let root = chat("root", ["snoozedUntilMs": future])
        let working = chat("working", ["branchRootChatId": "root", "acpSession": ["processing": true]])
        XCTAssertEqual(build([root, working]).snoozed.map(\.id), ["root"])
        let question = chat("question", ["branchRootChatId": "root", "acpSession": ["attentionKind": "question"]])
        XCTAssertTrue(build([root, working, question]).snoozed.isEmpty)
        let error = chat("error", ["branchRootChatId": "root", "acpSession": ["attentionKind": "error"]])
        XCTAssertEqual(build([root, error]).snoozed.map(\.id), ["root"])
        let unparkedRoot = chat("root")
        let parkedChild = chat("child", ["branchRootChatId": "root", "snoozedUntilMs": future])
        XCTAssertTrue(build([unparkedRoot, parkedChild]).snoozed.isEmpty)
        XCTAssertTrue(build([chat("expired", ["snoozedUntilMs": now.addingTimeInterval(-1).timeIntervalSince1970 * 1000])]).snoozed.isEmpty)
    }

    func testCachedFamilyRecencyIncludesImportedHistoryAndIgnoresStreamingUpdates()
    {
        let root = chat("root", ["interactionAt": "2026-10-16T12:00:00Z", "updatedAt": "2026-10-20T12:00:00Z"])
        let child = chat("child", ["branchRootChatId": "root", "interactionAt": "2026-10-17T13:00:00Z"])
        let other = chat("other", ["interactionAt": "2026-10-17T12:00:00Z"])
        let imported = chat("imported", ["interactionAt": "", "updatedAt": "2026-10-17T14:00:00Z"])
        XCTAssertEqual(build([root, child, other, imported]).roots.map(\.id), ["imported", "root", "other"])
        let a = chat("a"), b = chat("b")
        XCTAssertEqual(build([b, a]).roots.map(\.id), ["a", "b"])
        XCTAssertEqual(build([a, b]).roots.map(\.id), ["a", "b"])
        let newerCreation = chat("newer", ["createdAt": "2026-10-02T12:00:00Z"])
        XCTAssertEqual(build([a, newerCreation]).roots.map(\.id), ["newer", "a"])
    }
}
