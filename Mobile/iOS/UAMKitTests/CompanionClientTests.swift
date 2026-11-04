import Foundation
import XCTest
@testable import UAMKit

private final class StubState: @unchecked Sendable
{
    private let lock = NSLock()
    private var handler: (@Sendable (URLRequest, any URLProtocolClient, URLProtocol) -> Void)?
    private var stop: (@Sendable () -> Void)?

    func set(_ handler: @escaping @Sendable (URLRequest, any URLProtocolClient, URLProtocol) -> Void, stop: (@Sendable () -> Void)? = nil)
    {
        lock.withLock { self.handler = handler; self.stop = stop }
    }
    func handle(_ request: URLRequest, client: any URLProtocolClient, protocol stub: URLProtocol)
    {
        let handler = lock.withLock { handler }
        handler?(request, client, stub)
    }
    func stopped() { let callback = lock.withLock { stop }; callback?() }
}

private final class StubURLProtocol: URLProtocol, @unchecked Sendable
{
    static let state = StubState()
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading()
    {
        guard let client else { return }
        Self.state.handle(request, client: client, protocol: self)
    }
    override func stopLoading() { Self.state.stopped() }
}

private final class RequestLog: @unchecked Sendable
{
    private let lock = NSLock()
    private var values: [JSONValue] = []
    func append(_ value: JSONValue) { lock.withLock { values.append(value) } }
    var requests: [JSONValue] { lock.withLock { values } }
}

private final class UploadBuffer: @unchecked Sendable
{
    private let lock = NSLock()
    private var bytes = Data()
    private var declared = 0
    func begin(_ total: Int) { lock.withLock { declared = total } }
    var total: Int { lock.withLock { declared } }
    var data: Data { lock.withLock { bytes } }
    func append(_ data: Data, at offset: Int) -> Int
    {
        lock.withLock
        {
            XCTAssertEqual(offset, bytes.count)
            bytes.append(data)
            return bytes.count
        }
    }
}

private final class PausedResponses: @unchecked Sendable
{
    private let lock = NSLock()
    private var pending: [String: (URLRequest, any URLProtocolClient, URLProtocol)] = [:]
    func hold(_ key: String, request: URLRequest, client: any URLProtocolClient, stub: URLProtocol)
    { lock.withLock { pending[key] = (request, client, stub) } }
    func reply(_ key: String, body: Data)
    {
        guard let (request, client, stub) = lock.withLock({ pending.removeValue(forKey: key) }) else
        { XCTFail("Missing held response"); return }
        client.urlProtocol(stub, didReceive: HTTPURLResponse(url: request.url!, statusCode: 200, httpVersion: nil, headerFields: nil)!, cacheStoragePolicy: .notAllowed)
        client.urlProtocol(stub, didLoad: body)
        client.urlProtocolDidFinishLoading(stub)
    }
}

final class CompanionClientTests: XCTestCase, @unchecked Sendable
{
    private func configuration() -> URLSessionConfiguration
    {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.protocolClasses = [StubURLProtocol.self]
        return configuration
    }
    private func client() throws -> CompanionClient
    {
        CompanionClient(endpoint: try CompanionEndpoint(baseURL: URL(string: "https://uam.example")!), token: "secret-token", configuration: configuration())
    }

    private static func reply(_ value: JSONValue, request: URLRequest, client: any URLProtocolClient, stub: URLProtocol, status: Int = 200)
    {
        let data = try! JSONEncoder().encode(value)
        client.urlProtocol(stub, didReceive: HTTPURLResponse(url: request.url!, statusCode: status, httpVersion: nil, headerFields: ["Content-Type": "application/json"])!, cacheStoragePolicy: .notAllowed)
        client.urlProtocol(stub, didLoad: data)
        client.urlProtocolDidFinishLoading(stub)
    }

    private static func envelope(_ request: URLRequest) -> JSONValue
    {
        var data = request.httpBody ?? Data()
        if let stream = request.httpBodyStream
        {
            stream.open()
            defer { stream.close() }
            var buffer = [UInt8](repeating: 0, count: 4096)
            while true
            {
                let count = stream.read(&buffer, maxLength: buffer.count)
                if count <= 0 { break }
                data.append(contentsOf: buffer.prefix(count))
            }
        }
        return try! JSONDecoder().decode(JSONValue.self, from: data)
    }

    private static func installUpload(log: RequestLog, buffer: UploadBuffer, corruptAck: Bool = false, failCommit: Bool = false)
    {
        StubURLProtocol.state.set { request, client, stub in
            let envelope = Self.envelope(request)
            log.append(envelope)
            let payload = envelope["payload"]!
            switch envelope.string("action")
            {
            case "getCompanionCapabilities":
                Self.reply(["protocolVersion": 1, "features": ["requestChunks": true], "limits": ["maxRequestBytes": 33554432, "requestChunkBytes": 32768]], request: request, client: client, stub: stub)
            case "beginCompanionRequestTransfer":
                buffer.begin(payload.int("totalBytes")!)
                Self.reply(["id": "upload", "totalBytes": .integer(Int64(buffer.total)), "chunkBytes": 32768, "nextOffset": 0], request: request, client: client, stub: stub)
            case "appendCompanionRequestChunk":
                let next = buffer.append(Data(base64Encoded: payload.string("base64")!)!, at: payload.int("offset")!)
                Self.reply(["id": "upload", "nextOffset": .integer(Int64(next + (corruptAck ? 1 : 0)))], request: request, client: client, stub: stub)
            case "commitCompanionRequestTransfer":
                if failCommit { client.urlProtocol(stub, didFailWithError: URLError(.networkConnectionLost)) }
                else { Self.reply(["ok": true, "data": ["accepted": true]], request: request, client: client, stub: stub) }
            default: XCTFail("Unexpected direct action during upload")
            }
        }
    }

    func testNegotiatedUploadReconstructsFullEnvelopeAndCommitsOnce() async throws
    {
        let log = RequestLog()
        let buffer = UploadBuffer()
        Self.installUpload(log: log, buffer: buffer)
        let prompt = String(repeating: "👋/", count: 16000)
        let result = try await client().request(action: "sendAcpPrompt", payload: ["chatId": "chat", "prompt": .string(prompt)])
        XCTAssertEqual(result["accepted"], true)
        XCTAssertEqual(buffer.data.count, buffer.total)
        let envelope = try CompanionProtocol.decode(buffer.data)
        XCTAssertEqual(envelope.string("action"), "sendAcpPrompt")
        XCTAssertEqual(envelope["payload"]?.string("prompt"), prompt)
        XCTAssertNotNil(envelope.string("requestId"))
        XCTAssertEqual(log.requests.first?.string("action"), "getCompanionCapabilities")
        XCTAssertEqual(log.requests.last?.string("action"), "commitCompanionRequestTransfer")
        XCTAssertEqual(log.requests.filter { $0.string("action") == "commitCompanionRequestTransfer" }.count, 1)
        XCTAssertEqual(log.requests.filter { $0.string("action") == "sendAcpPrompt" }.count, 0)
    }

    func testUploadStopsBeforeCommitOnInvalidAcknowledgement() async throws
    {
        let log = RequestLog()
        Self.installUpload(log: log, buffer: UploadBuffer(), corruptAck: true)
        do { _ = try await client().request(action: "sendAcpPrompt", payload: ["prompt": .string(String(repeating: "x", count: 70000))]); XCTFail("Expected invalid acknowledgement") }
        catch let error as CompanionError
        {
            guard case .invalidTransfer = error else { return XCTFail("Unexpected error: \(error)") }
        }
        XCTAssertEqual(log.requests.filter { $0.string("action") == "appendCompanionRequestChunk" }.count, 1)
        XCTAssertFalse(log.requests.contains { $0.string("action") == "commitCompanionRequestTransfer" })
    }

    func testLostCommitResponseNeverRetriesCommit() async throws
    {
        let log = RequestLog()
        Self.installUpload(log: log, buffer: UploadBuffer(), failCommit: true)
        do { _ = try await client().request(action: "sendAcpPrompt", payload: ["prompt": .string(String(repeating: "x", count: 70000))]); XCTFail("Expected transport failure") }
        catch let error as CompanionError { XCTAssertEqual(error, .network(code: URLError.networkConnectionLost.rawValue)) }
        XCTAssertEqual(log.requests.filter { $0.string("action") == "commitCompanionRequestTransfer" }.count, 1)
    }

    func testOldHostRejectsUploadBeforeOriginalActionIsSent() async throws
    {
        let log = RequestLog()
        StubURLProtocol.state.set { request, client, stub in
            log.append(Self.envelope(request))
            Self.reply(["error": "unavailable"], request: request, client: client, stub: stub, status: 403)
        }
        do { _ = try await client().request(action: "sendAcpPrompt", payload: ["prompt": .string(String(repeating: "x", count: 70000))]); XCTFail("Expected missing capability") }
        catch let error as CompanionError { XCTAssertEqual(error, .requestTransferUnavailable) }
        XCTAssertEqual(log.requests.map { $0.string("action") }, ["getCompanionCapabilities"])
    }

    func testPostEnvelopeBearerAndWrappedData() async throws
    {
        let log = RequestLog()
        StubURLProtocol.state.set { request, client, stub in
            XCTAssertEqual(request.url?.absoluteString, "https://uam.example/api")
            XCTAssertEqual(request.httpMethod, "POST")
            XCTAssertEqual(request.value(forHTTPHeaderField: "Authorization"), "Bearer secret-token")
            XCTAssertEqual(request.value(forHTTPHeaderField: "Cache-Control"), "no-store")
            log.append(Self.envelope(request))
            Self.reply(["ok": true, "data": ["future": ["nested": true]]], request: request, client: client, stub: stub)
        }
        let result = try await client().request(action: "getInitialState", payload: ["summaryOnly": true])
        XCTAssertEqual(result["future"]?["nested"], true)
        XCTAssertEqual(log.requests.first?["action"], "getInitialState")
        XCTAssertNotNil(log.requests.first?["requestId"]?.stringValue)
    }

    func testChunkedUTF8ResponseKeepsFinalChunkLast() async throws
    {
        let expected: JSONValue = ["ok": true, "data": ["content": "Hello 👋 café", "unknown": [1, 2]]]
        let bytes = try JSONEncoder().encode(expected)
        let log = RequestLog()
        StubURLProtocol.state.set { request, client, stub in
            let envelope = Self.envelope(request)
            log.append(envelope)
            if envelope.string("action") == "getInitialState"
            {
                Self.reply(["uamTransfer": ["id": "transfer", "totalBytes": .integer(Int64(bytes.count)), "chunkBytes": 3]], request: request, client: client, stub: stub)
            }
            else
            {
                let offset = envelope["payload"]!.int("offset")!
                let next = min(offset + 3, bytes.count)
                Self.reply(["base64": .string(bytes.subdata(in: offset..<next).base64EncodedString()), "nextOffset": .integer(Int64(next)), "done": .bool(next == bytes.count)], request: request, client: client, stub: stub)
            }
        }
        let result = try await client().request(action: "getInitialState", payload: [:])
        XCTAssertEqual(result, expected["data"])
        let offsets = log.requests.dropFirst().map { $0["payload"]!.int("offset")! }
        XCTAssertEqual(offsets, Array(stride(from: 0, to: bytes.count, by: 3)))
    }

    func testChunkFailureDoesNotRetryOriginalMutation() async throws
    {
        let log = RequestLog()
        StubURLProtocol.state.set { request, client, stub in
            let envelope = Self.envelope(request)
            log.append(envelope)
            let result: JSONValue = envelope.string("action") == "sendAcpPrompt"
                ? ["uamTransfer": ["id": "transfer", "totalBytes": 5, "chunkBytes": 3]]
                : ["base64": "YWJj", "nextOffset": 4, "done": false]
            Self.reply(result, request: request, client: client, stub: stub)
        }
        do
        {
            _ = try await client().request(action: "sendAcpPrompt", payload: ["chatId": "chat", "prompt": "hi"])
            XCTFail("Expected invalid chunk")
        }
        catch let error as CompanionError
        {
            guard case .invalidTransfer = error else { return XCTFail("Unexpected error: \(error)") }
        }
        XCTAssertEqual(log.requests.filter { $0.string("action") == "sendAcpPrompt" }.count, 1)
        XCTAssertEqual(log.requests.count, 2)
    }

    func testHTTPErrorDoesNotExposeServerSecrets() async throws
    {
        StubURLProtocol.state.set { request, client, stub in
            Self.reply(["error": "echo secret-token"], request: request, client: client, stub: stub, status: 401)
        }
        do { _ = try await client().request(action: "getInitialState", payload: [:]); XCTFail("Expected authentication failure") }
        catch let error as CompanionError
        {
            XCTAssertEqual(error, .http(status: 401))
            XCTAssertFalse(error.localizedDescription.contains("secret-token"))
        }
    }

    func testOversizedRequestNeverReachesTransport() async throws
    {
        let log = RequestLog()
        StubURLProtocol.state.set { request, client, stub in
            log.append(.null)
            Self.reply([:], request: request, client: client, stub: stub)
        }
        do { _ = try await client().request(action: "sendAcpPrompt", payload: ["prompt": .string(String(repeating: "x", count: CompanionProtocol.maximumRequestBytes))]); XCTFail("Expected size failure") }
        catch let error as CompanionError { XCTAssertEqual(error, .requestTooLarge) }
        XCTAssertTrue(log.requests.isEmpty)
    }

    func testStreamingLimitAppliesBeforeJSONDecoding() async throws
    {
        StubURLProtocol.state.set { request, client, stub in
            client.urlProtocol(stub, didReceive: HTTPURLResponse(url: request.url!, statusCode: 200, httpVersion: nil, headerFields: nil)!, cacheStoragePolicy: .notAllowed)
            client.urlProtocol(stub, didLoad: Data(repeating: 32, count: 5))
            client.urlProtocol(stub, didLoad: Data(repeating: 32, count: 5))
            client.urlProtocolDidFinishLoading(stub)
        }
        let operation = BoundedRequest(configuration: configuration(), maximumBytes: 8)
        do { _ = try await operation.perform(URLRequest(url: URL(string: "https://uam.example/api")!)); XCTFail("Expected streaming limit") }
        catch let error as CompanionError { XCTAssertEqual(error, .responseTooLarge) }
    }

    func testCancellationAndDisconnectStopInFlightRequest() async throws
    {
        let started = expectation(description: "request started")
        StubURLProtocol.state.set { _, _, _ in started.fulfill() }
        let client = try client()
        let task = Task { try await client.request(action: "getInitialState", payload: [:]) }
        await fulfillment(of: [started], timeout: 3)
        task.cancel()
        do { _ = try await task.value; XCTFail("Expected cancellation") }
        catch let error as CompanionError { XCTAssertEqual(error, .cancelled) }
        await client.disconnect()
        do { _ = try await client.request(action: "getInitialState", payload: [:]); XCTFail("Expected disconnected state") }
        catch let error as CompanionError { XCTAssertEqual(error, .cancelled) }
    }

    func testDisconnectCancelsAllConcurrentRequests() async throws
    {
        let started = expectation(description: "both requests started")
        started.expectedFulfillmentCount = 2
        StubURLProtocol.state.set { _, _, _ in started.fulfill() }
        let client = try client()
        let first = Task { try await client.request(action: "getInitialState", payload: [:]) }
        let second = Task { try await client.request(action: "getInitialState", payload: [:]) }
        await fulfillment(of: [started], timeout: 3)
        await client.disconnect()
        for task in [first, second]
        {
            do { _ = try await task.value; XCTFail("Expected disconnect cancellation") }
            catch let error as CompanionError { XCTAssertEqual(error, .cancelled) }
        }
    }

    func testRedirectDelegateRefusesCrossOriginAndDowngrade() async throws
    {
        let started = expectation(description: "request started")
        StubURLProtocol.state.set { _, _, _ in started.fulfill() }
        let operation = BoundedRequest(configuration: configuration(), maximumBytes: 8)
        let request = URLRequest(url: URL(string: "https://uam.example/api")!)
        let pending = Task { try await operation.perform(request) }
        await fulfillment(of: [started], timeout: 3)
        let session = URLSession(configuration: .ephemeral)
        defer { session.invalidateAndCancel() }
        operation.urlSession(session, task: session.dataTask(with: request), willPerformHTTPRedirection: HTTPURLResponse(url: request.url!, statusCode: 302, httpVersion: nil, headerFields: nil)!, newRequest: URLRequest(url: URL(string: "http://other.example/api")!)) { redirected in
            XCTAssertNil(redirected)
        }
        do { _ = try await pending.value; XCTFail("Expected redirect failure") }
        catch let error as CompanionError { XCTAssertEqual(error, .redirectForbidden) }
    }
    func testCancelledRequestDoesNotCancelConcurrentOrSubsequentRequest() async throws
    {
        let pending = PausedResponses(), started = expectation(description: "two independent requests started")
        started.expectedFulfillmentCount = 2
        StubURLProtocol.state.set { request, client, stub in
            let action = Self.envelope(request).string("action")!
            if action == "next" { Self.reply(["accepted": true], request: request, client: client, stub: stub); return }
            pending.hold(action, request: request, client: client, stub: stub)
            started.fulfill()
        }
        let client = try client()
        let cancelled = Task { try await client.request(action: "cancel-me") }
        let healthy = Task { try await client.request(action: "healthy") }
        await fulfillment(of: [started], timeout: 3)
        cancelled.cancel()
        do { _ = try await cancelled.value; XCTFail("Expected cancellation") }
        catch let error as CompanionError { XCTAssertEqual(error, .cancelled) }
        pending.reply("healthy", body: Data(#"{"accepted":true}"#.utf8))
        let result = try await healthy.value
        XCTAssertEqual(result["accepted"], true)
        cancelled.cancel() // An old cancellation must never affect the next task identifier.
        let next = try await client.request(action: "next")
        XCTAssertEqual(next["accepted"], true)
        await client.disconnect()
    }

    func testOversizedResponseDoesNotCancelHealthySiblingOrNextRequest() async throws
    {
        let pending = PausedResponses(), started = expectation(description: "two bounded requests started")
        started.expectedFulfillmentCount = 2
        StubURLProtocol.state.set { request, client, stub in
            pending.hold(request.url!.lastPathComponent, request: request, client: client, stub: stub)
            started.fulfill()
        }
        let transport = BoundedTransport(configuration: configuration())
        defer { transport.invalidate() }
        let overflow = BoundedRequest(transport: transport, maximumBytes: 8)
        let sibling = BoundedRequest(transport: transport, maximumBytes: 8)
        let oversized = Task { try await overflow.perform(URLRequest(url: URL(string: "https://uam.example/oversized")!)) }
        let healthy = Task { try await sibling.perform(URLRequest(url: URL(string: "https://uam.example/healthy")!)) }
        await fulfillment(of: [started], timeout: 3)
        pending.reply("oversized", body: Data(repeating: 32, count: 9))
        do { _ = try await oversized.value; XCTFail("Expected response bound") }
        catch let error as CompanionError { XCTAssertEqual(error, .responseTooLarge) }
        pending.reply("healthy", body: Data("ok".utf8))
        let (body, _) = try await healthy.value
        XCTAssertEqual(body, Data("ok".utf8))
        StubURLProtocol.state.set { request, client, stub in Self.reply([:], request: request, client: client, stub: stub) }
        let next = BoundedRequest(transport: transport, maximumBytes: 8)
        let (nextBody, _) = try await next.perform(URLRequest(url: URL(string: "https://uam.example/next")!))
        XCTAssertEqual(nextBody, Data("{}".utf8))
    }

    func testCompletedTransportAndClientAreReleasedWithoutExplicitDisconnect() async throws
    {
        StubURLProtocol.state.set { request, client, stub in Self.reply([:], request: request, client: client, stub: stub) }
        weak var releasedTransport: BoundedTransport?
        var transport: BoundedTransport? = BoundedTransport(configuration: configuration())
        releasedTransport = transport
        var operation: BoundedRequest? = BoundedRequest(transport: transport!, maximumBytes: 8)
        _ = try await operation!.perform(URLRequest(url: URL(string: "https://uam.example/api")!))
        operation = nil; transport = nil
        XCTAssertNil(releasedTransport)
        weak var releasedClient: CompanionClient?
        var client: CompanionClient? = try client()
        releasedClient = client
        _ = try await client!.request(action: "fixture")
        client = nil
        XCTAssertNil(releasedClient)
    }

}
