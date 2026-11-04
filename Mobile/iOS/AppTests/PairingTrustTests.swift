import XCTest
import UAMKit
@testable import UAM

@MainActor final class PairingTrustTests: XCTestCase
{
    private let root = "MIIC+jCCAeKgAwIBAgIUJvOfWpi2eJuY/0OyPGrmfxEyKsEwDQYJKoZIhvcNAQELBQAwIzEhMB8GA1UEAwwYRGlzcG9zYWJsZSBVQU0gVGVzdCBSb290MB4XDTIwMDEwMTAwMDAwMFoXDTQwMDEwMTAwMDAwMFowIzEhMB8GA1UEAwwYRGlzcG9zYWJsZSBVQU0gVGVzdCBSb290MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEArSbv9d80hcK/TE5FMEsg3BLRg0BsGZ+2QAw3jETfMF8mHshe9aQlKGJ+JrxLmasXJKQBOD6hybZikYatTKLus5Ib1pWG5S/Po4ROkhTjd2+2icvdJyCJ7cnlR+A1X06N1tw0b/7l/lrA2h9f4qDzwMAI/QoceJpjiuLS/1SO/84YiIEcHQO7Hgn3FitotMwzcV57y2DLulLAALnCQJ1/tnSsMlCspQnGGqiJqVwQKG7B2wAPUH7EPEyyJcPPGV/V91d0zggTdjRYYCX08DqEL6VgA3X4y6GteDHTlwOu17y5NQbBZ2WW0Iejzyy+BjG7g6tQkqpC8wbpVcEry+AacQIDAQABoyYwJDASBgNVHRMBAf8ECDAGAQH/AgEBMA4GA1UdDwEB/wQEAwIBBjANBgkqhkiG9w0BAQsFAAOCAQEAkXeMIh1pMAzk7HMEMT3JPbPB2iWlSV9jee3HQXOuP3yYUURerLwVNm6ihW+qgMYrjE8rxRFy+bxFyq4C6sndrJw+3p8T54zXwKpJS74plD4/XN3bGKXNqhrvKQU6pIFw/eJZv3/2A+eg/7rp83ALI7Zz/5NMlsoNWshzSAeFAcN2kat7qvamqUenZZtliVH8qBmOH0evWpc/HkOfEM/7A46BfCkvxnnaGuU1vqKk6jBDXS7dd82IQCjfwwPGdfaEWeu1YyqlU7lnNnctW3Ip1hMliM9TyRGJ+K7t4RDLNMY+zxp0qR7EZso+ku+VWkiKCj55zwuJykJM1hmtVhPF8g=="

    func testQRCodeCertificateIsPreservedOnlyForItsOriginalHTTPSOrigin() async throws
    {
        let flow = PairingFlow()
        var submitted: [PairingDetails] = []
        let check: PairingFlow.Check = { submitted.append($0); throw CompanionError.network(code: -1001) }
        let json: WireObject = ["type": "uam-companion", "version": 1, "url": "https://paired.example:8443/companion", "token": "fixture", "certificate": root]
        let code = try XCTUnwrap(String(data: JSONSerialization.data(withJSONObject: json), encoding: .utf8))
        XCTAssertEqual(try PairingCode(code).certificate, root)
        XCTAssertTrue(flow.acceptCode(code, using: check))
        for _ in 0..<1000 { if flow.phase != .checking { break }; await Task.yield() }
        XCTAssertEqual(submitted.count, 1)
        XCTAssertEqual(submitted[0].pairingCertificate, root)
        flow.editDetails(); flow.address = "https://PAIRED.example:8443/api"
        flow.submit(using: check)
        for _ in 0..<1000 { if flow.phase != .checking { break }; await Task.yield() }
        XCTAssertEqual(submitted.count, 2)
        XCTAssertEqual(submitted[1].pairingCertificate, root)
        flow.editDetails(); flow.address = "https://other.example:8443"
        flow.submit(using: check)
        for _ in 0..<1000 { if flow.phase != .checking { break }; await Task.yield() }
        XCTAssertEqual(submitted.count, 3)
        XCTAssertNil(submitted[2].pairingCertificate)
    }

    func testInvalidQRCodeCertificateCannotFallBackToImplicitTrust() throws
    {
        for value: Any in ["not-base64", 42, Data(repeating: 1, count: 8193).base64EncodedString()]
        {
            let json: WireObject = ["type": "uam-companion", "version": 1, "url": "https://paired.example", "token": "fixture", "certificate": value]
            let code = try XCTUnwrap(String(data: JSONSerialization.data(withJSONObject: json), encoding: .utf8))
            XCTAssertThrowsError(try PairingCode(code))
        }
    }

    func testSavedConnectionsDecodeWithoutNewFieldsAndRoundTripPublicAnchor() throws
    {
        let old = try JSONDecoder().decode(MobileConnection.self, from: Data(#"{"id":"retained","name":"Old","address":"https://paired.example"}"#.utf8))
        XCTAssertEqual(old.id, "retained")
        XCTAssertEqual(old.routes, [])
        XCTAssertNil(old.pairingCertificate)
        var paired = try MobileConnection.paired(name: "Repaired", address: XCTUnwrap(URL(string: "https://paired.example/api")), saved: [old], preferredID: old.id)
        paired.pairingCertificate = root
        let saved = try JSONDecoder().decode(MobileConnection.self, from: JSONEncoder().encode(paired))
        XCTAssertEqual(saved.pairingCertificate, root)
        XCTAssertEqual(saved.id, old.id)
        let same = try MobileConnection.paired(name: "Same origin", address: XCTUnwrap(URL(string: "https://PAIRED.example:443/companion")), saved: [saved], preferredID: saved.id)
        XCTAssertEqual(same.pairingCertificate, root)
        let other = try MobileConnection.paired(name: "Other origin", address: XCTUnwrap(URL(string: "https://other.example")), saved: [saved], preferredID: saved.id)
        XCTAssertNil(other.pairingCertificate)
    }
}
