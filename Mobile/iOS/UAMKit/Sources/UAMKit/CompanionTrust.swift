import Foundation
import Security

/// A public pairing root belongs to one HTTPS origin, never to a global trust store.
public struct CompanionTrust: Sendable
{
    private let certificate: Data
    private let host: String
    private let port: Int

    public init(baseURL: URL, certificate: String) throws
    {
        let endpoint = try CompanionEndpoint(baseURL: baseURL).apiURL
        guard endpoint.scheme == "https", certificate.utf8.count <= 10924,
              let bytes = Data(base64Encoded: certificate), !bytes.isEmpty, bytes.count <= 8192,
              bytes.base64EncodedString() == certificate,
              SecCertificateCreateWithData(nil, bytes as CFData) != nil else
        { throw CompanionError.invalidPairingCertificate }
        self.certificate = bytes
        host = Self.normalizedHost(endpoint.host ?? "")
        port = endpoint.port ?? 443
    }

    private static func normalizedHost(_ value: String) -> String
    { value.trimmingCharacters(in: CharacterSet(charactersIn: "[]")).lowercased() }

    func matches(host: String, port: Int, protocol scheme: String?) -> Bool
    { scheme?.lowercased() == "https" && Self.normalizedHost(host) == self.host && port == self.port }

    func evaluate(_ trust: SecTrust, host: String, port: Int, protocol scheme: String?) -> Bool
    {
        guard matches(host: host, port: port, protocol: scheme),
              let anchor = SecCertificateCreateWithData(nil, certificate as CFData),
              SecTrustSetPolicies(trust, SecPolicyCreateSSL(true, self.host as CFString)) == errSecSuccess,
              SecTrustSetAnchorCertificates(trust, [anchor] as CFArray) == errSecSuccess,
              SecTrustSetAnchorCertificatesOnly(trust, true) == errSecSuccess else { return false }
        // Keep signature, chain, validity dates, server-auth and hostname checks.
        // No exceptions or trust results are installed outside this individual trust.
        return SecTrustEvaluateWithError(trust, nil)
    }
}
