import XCTest
@testable import UAM

final class PairingAndActivityTests: XCTestCase
{
    func testRepairPairingRetainsDraftIdentityWithoutCrossingOrigins() throws
    {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = DraftStore(directory: directory)
        let saved = MobileConnection(id: "retained", name: "Computer", address: "https://MAIN.example:443/companion/")
        let draft = ComposerDraft(text: "Recover this unsent draft", attachments: [.init(name: "original", mimeType: "application/octet-stream", data: Data([1, 2, 3]))])
        try store.save(draft, in: .chat(connectionID: saved.id, chatID: "chat"))
        let repaired = try MobileConnection.paired(name: "Repaired", address: XCTUnwrap(URL(string: "https://main.example/api")), saved: [saved], preferredID: saved.id)
        XCTAssertEqual(repaired.id, saved.id)
        XCTAssertEqual(try store.load(.chat(connectionID: repaired.id, chatID: "chat")), draft)
        let other = try MobileConnection.paired(name: "Computer", address: XCTUnwrap(URL(string: "https://other.example")), saved: [saved], preferredID: saved.id)
        XCTAssertNotEqual(other.id, saved.id)
        XCTAssertTrue(try store.load(.chat(connectionID: other.id, chatID: "chat")).text.isEmpty)
        XCTAssertThrowsError(try MobileConnection.paired(name: "Unsafe", address: XCTUnwrap(URL(string: "http://main.example")), saved: [saved], preferredID: nil))
    }

    func testDesktopQRCodePreservesExactCredentialAndHost()
    {
        let code = #"{"type":"uam-companion","version":1,"url":"https://main.example:58950","token":"fixture-Credential_123","name":"My Mac"}"#
        let pairing = try? PairingCode(code)
        XCTAssertEqual(pairing?.address, "https://main.example:58950")
        XCTAssertEqual(pairing?.token, "fixture-Credential_123")
        XCTAssertEqual(pairing?.name, "My Mac")
    }

    func testPairingRejectsUntrustedFormatsAndCleartext()
    {
        for code in [
            #"{"type":"uam-companion","version":2,"url":"https://main.example","token":"fixture"}"#,
            #"{"type":"uam-companion","version":true,"url":"https://main.example","token":"fixture"}"#,
            #"{"type":"uam-companion","version":1,"url":"http://main.example","token":"fixture"}"#,
            "uam://pair?url=https%3A%2F%2Fmain.example&token=fixture&token=other",
            "uam://pair?url=https%3A%2F%2Fmain.example&token=has%20spaces",
            "uam://pair?url=https%3A%2F%2Fmain.example%2Fother&token=fixture",
            String(repeating: "x", count: 16385)
        ] { XCTAssertThrowsError(try PairingCode(code)) }
    }

    func testURLPairingDecodesFieldsWithoutChangingCase()
    {
        let pairing = try? PairingCode("uam://pair?url=https%3A%2F%2Fmain.example&token=Exact_Case-123&name=Work%20Mac")
        XCTAssertEqual(pairing?.token, "Exact_Case-123")
        XCTAssertEqual(pairing?.name, "Work Mac")
    }

    func testActiveFamilyShowsRootAndOpensMemberNeedingInput()
    {
        let root = MobileChat(raw: ["id": "root", "title": "Root", "attentionRevision": "reply"])
        let working = MobileChat(raw: ["id": "working", "branchRootChatId": "root", "acpSession": ["processing": true]])
        let question = MobileChat(raw: ["id": "question", "branchRootChatId": "root", "acpSession": ["pendingUserInput": ["requestId": "request"]]])
        let groups = MobileActivityGroup.build([root, working, question])
        XCTAssertEqual(groups.count, 1)
        XCTAssertEqual(groups.first?.root.id, "root")
        XCTAssertEqual(groups.first?.destination.id, "question")
        XCTAssertEqual(groups.first?.members.count, 3)
    }

    func testAcknowledgedAttentionOverridesLegacyRuntimeFlag()
    {
        let chat = MobileChat(raw: ["id": "read", "attentionRevision": "", "acpSession": ["readySinceLastSelect": true]])
        XCTAssertFalse(chat.unseen)
        XCTAssertTrue(MobileActivityGroup.build([chat]).isEmpty)
    }
}
