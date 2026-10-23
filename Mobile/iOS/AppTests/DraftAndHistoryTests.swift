import XCTest
import CryptoKit
@testable import UAM

final class DraftAndHistoryTests: XCTestCase
{
    private func store() throws -> DraftStore
    {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        addTeardownBlock { try? FileManager.default.removeItem(at: directory) }
        return DraftStore(directory: directory)
    }

    func testAcknowledgementSettlesOriginWithoutTouchingOtherChat() throws
    {
        let store = try store()
        let origin = DraftContext.chat(connectionID: "mac", chatID: "a")
        let other = DraftContext.chat(connectionID: "mac", chatID: "b")
        let submitted = ComposerDraft(text: "Send this", attachments: [.init(name: "note", mimeType: "text/plain", data: Data("file".utf8))])
        let otherDraft = ComposerDraft(text: "Keep this")
        try store.save(submitted, in: origin)
        try store.save(otherDraft, in: other)
        try store.settle(origin, sentRevision: submitted.revision)
        XCTAssertEqual(try store.load(origin).text, "")
        XCTAssertTrue(try store.load(origin).attachments.isEmpty)
        XCTAssertEqual(try store.load(other), otherDraft)
    }

    func testAcknowledgementPreservesEditsIncludingEditBackToSameText() throws
    {
        let store = try store()
        let context = DraftContext.chat(connectionID: "mac", chatID: "a")
        let submitted = ComposerDraft(text: "Send")
        var edited = submitted
        edited.text = "New text"
        edited.text = "Send"
        XCTAssertNotEqual(edited.revision, submitted.revision)
        try store.save(edited, in: context)
        XCTAssertEqual(try store.settle(context, sentRevision: submitted.revision), edited)
        XCTAssertEqual(try store.load(context), edited)
    }

    func testDraftIdentitiesIsolateConnectionsProjectsAndChatKinds() throws
    {
        let store = try store()
        let contexts: [DraftContext] = [.chat(connectionID: "a:b", chatID: "c"), .chat(connectionID: "a", chatID: "b:c"),
                                        .newChat(connectionID: "a", projectID: "b:c"), .newChat(connectionID: "other", projectID: "b:c"),
                                        .newChat(connectionID: "a", projectID: "other")]
        for (index, context) in contexts.enumerated() { try store.save(.init(text: "draft-\(index)"), in: context) }
        for (index, context) in contexts.enumerated() { XCTAssertEqual(try store.load(context).text, "draft-\(index)") }
    }

    func testUnacknowledgedDraftRoundTripsAndAttachmentEditsChangeRevision() throws
    {
        let store = try store()
        let context = DraftContext.newChat(connectionID: "mac", projectID: "project")
        var draft = ComposerDraft(text: "Never automatically resend")
        let before = draft.revision
        draft.attachments.append(.init(name: "photo.jpg", mimeType: "image/jpeg", data: Data([1, 2, 3])))
        XCTAssertNotEqual(before, draft.revision)
        try store.save(draft, in: context)
        // A failure or timeout makes no settlement call.
        XCTAssertEqual(try store.load(context), draft)
        XCTAssertEqual(try DraftStore(directory: store.directory).load(context), draft)
    }

    func testLegacyDraftWithoutRevisionDecodes() throws
    {
        let draft = try JSONDecoder().decode(ComposerDraft.self, from: Data(#"{"text":"retained","attachments":[]}"#.utf8))
        XCTAssertEqual(draft.text, "retained")
        XCTAssertEqual(try JSONDecoder().decode(ComposerDraft.self, from: JSONEncoder().encode(draft)), draft)
    }

    private func manifestURL(_ store: DraftStore, _ context: DraftContext) throws -> URL
    {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        let hash = SHA256.hash(data: try encoder.encode(context)).map { String(format: "%02x", $0) }.joined()
        return store.directory.appendingPathComponent(hash + ".json")
    }

    private func blobFolder(_ store: DraftStore, _ context: DraftContext) throws -> URL
    { try manifestURL(store, context).deletingPathExtension().appendingPathExtension("attachments") }

    func testBlobDraftReloadsExactAttachmentBytesAndSmallManifest() throws
    {
        let store = try store()
        let context = DraftContext.chat(connectionID: "mac", chatID: "blob-roundtrip")
        let bytes = Data((0..<(1024 * 1024)).map { UInt8($0 % 251) })
        let photo = ComposerAttachment(name: "original.heic", mimeType: "application/octet-stream", data: bytes)
        let draft = ComposerDraft(text: "Retain my original", attachments: [photo])
        try store.save(draft, in: context)
        let manifest = try Data(contentsOf: manifestURL(store, context))
        XCTAssertLessThan(manifest.count, 2048)
        let json = try XCTUnwrap(JSONSerialization.jsonObject(with: manifest) as? [String: Any])
        XCTAssertEqual(json["version"] as? Int, 1)
        let refs = try XCTUnwrap(json["attachments"] as? [[String: Any]])
        XCTAssertNil(refs[0]["data"])
        let blobs = try FileManager.default.contentsOfDirectory(at: blobFolder(store, context), includingPropertiesForKeys: nil)
        XCTAssertEqual(blobs.count, 1)
        XCTAssertEqual(try Data(contentsOf: blobs[0]), bytes)
        XCTAssertEqual(try DraftStore(directory: store.directory).load(context), draft)
    }

    func testTextEditDoesNotRewriteAttachmentBlob() throws
    {
        let store = try store()
        let context = DraftContext.newChat(connectionID: "mac", projectID: "immutable")
        var draft = ComposerDraft(text: "First", attachments: [.init(name: "image.jpg", mimeType: "image/jpeg", data: Data(repeating: 7, count: 1024 * 1024))])
        try store.save(draft, in: context)
        let blob = try XCTUnwrap(FileManager.default.contentsOfDirectory(at: blobFolder(store, context), includingPropertiesForKeys: nil).first)
        // A fixed old mtime makes an accidental rewrite observable without timing sleeps.
        let sentinel = Date(timeIntervalSince1970: 1_000_000)
        try FileManager.default.setAttributes([.modificationDate: sentinel], ofItemAtPath: blob.path)
        draft.text = "Second text"
        try store.save(draft, in: context)
        let modified = try XCTUnwrap(FileManager.default.attributesOfItem(atPath: blob.path)[.modificationDate] as? Date)
        XCTAssertEqual(modified.timeIntervalSince1970, sentinel.timeIntervalSince1970, accuracy: 0.01)
        XCTAssertEqual(try store.load(context), draft)
    }

    func testSettlementCleansOnlyOriginBlobsWithSharedContent() throws
    {
        let store = try store()
        let origin = DraftContext.chat(connectionID: "mac", chatID: "origin")
        let other = DraftContext.chat(connectionID: "mac", chatID: "other")
        let attachment = ComposerAttachment(name: "same.bin", mimeType: "application/octet-stream", data: Data([0, 1, 2, 3]))
        let submitted = ComposerDraft(text: "Send", attachments: [attachment])
        let retained = ComposerDraft(text: "Keep", attachments: [attachment])
        try store.save(submitted, in: origin)
        try store.save(retained, in: other)
        try store.settle(origin, sentRevision: submitted.revision)
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(at: blobFolder(store, origin), includingPropertiesForKeys: nil).isEmpty)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(at: blobFolder(store, other), includingPropertiesForKeys: nil).count, 1)
        XCTAssertEqual(try store.load(other), retained)
    }

    func testMissingCorruptAndUnsafeBlobKeepManifestAndDoNotFallBack() throws
    {
        let store = try store()
        let context = DraftContext.chat(connectionID: "mac", chatID: "damaged")
        let draft = ComposerDraft(text: "Do not lose this text", attachments: [.init(name: "file", mimeType: "application/octet-stream", data: Data([1, 2, 3]))])
        try store.save(draft, in: context)
        let manifest = try manifestURL(store, context)
        let originalManifest = try Data(contentsOf: manifest)
        let blob = try XCTUnwrap(FileManager.default.contentsOfDirectory(at: blobFolder(store, context), includingPropertiesForKeys: nil).first)
        try FileManager.default.removeItem(at: blob)
        XCTAssertThrowsError(try store.load(context))
        XCTAssertThrowsError(try store.settle(context, sentRevision: draft.revision))
        XCTAssertEqual(try Data(contentsOf: manifest), originalManifest)
        try Data([3, 2, 1]).write(to: blob)
        XCTAssertThrowsError(try store.load(context))
        XCTAssertEqual(try Data(contentsOf: manifest), originalManifest)
        try draft.attachments[0].data.write(to: blob)
        var json = try XCTUnwrap(JSONSerialization.jsonObject(with: originalManifest) as? [String: Any])
        var refs = try XCTUnwrap(json["attachments"] as? [[String: Any]])
        for invalidName in ["../outside", blob.lastPathComponent.uppercased(), String(repeating: "a", count: 63)]
        {
            refs[0]["blob"] = invalidName; json["attachments"] = refs
            let badManifest = try JSONSerialization.data(withJSONObject: json)
            try badManifest.write(to: manifest)
            XCTAssertThrowsError(try store.load(context))
            XCTAssertEqual(try Data(contentsOf: manifest), badManifest)
        }
    }

    func testFailedBlobWritePreservesPreviousDraftAndFiles() throws
    {
        let store = try store()
        let context = DraftContext.chat(connectionID: "mac", chatID: "write-failure")
        let previous = ComposerDraft(text: "Previous", attachments: [.init(name: "previous", mimeType: "text/plain", data: Data([1]))])
        try store.save(previous, in: context)
        let manifest = try manifestURL(store, context)
        let previousManifest = try Data(contentsOf: manifest)
        let incoming = ComposerAttachment(name: "new", mimeType: "text/plain", data: Data([2]))
        let hash = SHA256.hash(data: incoming.data).map { String(format: "%02x", $0) }.joined()
        // An incompatible filesystem entry forces a real write-path failure without relying
        // on process permissions (CI can run with elevated filesystem rights).
        try FileManager.default.createDirectory(at: blobFolder(store, context).appendingPathComponent(hash), withIntermediateDirectories: true)
        XCTAssertThrowsError(try store.save(ComposerDraft(text: "New", attachments: [incoming]), in: context))
        XCTAssertEqual(try Data(contentsOf: manifest), previousManifest)
        XCTAssertEqual(try store.load(context), previous)
    }

    func testLegacyInlineMigrationAndSettledManifestPreventResurrection() throws
    {
        let store = try store()
        let context = DraftContext.chat(connectionID: "mac", chatID: "legacy")
        let draft = ComposerDraft(text: "Legacy original", attachments: [.init(name: "old.bin", mimeType: "application/octet-stream", data: Data([9, 8, 7]))])
        try FileManager.default.createDirectory(at: store.directory, withIntermediateDirectories: true)
        let canonical = try manifestURL(store, context)
        let inline = try JSONEncoder().encode(draft)
        try inline.write(to: canonical)
        XCTAssertEqual(try store.load(context), draft)
        // Loading the inline canonical record does not rewrite it; the next save migrates it.
        XCTAssertEqual(try Data(contentsOf: canonical), inline)
        try store.save(draft, in: context)
        XCTAssertEqual(try store.load(context), draft)
        XCTAssertNotEqual(try Data(contentsOf: canonical), inline)
        let legacyHash = SHA256.hash(data: Data("mac:legacy".utf8)).map { String(format: "%02x", $0) }.joined()
        try inline.write(to: store.directory.appendingPathComponent(legacyHash + ".json"))
        try store.settle(context, sentRevision: draft.revision)
        XCTAssertTrue(try store.load(context).attachments.isEmpty)
        XCTAssertEqual(try store.load(context).text, "")
        XCTAssertEqual(try DraftStore(directory: store.directory).load(context).text, "")
    }

    private func messages(_ range: Range<Int>) -> [MobileMessage]
    { range.map { MobileMessage(index: $0, raw: ["role": "assistant", "content": "message-\($0)"]) } }

    func testTailGapDropsStalePrefixAndExposesOlderCursor() throws
    {
        let merge = HistoryPageMerge(existing: messages(0..<50), existingDigest: "old", loaded: messages(350..<550), pageDigest: "new", older: false)
        XCTAssertEqual(merge.messages.map(\.index), Array(350..<550))
        XCTAssertEqual(merge.messages.first?.index, 350)
        XCTAssertEqual(merge.digest, "new")
    }

    func testOlderPageFromNewRevisionCannotPoisonTailDigest() throws
    {
        let existing = messages(50..<100)
        let merge = HistoryPageMerge(existing: existing, existingDigest: "before-append", loaded: messages(0..<50), pageDigest: "after-append", older: true)
        XCTAssertTrue(merge.needsTailRefresh)
        XCTAssertEqual(merge.digest, "")
        XCTAssertEqual(merge.messages.map(\.index), existing.map(\.index))
    }

    func testOlderPagesJoinOnlyAdjacentSameRevisionWindows() throws
    {
        let valid = HistoryPageMerge(existing: messages(50..<100), existingDigest: "revision", loaded: messages(0..<50), pageDigest: "revision", older: true)
        XCTAssertFalse(valid.needsTailRefresh)
        XCTAssertEqual(valid.messages.map(\.index), Array(0..<100))
        let gap = HistoryPageMerge(existing: messages(70..<100), existingDigest: "revision", loaded: messages(0..<50), pageDigest: "revision", older: true)
        XCTAssertTrue(gap.needsTailRefresh)
    }

    func testChangedHistoryInvalidatesCachedEarlierMessages() throws
    {
        let merge = HistoryPageMerge(existing: messages(0..<100), existingDigest: "old", loaded: messages(50..<100), pageDigest: "rewritten", older: false)
        XCTAssertEqual(merge.messages.map(\.index), Array(50..<100))
        XCTAssertEqual(merge.digest, "rewritten")
    }
}
