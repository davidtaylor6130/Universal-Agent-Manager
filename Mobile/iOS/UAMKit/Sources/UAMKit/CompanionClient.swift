import Foundation

/// Own one client per connection. Tokens are memory-only; callers own Keychain persistence.
/// No action is retried automatically, including after cancellation or partial transfers.
public actor CompanionClient
{
    public nonisolated let endpoint: CompanionEndpoint
    private var token: String
    private let transport: BoundedTransport
    private var generation: UInt64 = 0
    private var disconnected = false
    private var active: [UUID: BoundedRequest] = [:]

    public init(baseURL: URL, token: String, allowLoopbackHTTP: Bool = false, pairingCertificate: String? = nil) throws
    {
        endpoint = try CompanionEndpoint(baseURL: baseURL, allowLoopbackHTTP: allowLoopbackHTTP)
        guard CompanionProtocol.isValidAccessToken(token) else { throw CompanionError.invalidToken }
        self.token = token
        let apiURL = endpoint.apiURL
        let trust = try pairingCertificate.map { try CompanionTrust(baseURL: apiURL, certificate: $0) }
        transport = BoundedTransport(configuration: Self.secureConfiguration(), trust: trust)
    }

    // Test-only transport injection; production callers cannot install an insecure trust delegate.
    init(endpoint: CompanionEndpoint, token: String, configuration: URLSessionConfiguration)
    {
        self.endpoint = endpoint
        self.token = token
        transport = BoundedTransport(configuration: configuration)
    }

    deinit { transport.invalidate() }

    private static func secureConfiguration() -> URLSessionConfiguration
    {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.urlCache = nil
        configuration.urlCredentialStorage = nil
        configuration.httpCookieStorage = nil
        configuration.httpShouldSetCookies = false
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        configuration.timeoutIntervalForRequest = 30
        configuration.timeoutIntervalForResource = 60
        configuration.tlsMinimumSupportedProtocolVersion = .TLSv12
        return configuration
    }

    /// Cancels every in-flight operation and prevents further requests with this connection/token.
    public func disconnect()
    {
        disconnected = true
        token = ""
        generation &+= 1
        for request in active.values { request.cancel() }
        active.removeAll()
        transport.invalidate()
    }

    public func request(action: String, payload: JSONValue = .object([:])) async throws -> JSONValue
    {
        guard !Task.isCancelled else { throw CompanionError.cancelled }
        guard !disconnected else { throw CompanionError.cancelled }
        guard !action.isEmpty, action.utf8.count <= 256, payload.objectValue != nil else
        {
            throw CompanionError.invalidRequest("Provide an action and an object payload.")
        }
        let version = generation
        let envelope: JSONValue = ["action": .string(action), "payload": payload, "requestId": .string(UUID().uuidString)]
        var result = try await sendEnvelope(envelope, version: version)
        if let descriptor = result["uamTransfer"]
        {
            let transfer = try CompanionTransfer(json: descriptor)
            var bytes = Data()
            bytes.reserveCapacity(transfer.totalBytes)
            // Sequential loading keeps the destructive final chunk strictly last.
            while bytes.count < transfer.totalBytes
            {
                try checkConnection(version)
                let chunk = try await post(["action": "getCompanionResponseChunk", "payload": ["id": .string(transfer.id), "offset": .integer(Int64(bytes.count))]])
                bytes.append(try transfer.decodeChunk(chunk, at: bytes.count))
            }
            try checkConnection(version)
            result = try CompanionProtocol.decode(bytes)
            guard result["uamTransfer"] == nil else { throw CompanionError.invalidTransfer("Nested response transfers are unsupported.") }
        }
        try checkConnection(version)
        return try CompanionProtocol.unwrap(result)
    }

    public func initialState() async throws -> InitialState
    {
        try InitialState(json: await request(action: "getInitialState", payload: ["summaryOnly": true]))
    }

    public func messages(_ query: ChatMessagesRequest) async throws -> ChatMessagesPage
    {
        let page = try ChatMessagesPage(json: await request(action: "getChatMessages", payload: query.payload))
        guard page.chatId == query.chatId else { throw CompanionError.invalidResponse("UAM returned a page for a different chat.") }
        if !page.unchanged
        {
            guard let messages = page.messages, let start = page.startIndex, let total = page.totalCount,
                  messages.count <= query.limit, start + messages.count == min(query.before ?? total, total),
                  start == max(0, min(query.before ?? total, total) - query.limit) else
            {
                throw CompanionError.invalidResponse("UAM returned an invalid message page window.")
            }
        }
        return page
    }

    private func checkConnection(_ expected: UInt64) throws
    {
        guard !disconnected, generation == expected, !Task.isCancelled else { throw CompanionError.cancelled }
    }

    private func encode(_ envelope: JSONValue) throws -> Data
    {
        do
        {
            let encoder = JSONEncoder()
            // Base64 must fit in a 64 KiB wire envelope even when it contains many slashes.
            encoder.outputFormatting = [.withoutEscapingSlashes]
            return try encoder.encode(envelope)
        }
        catch { throw CompanionError.invalidRequest("The request contains a value that cannot be encoded as JSON.") }
    }

    private func sendEnvelope(_ envelope: JSONValue, version: UInt64) async throws -> JSONValue
    {
        let body = try encode(envelope)
        guard body.count <= CompanionProtocol.maximumRequestBytes else { throw CompanionError.requestTooLarge }
        if body.count <= CompanionProtocol.directRequestBytes { return try await postData(body) }
        guard !["beginCompanionRequestTransfer", "appendCompanionRequestChunk", "commitCompanionRequestTransfer"].contains(envelope.string("action") ?? "") else
        {
            throw CompanionError.invalidRequest("Nested request transfers are unsupported.")
        }
        let capabilities: JSONValue
        do { capabilities = try CompanionProtocol.unwrap(await post(["action": "getCompanionCapabilities", "payload": [:]])) }
        catch CompanionError.http(let status) where status == 403 || status == 404
        {
            throw CompanionError.requestTransferUnavailable
        }
        guard capabilities.int("protocolVersion") == 1,
              capabilities["features"]?.bool("requestChunks") == true,
              let maximum = capabilities["limits"]?.int("maxRequestBytes"), maximum > 0,
              let chunkBytes = capabilities["limits"]?.int("requestChunkBytes"),
              (1...CompanionProtocol.maximumRequestChunkBytes).contains(chunkBytes),
              (body.count - 1) / chunkBytes + 1 <= CompanionProtocol.maximumTransferChunks else
        {
            throw CompanionError.requestTransferUnavailable
        }
        guard body.count <= maximum else { throw CompanionError.requestTooLarge }
        try checkConnection(version)
        let transfer = try CompanionProtocol.unwrap(await post(["action": "beginCompanionRequestTransfer", "payload": ["totalBytes": .integer(Int64(body.count))]]))
        guard let id = transfer.string("id"), !id.isEmpty, id.utf8.count <= 128,
              transfer.int("totalBytes") == body.count, transfer.int("nextOffset") == 0,
              transfer.int("chunkBytes") == chunkBytes else
        {
            throw CompanionError.invalidTransfer("UAM returned an invalid request upload descriptor.")
        }
        var offset = 0
        while offset < body.count
        {
            try checkConnection(version)
            let next = min(offset + chunkBytes, body.count)
            let chunk = body.subdata(in: offset..<next).base64EncodedString()
            let acknowledgement = try CompanionProtocol.unwrap(await post(["action": "appendCompanionRequestChunk", "payload": ["id": .string(id), "offset": .integer(Int64(offset)), "base64": .string(chunk)]]))
            guard acknowledgement.string("id") == id, acknowledgement.int("nextOffset") == next else
            {
                throw CompanionError.invalidTransfer("UAM returned an invalid request upload acknowledgement. The action was not committed.")
            }
            offset = next
        }
        try checkConnection(version)
        // Exactly one commit attempt; a lost response never authorizes replaying an action.
        return try await post(["action": "commitCompanionRequestTransfer", "payload": ["id": .string(id)]])
    }

    private func post(_ envelope: JSONValue) async throws -> JSONValue
    {
        guard !disconnected, !Task.isCancelled else { throw CompanionError.cancelled }
        return try await postData(encode(envelope))
    }

    private func postData(_ body: Data) async throws -> JSONValue
    {
        guard !disconnected, !Task.isCancelled else { throw CompanionError.cancelled }
        guard body.count <= CompanionProtocol.maximumWireRequestBytes else { throw CompanionError.requestTooLarge }
        var request = URLRequest(url: endpoint.apiURL)
        request.httpMethod = "POST"
        request.httpBody = body
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        request.setValue("no-store", forHTTPHeaderField: "Cache-Control")
        let id = UUID()
        let operation = BoundedRequest(transport: transport, maximumBytes: CompanionProtocol.maximumDecodedBytes)
        active[id] = operation
        defer { active[id] = nil }
        let (data, response) = try await operation.perform(request)
        guard (200...299).contains(response.statusCode) else { throw CompanionError.http(status: response.statusCode) }
        return try CompanionProtocol.decode(data)
    }
}
