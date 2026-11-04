import Foundation
import CoreFoundation
import UAMKit

struct PairingCode
{
    let name: String
    let address: String
    let token: String
    let certificate: String?

    init(_ code: String) throws
    {
        guard code.utf8.count <= 16 * 1024 else { throw MobileError.message("This pairing code is too large.") }
        let endpoint: String
        let credential: String
        let label: String
        let root: String?
        if let data = code.data(using: .utf8), let value = try? JSONSerialization.jsonObject(with: data) as? WireObject,
           value.text("type") == "uam-companion", let version = value["version"] as? NSNumber,
           CFGetTypeID(version) != CFBooleanGetTypeID(), version.doubleValue == 1
        {
            endpoint = value.text("url"); credential = value.text("token"); label = value.text("name")
            guard value["certificate"] == nil || value["certificate"] is NSNull || value["certificate"] is String else { throw CompanionError.invalidPairingCertificate }
            root = value["certificate"] as? String
        }
        else
        {
            guard let parts = URLComponents(string: code), parts.scheme == "uam", parts.host == "pair",
                  parts.user == nil, parts.password == nil, parts.fragment == nil else
            { throw MobileError.message("This is not a UAM pairing code.") }
            var fields: [String: String] = [:]
            for item in parts.queryItems ?? []
            {
                guard fields[item.name] == nil else { throw MobileError.message("The pairing code contains duplicate fields.") }
                fields[item.name] = item.value ?? ""
            }
            endpoint = fields["url"] ?? ""; credential = fields["token"] ?? ""; label = fields["name"] ?? ""; root = fields["certificate"]
        }
        guard !endpoint.isEmpty, !credential.isEmpty else
        { throw MobileError.message("The pairing code is missing its address or token.") }
        guard let url = URL(string: endpoint) else { throw CompanionError.invalidEndpoint }
        _ = try CompanionEndpoint(baseURL: url)
        guard CompanionProtocol.isValidAccessToken(credential) else
        { throw CompanionError.invalidToken }
        if let root { _ = try CompanionTrust(baseURL: url, certificate: root) }
        name = String(label.trimmingCharacters(in: .whitespacesAndNewlines).prefix(160)); address = endpoint; token = credential; certificate = root
    }
}
