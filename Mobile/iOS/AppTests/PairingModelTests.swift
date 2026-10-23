import XCTest
import UAMKit
@testable import UAM

@MainActor final class PairingModelTests: XCTestCase
{
    @MainActor private final class Checker
    {
        var count = 0
        var waiting: CheckedContinuation<Data, Never>?
        func check(_ client: CompanionClient) async throws -> WireObject
        {
            count += 1
            let bytes: Data = await withCheckedContinuation { waiting = $0 }
            return try JSONSerialization.jsonObject(with: bytes) as! WireObject
        }
        func finish(_ value: WireObject)
        { waiting!.resume(returning: try! JSONSerialization.data(withJSONObject: value)); waiting = nil }
    }
    @MainActor private final class Writes { var values: [(String, String)] = []; var requests = 0 }

    private func fixture(_ checker: Checker, _ writes: Writes) throws -> (MobileAppModel, DraftStore)
    {
        let suite = "uam-pairing-model-" + UUID().uuidString
        let preferences = try XCTUnwrap(UserDefaults(suiteName: suite))
        let store = DraftStore(directory: FileManager.default.temporaryDirectory.appendingPathComponent(suite))
        addTeardownBlock { UserDefaults(suiteName: suite)?.removePersistentDomain(forName: suite); try? FileManager.default.removeItem(at: store.directory) }
        let model = MobileAppModel(preferences: preferences, draftStore: store, requestHandler: { _, _ in writes.requests += 1; return [:] },
                                   pairingRequestHandler: checker.check, credentialWriter: { writes.values.append(($0, $1)) })
        model.setForeground(false)
        return (model, store)
    }
    private func wait(_ checker: Checker) async
    {
        for _ in 0..<1000 { if checker.waiting != nil { return }; await Task.yield() }
        XCTFail("Expected local suspended pairing check")
    }
    private var snapshot: WireObject
    { ["chats": [["id": "verified", "title": "Verified snapshot"]], "folders": [], "providers": [], "appVersion": "verified-fixture", "bootId": "pairing-fixture"] }

    func testCancelledValidationNeverPersistsEvenWhenCheckReturnsLate() async throws
    {
        let checker = Checker(), writes = Writes(), (model, _) = try fixture(checker, writes)
        let pending = Task { try await model.addConnection(name: "Fixture", address: "https://pairing.example", token: "fixture-token") }
        await wait(checker)
        pending.cancel()
        checker.finish(snapshot)
        do { try await pending.value; XCTFail("Cancelled validation must not activate") }
        catch is CancellationError { }
        XCTAssertTrue(writes.values.isEmpty)
        XCTAssertTrue(model.connections.isEmpty)
        XCTAssertNil(model.activeConnectionID)
        XCTAssertFalse(model.connected)
    }

    func testSuccessfulValidationActivatesItsSnapshotOnceAndPreservesSameOriginDraftIdentity() async throws
    {
        let checker = Checker(), writes = Writes(), (model, store) = try fixture(checker, writes)
        let saved = MobileConnection(id: "retained-fixture", name: "Previous name", address: "https://PAIRING.example:443/companion")
        model.connections = [saved]; model.activeConnectionID = saved.id; model.activeChatID = "draft-chat"
        model.draft = .init(text: "Unsent original")
        let original = model.draft
        let pending = Task { try await model.addConnection(name: "Repaired fixture", address: "https://pairing.example/api", token: "updated-fixture-token") }
        await wait(checker)
        checker.finish(snapshot)
        try await pending.value
        XCTAssertEqual(checker.count, 1)
        XCTAssertEqual(writes.requests, 0, "Pairing must not await a second initial-state refresh")
        XCTAssertEqual(writes.values.count, 1)
        XCTAssertEqual(writes.values[0].0, "updated-fixture-token")
        XCTAssertEqual(writes.values[0].1, saved.id)
        XCTAssertEqual(model.connections.count, 1)
        XCTAssertEqual(model.activeConnectionID, saved.id)
        XCTAssertEqual(model.connection?.name, "Repaired fixture")
        XCTAssertTrue(model.connected)
        XCTAssertEqual(model.chats.map(\.id), ["verified"])
        XCTAssertEqual(model.appVersion, "verified-fixture")
        XCTAssertTrue(model.lastUpdated != nil)
        XCTAssertEqual(try store.load(.chat(connectionID: saved.id, chatID: "draft-chat")), original)
    }

    func testInvalidSnapshotNeverWritesCredentialOrConnection() async throws
    {
        let checker = Checker(), writes = Writes(), (model, _) = try fixture(checker, writes)
        let pending = Task { try await model.addConnection(name: "Fixture", address: "https://pairing.example", token: "fixture-token") }
        await wait(checker)
        checker.finish(["chats": "invalid", "folders": []])
        do { try await pending.value; XCTFail("Invalid snapshot must fail") }
        catch { XCTAssertTrue(error.localizedDescription.contains("companion response")) }
        XCTAssertTrue(writes.values.isEmpty)
        XCTAssertTrue(model.connections.isEmpty)
    }

    func testChangedConnectionRejectsAnOlderPairingCompletion() async throws
    {
        let checker = Checker(), writes = Writes(), (model, _) = try fixture(checker, writes)
        let next = MobileConnection(id: "next-fixture", name: "Next", address: "https://next.example")
        model.connections = [next]
        let pending = Task { try await model.addConnection(name: "Old attempt", address: "https://pairing.example", token: "fixture-token") }
        await wait(checker)
        model.switchConnection(next.id)
        checker.finish(snapshot)
        do { try await pending.value; XCTFail("An old check must not take over a newer connection") }
        catch is CancellationError { }
        XCTAssertTrue(writes.values.isEmpty)
        XCTAssertEqual(model.connections, [next])
        XCTAssertEqual(model.activeConnectionID, next.id)
    }
}
