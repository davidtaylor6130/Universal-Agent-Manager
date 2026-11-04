import XCTest
import UAMKit
@testable import UAM

@MainActor final class PairingFollowupTests: XCTestCase
{
    private let oldRoot = "MIIC+jCCAeKgAwIBAgIUJvOfWpi2eJuY/0OyPGrmfxEyKsEwDQYJKoZIhvcNAQELBQAwIzEhMB8GA1UEAwwYRGlzcG9zYWJsZSBVQU0gVGVzdCBSb290MB4XDTIwMDEwMTAwMDAwMFoXDTQwMDEwMTAwMDAwMFowIzEhMB8GA1UEAwwYRGlzcG9zYWJsZSBVQU0gVGVzdCBSb290MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEArSbv9d80hcK/TE5FMEsg3BLRg0BsGZ+2QAw3jETfMF8mHshe9aQlKGJ+JrxLmasXJKQBOD6hybZikYatTKLus5Ib1pWG5S/Po4ROkhTjd2+2icvdJyCJ7cnlR+A1X06N1tw0b/7l/lrA2h9f4qDzwMAI/QoceJpjiuLS/1SO/84YiIEcHQO7Hgn3FitotMwzcV57y2DLulLAALnCQJ1/tnSsMlCspQnGGqiJqVwQKG7B2wAPUH7EPEyyJcPPGV/V91d0zggTdjRYYCX08DqEL6VgA3X4y6GteDHTlwOu17y5NQbBZ2WW0Iejzyy+BjG7g6tQkqpC8wbpVcEry+AacQIDAQABoyYwJDASBgNVHRMBAf8ECDAGAQH/AgEBMA4GA1UdDwEB/wQEAwIBBjANBgkqhkiG9w0BAQsFAAOCAQEAkXeMIh1pMAzk7HMEMT3JPbPB2iWlSV9jee3HQXOuP3yYUURerLwVNm6ihW+qgMYrjE8rxRFy+bxFyq4C6sndrJw+3p8T54zXwKpJS74plD4/XN3bGKXNqhrvKQU6pIFw/eJZv3/2A+eg/7rp83ALI7Zz/5NMlsoNWshzSAeFAcN2kat7qvamqUenZZtliVH8qBmOH0evWpc/HkOfEM/7A46BfCkvxnnaGuU1vqKk6jBDXS7dd82IQCjfwwPGdfaEWeu1YyqlU7lnNnctW3Ip1hMliM9TyRGJ+K7t4RDLNMY+zxp0qR7EZso+ku+VWkiKCj55zwuJykJM1hmtVhPF8g=="

    private func waitForResult(_ flow: PairingFlow) async
    {
        for _ in 0..<1000 { if flow.phase != .checking { return }; await Task.yield() }
        XCTFail("Expected a pairing result")
    }

    func testLocalPairingParseMessagesRemainSpecificWithoutEchoingUnknownMessages()
    {
        let cases = [
            ("This is not a UAM pairing code.", "Not a Universal Agent Manager pairing code"),
            ("The pairing code is missing its address or token.", "Pairing code is incomplete"),
            ("The pairing code contains duplicate fields.", "Pairing code has duplicate fields"),
            ("This pairing code is too large.", "Pairing code is too large")
        ]
        for (message, title) in cases { XCTAssertEqual(PairingProblem(MobileError.message(message)).title, title) }
        let unknown = PairingProblem(MobileError.message("unknown-token-fixture-do-not-echo"))
        XCTAssertFalse(unknown.title.contains("unknown-token-fixture"))
        XCTAssertFalse(unknown.message.contains("unknown-token-fixture"))
    }

    func testGenerationCancellationHasContextualRetryGuidance()
    {
        let problem = PairingProblem(CancellationError())
        XCTAssertEqual(problem.title, "Connection check cancelled")
        XCTAssertTrue(problem.message.contains("new connection check"))
        XCTAssertFalse(problem.message.localizedCaseInsensitiveContains("chat"))
    }

    func testQRCodeAndClientRejectTheSameNonASCIIOrOversizedTokens() throws
    {
        for token in ["has space", "with\nnewline", "é", "emoji-🦊", String(repeating: "a", count: 8193)]
        {
            let json: WireObject = ["type": "uam-companion", "version": 1, "url": "https://paired.example", "token": token]
            let code = try XCTUnwrap(String(data: JSONSerialization.data(withJSONObject: json), encoding: .utf8))
            XCTAssertThrowsError(try PairingCode(code))
            XCTAssertThrowsError(try CompanionClient(baseURL: XCTUnwrap(URL(string: "https://paired.example")), token: token))
        }
        let valid = String(repeating: "~", count: 8192)
        let json: WireObject = ["type": "uam-companion", "version": 1, "url": "https://paired.example", "token": valid]
        let code = try XCTUnwrap(String(data: JSONSerialization.data(withJSONObject: json), encoding: .utf8))
        XCTAssertEqual(try PairingCode(code).token, valid)
    }

    func testCapturedManualNameIsTrimmedAndCappedLikeScannedName() async
    {
        let flow = PairingFlow()
        flow.name = " \n" + String(repeating: "n", count: 180) + " \n"
        flow.address = "https://paired.example"; flow.token = "fixture"
        var captured: PairingDetails?
        flow.submit { captured = $0 }
        await waitForResult(flow)
        XCTAssertEqual(captured?.name, String(repeating: "n", count: 160))
        XCTAssertEqual(flow.checkedName, String(repeating: "n", count: 160))
    }
    func testFreshQRCodeWithoutCertificateReplacesTrustButEditedOriginKeepsItsOwnChoice() async throws
    {
        let flow = PairingFlow()
        var submitted: [PairingDetails] = []
        let check: PairingFlow.Check = { submitted.append($0); throw CompanionError.network(code: -1001) }
        let code = #"{"type":"uam-companion","version":1,"url":"https://paired.example","token":"fixture"}"#
        XCTAssertTrue(flow.acceptCode(code, using: check))
        await waitForResult(flow)
        XCTAssertTrue(submitted[0].clearSavedCertificate)
        XCTAssertNil(submitted[0].pairingCertificate)
        flow.editDetails(); flow.address = "https://PAIRED.example:443/api"
        flow.submit(using: check)
        await waitForResult(flow)
        XCTAssertTrue(submitted[1].clearSavedCertificate)
        flow.editDetails(); flow.address = "https://another.example"
        flow.submit(using: check)
        await waitForResult(flow)
        XCTAssertFalse(submitted[2].clearSavedCertificate)
        XCTAssertNil(submitted[2].pairingCertificate)
        let manual = PairingDetails(name: "Manual", address: "https://paired.example", token: "fixture")
        XCTAssertFalse(manual.clearSavedCertificate)
    }

    func testFreshQRCodeCanClearSavedAnchorBeforeCandidateConstruction() async throws
    {
        let suite = "uam-pairing-clear-root-" + UUID().uuidString
        let preferences = try XCTUnwrap(UserDefaults(suiteName: suite))
        let store = DraftStore(directory: FileManager.default.temporaryDirectory.appendingPathComponent(suite))
        addTeardownBlock { UserDefaults(suiteName: suite)?.removePersistentDomain(forName: suite); try? FileManager.default.removeItem(at: store.directory) }
        var checks = 0, writes = 0
        let model = MobileAppModel(preferences: preferences, draftStore: store, requestHandler: { _, _ in [:] },
                                   pairingRequestHandler: { _ in checks += 1; return ["chats": [], "folders": []] }, credentialWriter: { _, _ in writes += 1 })
        model.setForeground(false)
        for (index, certificate) in [oldRoot, "expired-corrupt-public-root-fixture"].enumerated()
        {
            let saved = MobileConnection(id: "retained-origin", name: "Old", address: "https://paired.example", pairingCertificate: certificate)
            model.connections = [saved]; model.activeConnectionID = saved.id
            // The corrupt variant also proves clearing precedes candidate creation:
            // selecting it would fail before the injected network check is reached.
            try await model.addConnection(name: "  Repaired  ", address: "https://paired.example/api", token: "fixture", clearSavedCertificate: true)
            XCTAssertEqual(checks, index + 1); XCTAssertEqual(writes, index + 1)
            XCTAssertEqual(model.activeConnectionID, saved.id)
            XCTAssertNil(model.connection?.pairingCertificate)
            XCTAssertEqual(model.connection?.name, "Repaired")
        }
    }

    func testScannedNameIsAlsoTrimmedBeforeTheSame160CharacterLimit() throws
    {
        let name = "  " + String(repeating: "n", count: 180) + "  "
        let json: WireObject = ["type": "uam-companion", "version": 1, "url": "https://paired.example", "token": "fixture", "name": name]
        let code = try XCTUnwrap(String(data: JSONSerialization.data(withJSONObject: json), encoding: .utf8))
        XCTAssertEqual(try PairingCode(code).name, String(repeating: "n", count: 160))
    }

    #if os(iOS)
    func testScannerStartupFailureReportsOnceInsteadOfLeavingAnEmptyScanner() async
    {
        var failures = 0
        let coordinator = PairCodeScanner.Coordinator(onCode: { _ in XCTFail("Failed scanner must not deliver a code") }, onError: { failures += 1 })
        coordinator.scannerFailed(); coordinator.scannerFailed()
        for _ in 0..<1000 { if failures > 0 { break }; await Task.yield() }
        XCTAssertEqual(failures, 1)
    }
    #endif

}
