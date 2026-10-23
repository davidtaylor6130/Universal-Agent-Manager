import Foundation
import XCTest
@testable import UAMKit

final class ProtocolTests: XCTestCase
{
    func testEndpointNormalizationAndCredentialRejection() throws
    {
        for path in ["", "/", "/companion", "/companion/", "/api"]
        {
            XCTAssertEqual(try CompanionEndpoint(baseURL: URL(string: "https://uam.example:443\(path)")!).apiURL.path, "/api")
        }
        for url in ["https://token@uam.example", "https://uam.example?token=secret", "https://uam.example/#secret", "https://uam.example/other", "https://uam.example/%61pi", "https://uam.example:0", "file:///tmp/uam"]
        {
            XCTAssertThrowsError(try CompanionEndpoint(baseURL: URL(string: url)!))
        }
        XCTAssertThrowsError(try CompanionEndpoint(baseURL: URL(string: "http://uam.example")!, allowLoopbackHTTP: true))
        XCTAssertThrowsError(try CompanionEndpoint(baseURL: URL(string: "http://localhost")!))
        #if DEBUG
        for host in ["localhost", "127.0.0.1", "[::1]"]
        {
            XCTAssertEqual(try CompanionEndpoint(baseURL: URL(string: "http://\(host):8080")!, allowLoopbackHTTP: true).apiURL.scheme, "http")
        }
        #else
        XCTAssertThrowsError(try CompanionEndpoint(baseURL: URL(string: "http://localhost")!, allowLoopbackHTTP: true))
        #endif
    }

    func testRawAndWrappedResponses() throws
    {
        let raw: JSONValue = ["chats": [], "extra": "preserved"]
        XCTAssertEqual(try CompanionProtocol.unwrap(raw), raw)
        XCTAssertEqual(try CompanionProtocol.unwrap(["ok": true, "data": raw]), raw)
        XCTAssertEqual(try CompanionProtocol.unwrap(["ok": true]), .null)
        XCTAssertEqual(try CompanionProtocol.unwrap(["ok": "provider-value"]), ["ok": "provider-value"])
        XCTAssertThrowsError(try CompanionProtocol.unwrap(["ok": false, "error": "token-secret"])) { error in
            XCTAssertEqual(error as? CompanionError, .serverRejected)
            XCTAssertFalse(error.localizedDescription.contains("token-secret"))
        }
        XCTAssertThrowsError(try CompanionProtocol.decode(Data([0xFF])))
        XCTAssertThrowsError(try CompanionProtocol.decode(Data("<html>".utf8)))
    }

    func testDescriptorBounds() throws
    {
        for json: JSONValue in [
            ["id": "", "totalBytes": 10], ["id": "x", "totalBytes": 0],
            ["id": "x", "totalBytes": 1.5], ["id": "x", "totalBytes": true],
            ["id": "x", "totalBytes": 10, "chunkBytes": .null],
            ["id": "x", "totalBytes": 10, "chunkBytes": 0],
            ["id": "x", "totalBytes": 10, "chunkBytes": 131073],
            ["id": "x", "totalBytes": 5000, "chunkBytes": 1]
        ] { XCTAssertThrowsError(try CompanionTransfer(json: json)) }
        XCTAssertThrowsError(try CompanionTransfer(json: ["id": "x", "totalBytes": .integer(Int64(CompanionProtocol.maximumDecodedBytes + 1))])) { error in
            XCTAssertEqual(error as? CompanionError, .responseTooLarge)
        }
        let maximum = try CompanionTransfer(json: ["id": "x", "totalBytes": .integer(Int64(CompanionProtocol.maximumDecodedBytes))])
        XCTAssertEqual(maximum.chunkBytes, 128 * 1024)
    }

    func testChunkLengthsOffsetsFinalMarkerAndStrictBase64() throws
    {
        let transfer = try CompanionTransfer(json: ["id": "x", "totalBytes": 5, "chunkBytes": 3])
        XCTAssertEqual(try transfer.decodeChunk(["base64": "YWJj", "nextOffset": 3, "done": false], at: 0), Data("abc".utf8))
        XCTAssertEqual(try transfer.decodeChunk(["base64": "ZGU=", "nextOffset": 5, "done": true], at: 3), Data("de".utf8))
        for chunk: JSONValue in [
            ["base64": "YWI=", "nextOffset": 2, "done": false],
            ["base64": "YWJj", "nextOffset": 4, "done": false],
            ["base64": "YWJj", "nextOffset": 3, "done": true],
            ["base64": "!!!!", "nextOffset": 3, "done": false],
            ["base64": "YWJj", "nextOffset": true, "done": false]
        ] { XCTAssertThrowsError(try transfer.decodeChunk(chunk, at: 0)) }
        XCTAssertThrowsError(try transfer.decodeChunk(["base64": "ZGV=", "nextOffset": 5, "done": true], at: 3))
        XCTAssertThrowsError(try transfer.decodeChunk(["base64": "ZGU=", "nextOffset": 5, "done": false], at: 3))
        XCTAssertThrowsError(try transfer.decodeChunk([:], at: 1))
        XCTAssertThrowsError(try transfer.decodeChunk([:], at: 5))
    }
}
