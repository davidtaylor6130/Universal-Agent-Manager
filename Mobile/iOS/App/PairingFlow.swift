import Foundation
import Observation
import UAMKit

struct PairingDetails
{
    let name: String
    let address: String
    let token: String
    var pairingCertificate: String? = nil
    var clearSavedCertificate = false

    init(name: String, address: String, token: String, pairingCertificate: String? = nil, clearSavedCertificate: Bool = false)
    {
        self.name = String(name.trimmingCharacters(in: .whitespacesAndNewlines).prefix(160))
        self.address = address; self.token = token
        self.pairingCertificate = pairingCertificate; self.clearSavedCertificate = clearSavedCertificate
    }

    var displayAddress: String
    {
        guard var parts = URLComponents(string: address), parts.host != nil else { return "the entered address" }
        parts.user = nil; parts.password = nil; parts.query = nil; parts.fragment = nil; parts.path = ""
        return parts.string ?? "the entered address"
    }
    var displayName: String
    { name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? URL(string: address)?.host ?? "Your computer" : name.trimmingCharacters(in: .whitespacesAndNewlines) }
}

struct PairingProblem
{
    let title: String
    let message: String
    let detail: String?

    init(_ error: any Error)
    {
        detail = (error as? CompanionError).flatMap { if case .network(let code) = $0 { return "URL error \(code)" }; return nil }
        if error is CancellationError
        {
            title = "Connection check cancelled"
            message = "Edit the details to start a new connection check."
            return
        }
        if case MobileError.message(let message) = error
        {
            // Only known local validation messages get specific copy. Never echo an
            // arbitrary error string or a server/NSError description into this page.
            switch message
            {
            case "This is not a UAM pairing code.":
                title = "Not a Universal Agent Manager pairing code"
                self.message = "Scan the code shown in your computer’s Universal Agent Manager Phone Access settings."
            case "The pairing code is missing its address or token.":
                title = "Pairing code is incomplete"
                self.message = "The code is missing its address or access token. Show a fresh pairing code on your computer."
            case "The pairing code contains duplicate fields.":
                title = "Pairing code has duplicate fields"
                self.message = "This code repeats a connection field. Scan a fresh code from your computer’s Phone Access settings."
            case "This pairing code is too large.":
                title = "Pairing code is too large"
                self.message = "Scan a fresh code from your computer’s Phone Access settings."
            case "Enter a valid UAM address.":
                title = "Check the HTTPS address"
                self.message = "Copy the current HTTPS address from your computer’s Universal Agent Manager Phone Access settings."
            case "This address did not return a UAM companion response.", "UAM returned an unexpected response.":
                title = "Unexpected response"
                self.message = "This address did not return a supported Universal Agent Manager response. Check the published HTTPS address."
            default:
                title = "Connection could not be checked"
                self.message = "Check the current HTTPS address and access token in your computer’s Phone Access settings, then try again."
            }
            return
        }
        guard let error = error as? CompanionError else
        {
            if error is ConnectionSecrets.SecretError
            { title = "Connection could not be saved"; message = "This iPhone could not save the access token securely. Edit the details and try again." }
            else
            { title = "Connection could not be checked"; message = "Check the current HTTPS address and access token in your computer’s Phone Access settings, then try again." }
            return
        }
        switch error
        {
        case .network(-1001):
            title = "Connection timed out"
            message = "Your computer did not respond in time. Check its current Phone Access address and that this iPhone can reach it on your network."
        case .network(-1009):
            title = "This iPhone is offline"
            message = "Connect to a network that can reach your computer, then try again."
        case .network(-1206 ... -1200):
            title = "Secure connection failed"
            message = "Use your computer’s published HTTPS address. Its certificate must match that address. Scan a fresh pairing code from your computer’s Phone Access settings to configure this secure connection."
        case .network:
            title = "Computer could not be reached"
            message = "Check that Universal Agent Manager is running, Phone Access is enabled and its current address is reachable from this iPhone."
        case .http(401):
            title = "Access token not accepted"
            message = "Scan a fresh pairing code or copy the current access token from your computer’s Phone Access settings."
        case .http(404):
            title = "Universal Agent Manager was not found"
            message = "Check the published HTTPS address in Phone Access settings. The address must lead to Universal Agent Manager’s companion interface."
        case .invalidEndpoint, .insecureEndpoint:
            title = "Check the HTTPS address"
            message = "Use the published HTTPS address with no username, password, query or fragment. Its path must be /, /companion or /api."
        case .invalidPairingCertificate:
            title = "Pairing certificate is invalid"
            message = "Scan a fresh pairing code from your computer’s Phone Access settings."
        case .invalidToken:
            title = "Check the access token"
            message = "Copy the current Phone Access token exactly. It must use printable ASCII characters without spaces or line breaks."
        case .redirectForbidden:
            title = "Address redirected elsewhere"
            message = "Enter the final published HTTPS address directly. Universal Agent Manager will not forward the access token to another address."
        case .invalidResponse, .invalidTransfer, .responseTooLarge:
            title = "Unexpected response"
            message = "This address did not return a supported Universal Agent Manager response. Check the address and update Universal Agent Manager on your computer if needed."
        case .cancelled:
            title = "Connection check cancelled"
            message = "Edit the details to start a new connection check."
        default:
            title = "Connection was not accepted"
            message = "Check that Phone Access is enabled and use its current HTTPS address and access token."
        }
    }
}

@MainActor @Observable final class PairingFlow
{
    enum Phase { case entry, checking, success, failure }
    typealias Check = @MainActor (PairingDetails) async throws -> Void
    var name = ""
    var address = ""
    var token = ""
    private(set) var pairingCertificate: String?
    private(set) var clearSavedCertificate = false
    @ObservationIgnored private var certificateOrigin: String?
    private(set) var phase = Phase.entry
    private(set) var problem: PairingProblem?
    private(set) var checkedName = ""
    private(set) var checkedAddress = ""
    @ObservationIgnored private var task: Task<Void, Never>?
    @ObservationIgnored private var attemptID = UUID()

    var canSubmit: Bool
    { phase == .entry && !address.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && !token.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }

    func submit(using check: @escaping Check)
    {
        guard phase == .entry else { return }
        let details = PairingDetails(name: name, address: address.trimmingCharacters(in: .whitespacesAndNewlines), token: token.trimmingCharacters(in: .whitespacesAndNewlines), pairingCertificate: origin(address) == certificateOrigin ? pairingCertificate : nil, clearSavedCertificate: certificateOrigin != nil && origin(address) == certificateOrigin && clearSavedCertificate)
        phase = .checking; problem = nil
        checkedName = details.displayName; checkedAddress = details.displayAddress
        let id = UUID(); attemptID = id
        task = Task { [weak self] in
            do
            {
                try Task.checkCancellation()
                try await check(details)
                guard !Task.isCancelled, let self, self.attemptID == id else { return }
                self.phase = .success; self.task = nil
            }
            catch
            {
                guard !Task.isCancelled, let self, self.attemptID == id else { return }
                self.problem = PairingProblem(error); self.phase = .failure; self.task = nil
            }
        }
    }

    @discardableResult func acceptCode(_ code: String, using check: @escaping Check) -> Bool
    {
        guard phase == .entry else { return false }
        do
        {
            let pairing = try PairingCode(code)
            name = pairing.name; address = pairing.address; token = pairing.token
            pairingCertificate = pairing.certificate; certificateOrigin = origin(pairing.address)
            clearSavedCertificate = true
            submit(using: check)
            return true
        }
        catch
        {
            problem = PairingProblem(error); phase = .failure
            checkedName = ""; checkedAddress = ""
            return false
        }
    }

    private func origin(_ address: String) -> String?
    {
        guard let url = URL(string: address.trimmingCharacters(in: .whitespacesAndNewlines)),
              let endpoint = try? CompanionEndpoint(baseURL: url).apiURL else { return nil }
        return "\(endpoint.scheme ?? "")://\(endpoint.host ?? ""):\(endpoint.port ?? 443)"
    }

    func editDetails()
    { guard phase == .failure else { return }; problem = nil; phase = .entry }

    // A closing sheet must keep its visible result throughout the dismissal.
    // Invalidate the attempt independently of resetting a form for reuse.
    func invalidate()
    {
        attemptID = UUID(); task?.cancel(); task = nil
    }

    func cancel()
    {
        invalidate()
        problem = nil; phase = .entry
    }

    // Incoming links are single-use events, not a queue. A link arriving while
    // this sheet is busy must be opened again explicitly after finishing it.
    @discardableResult func consumePendingCode(_ pending: inout URL?, using check: @escaping Check) -> Bool
    {
        guard let code = pending else { return false }
        pending = nil
        guard phase == .entry else { return false }
        return acceptCode(code.absoluteString, using: check)
    }
}
