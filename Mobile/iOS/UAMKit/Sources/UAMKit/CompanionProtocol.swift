import Foundation

public enum CompanionError: Error, Sendable, Equatable, LocalizedError
{
    case invalidEndpoint
    case insecureEndpoint
    case invalidToken
    case invalidPairingCertificate
    case invalidRequest(String)
    case requestTooLarge
    case requestTransferUnavailable
    case responseTooLarge
    case invalidResponse(String)
    case invalidTransfer(String)
    case redirectForbidden
    case http(status: Int)
    case serverRejected
    case network(code: Int)
    case cancelled

    public var errorDescription: String?
    {
        switch self
        {
        case .invalidEndpoint: return "Use a UAM host URL with no credentials, query, or fragment; its path must be /, /companion, or /api."
        case .insecureEndpoint: return "Use HTTPS with a certificate trusted by this device. HTTP requires an explicit debug-only loopback connection."
        case .invalidPairingCertificate: return "The pairing certificate is invalid. Scan a fresh code from your computer’s Phone Access settings."
        case .invalidToken: return "Enter a nonempty access token using printable ASCII characters without spaces or line breaks."
        case .invalidRequest(let reason), .invalidResponse(let reason), .invalidTransfer(let reason): return reason
        case .requestTooLarge: return "The request exceeds the host or device size limit. Reduce its content."
        case .requestTransferUnavailable: return "This request needs chunked uploads. Upgrade the main UAM device or reduce the request below 60 KiB."
        case .responseTooLarge: return "The response exceeds the 32 MiB device limit. Request a smaller message page."
        case .redirectForbidden: return "UAM redirected the request. Enter the final HTTPS host address directly."
        case .http(401): return "UAM rejected the access token. Reconnect with the current token."
        case .http(403): return "This action is unavailable through the companion interface."
        case .http(404): return "The companion endpoint or response transfer is unavailable. Refresh before retrying."
        case .http(413): return "UAM rejected the request size. Reduce its content."
        case .http(let status): return "UAM returned HTTP \(status). Check the connection and chat state before retrying."
        case .serverRejected: return "UAM rejected this action. Refresh the chat and check its current permissions before retrying."
        case .network(let code): return "Connection failed (URL error \(code)). Check the host, network and certificate trust. Check the chat before retrying; the action may have reached UAM."
        case .cancelled: return "Request cancelled. Check the chat before retrying; the action may have reached UAM."
        }
    }
}

public struct CompanionEndpoint: Sendable, Equatable
{
    public let apiURL: URL

    public init(baseURL: URL, allowLoopbackHTTP: Bool = false) throws
    {
        guard var components = URLComponents(url: baseURL, resolvingAgainstBaseURL: false),
              let scheme = components.scheme?.lowercased(),
              let host = components.host?.lowercased(), !host.isEmpty,
              components.user == nil, components.password == nil,
              components.query == nil, components.fragment == nil,
              ["", "/", "/companion", "/companion/", "/api", "/api/"].contains(components.percentEncodedPath),
              components.port.map({ (1...65535).contains($0) }) ?? true else
        {
            throw CompanionError.invalidEndpoint
        }
        if scheme != "https"
        {
            #if DEBUG
            guard scheme == "http", allowLoopbackHTTP,
                  ["localhost", "127.0.0.1", "::1", "[::1]"].contains(host) else { throw CompanionError.insecureEndpoint }
            #else
            throw CompanionError.insecureEndpoint
            #endif
        }
        components.scheme = scheme
        components.host = host
        components.path = "/api"
        guard let url = components.url else { throw CompanionError.invalidEndpoint }
        apiURL = url
    }
}

/// Protocol checks are public so clients can validate fixtures without a network connection.
public enum CompanionProtocol
{
    public static func isValidAccessToken(_ token: String) -> Bool
    { !token.isEmpty && token.utf8.count <= 8192 && token.unicodeScalars.allSatisfy { (33...126).contains($0.value) } }

    public static let maximumDecodedBytes = 32 * 1024 * 1024
    public static let maximumRequestBytes = 32 * 1024 * 1024
    public static let maximumWireRequestBytes = 64 * 1024
    public static let directRequestBytes = 60 * 1024
    public static let maximumRequestChunkBytes = 32 * 1024
    public static let maximumChunkBytes = 128 * 1024
    public static let maximumTransferChunks = 4096

    public static func decode(_ data: Data) throws -> JSONValue
    {
        guard data.count <= maximumDecodedBytes else { throw CompanionError.responseTooLarge }
        guard String(data: data, encoding: .utf8) != nil else { throw CompanionError.invalidResponse("UAM returned invalid UTF-8 JSON.") }
        do { return try JSONDecoder().decode(JSONValue.self, from: data) }
        catch { throw CompanionError.invalidResponse("UAM returned invalid JSON. Check the host address and refresh.") }
    }

    public static func unwrap(_ value: JSONValue) throws -> JSONValue
    {
        guard let ok = value["ok"]?.boolValue else { return value }
        guard ok else { throw CompanionError.serverRejected }
        // Existing native handlers also return raw {ok:true,runId:...} receipts.
        // Preserve their identity fields; only an empty success has no result.
        return value["data"] ?? ((value.objectValue?.count ?? 0) > 1 ? value : .null)
    }
}

public struct CompanionTransfer: Sendable, Equatable
{
    public let id: String
    public let totalBytes: Int
    public let chunkBytes: Int

    public init(json: JSONValue) throws
    {
        guard let id = json["id"]?.stringValue, !id.isEmpty, id.utf8.count <= 256,
              let total = json["totalBytes"]?.intValue, total > 0 else
        {
            throw CompanionError.invalidTransfer("UAM returned an invalid response transfer descriptor.")
        }
        guard total <= CompanionProtocol.maximumDecodedBytes else { throw CompanionError.responseTooLarge }
        let chunk = json["chunkBytes"]?.intValue ?? (json["chunkBytes"] == nil ? CompanionProtocol.maximumChunkBytes : 0)
        guard chunk > 0, chunk <= CompanionProtocol.maximumChunkBytes,
              (total - 1) / chunk + 1 <= CompanionProtocol.maximumTransferChunks else
        {
            throw CompanionError.invalidTransfer("The response transfer chunk size or chunk count exceeds device bounds.")
        }
        self.id = id
        totalBytes = total
        chunkBytes = chunk
    }

    /// Validate before appending: strict base64, exact length, monotonic offset, exact final marker.
    public func decodeChunk(_ json: JSONValue, at offset: Int) throws -> Data
    {
        guard offset >= 0, offset < totalBytes, offset % chunkBytes == 0 else
        {
            throw CompanionError.invalidTransfer("The response transfer offset is invalid.")
        }
        let expected = min(chunkBytes, totalBytes - offset)
        let encodedLength = ((expected + 2) / 3) * 4
        guard let base64 = json["base64"]?.stringValue, base64.utf8.count == encodedLength,
              let data = Data(base64Encoded: base64), data.count == expected,
              // Reject noncanonical padding/unused bits as well as illegal characters.
              data.base64EncodedString() == base64,
              json["nextOffset"]?.intValue == offset + expected,
              json["done"]?.boolValue == (offset + expected == totalBytes) else
        {
            throw CompanionError.invalidTransfer("UAM returned an incomplete or invalid response chunk. Refresh the chat before retrying.")
        }
        return data
    }
}
