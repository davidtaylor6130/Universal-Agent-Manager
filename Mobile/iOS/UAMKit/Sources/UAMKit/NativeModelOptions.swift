import Foundation

/// Catalogue path spelling only: this is not a filesystem resolver or an action identity.
/// Match the desktop's separator/trailing-slash rules without folding POSIX case,
/// resolving a controller's home directory, or changing opaque provider/model IDs.
public enum NativeModelCatalogWorkspace
{
    public static func matches(_ candidate: String, _ workspace: String, platform: String = "") -> Bool
    { key(candidate, platform: platform) == key(workspace, platform: platform) }

    private static func key(_ workspace: String, platform: String) -> String
    {
        let units = Array(workspace.utf8.prefix(3))
        let drivePath = units.count == 3 && ((65...90).contains(units[0]) || (97...122).contains(units[0])) &&
            units[1] == 58 && (units[2] == 47 || units[2] == 92)
        let windows = platform.lowercased() == "windows" || (platform.isEmpty && drivePath)
        var path = windows || (platform.isEmpty && workspace.hasPrefix("\\\\")) ? workspace.replacingOccurrences(of: "\\", with: "/") : workspace
        if windows { path = path.lowercased() }
        while path.hasSuffix("/"), path != "/" { path.removeLast() }
        return path
    }
}

/// Absence differs from a present null preference (an explicit provider Default).
public enum NativeOptionPresence<Value: Sendable & Equatable>: Sendable, Equatable
{
    case absent
    case value(Value)

    fileprivate init(json: JSONValue?, parse: (JSONValue) throws -> Value) throws
    {
        guard let json, let present = json.bool("present") else { throw invalidNativeOption() }
        if !present { self = .absent; return }
        guard let value = json["value"] else { throw invalidNativeOption() }
        self = .value(try parse(value))
    }
}

public struct NativeModelOptionOwnership: Sendable, Equatable
{
    public let chatID: String
    public let nativeSessionID: String
    public let executionHostID: String
    public let lifetime: String

    fileprivate init(json: JSONValue) throws
    {
        chatID = try nativeOptionString(json["chatId"], maximum: 512)
        nativeSessionID = try nativeOptionString(json["nativeSessionId"], maximum: 512)
        executionHostID = try nativeOptionString(json["executionHostId"], maximum: 512)
        lifetime = try nativeOptionString(json["lifetime"], maximum: 128)
    }

    public var payload: JSONValue
    {
        ["chatId": .string(chatID), "nativeSessionId": .string(nativeSessionID),
         "executionHostId": .string(executionHostID), "lifetime": .string(lifetime)]
    }
}

public struct CopilotAutoRoutingTier: Sendable, Equatable, Identifiable
{
    public let id: String
    public let displayName: String
    public let description: String
    public let type: String
    public let enabled: Bool
    public let disabledReason: String

    fileprivate init(json: JSONValue) throws
    {
        id = try nativeOptionString(json["id"], maximum: 512)
        displayName = try nativeOptionString(json["displayName"], maximum: 256, emptyAllowed: true)
        description = try nativeOptionString(json["description"], maximum: 2048, emptyAllowed: true)
        type = try nativeOptionString(json["type"], maximum: 64)
        guard let enabled = json["status"]?.bool("enabled") else { throw invalidNativeOption() }
        self.enabled = enabled
        let reason = try json["status"]?["reason"].map { try nativeOptionString($0, maximum: 128, emptyAllowed: true) } ?? ""
        let message = try json["status"]?["message"].map { try nativeOptionString($0, maximum: 2048, emptyAllowed: true) } ?? ""
        disabledReason = message.isEmpty ? reason : message
    }

    public var name: String { displayName.isEmpty ? id : displayName }
    public var selectable: Bool { enabled && type == "auto" }
}

/// Only the public, bounded controller projection is an action target; unknown keys never enter payloads.
public struct CopilotAutoRoutingOption: Sendable, Equatable
{
    public let ownership: NativeModelOptionOwnership?
    public let catalogStatus: String
    public let stateStatus: String
    public let eligibleTiers: NativeOptionPresence<[CopilotAutoRoutingTier]>
    public let providerDefault: NativeOptionPresence<String>
    public let currentModel: NativeOptionPresence<String>
    public let effective: NativeOptionPresence<String>
    public let pending: NativeOptionPresence<String?>
    public let activating: NativeOptionPresence<String?>
    public let requestPending: Bool
    public let available: Bool
    public let disabledReason: String
    public let lastError: String

    public init(json: JSONValue) throws
    {
        let statuses = ["unknown", "ready", "invalid", "unsupported"]
        guard let catalogStatus = json.string("catalogStatus"), statuses.contains(catalogStatus),
              let stateStatus = json.string("stateStatus"), statuses.contains(stateStatus),
              let requestPending = json.bool("requestPending"), let available = json.bool("available"),
              let ack = json.string("lastAckStatus"), ["", "pending", "unchanged"].contains(ack) else { throw invalidNativeOption() }
        self.catalogStatus = catalogStatus; self.stateStatus = stateStatus
        self.requestPending = requestPending; self.available = available
        _ = try nativeOptionString(json["disabledCode"], maximum: 128, emptyAllowed: true)
        disabledReason = try nativeOptionString(json["disabledReason"], maximum: 2048, emptyAllowed: true)
        lastError = try nativeOptionString(json["lastError"], maximum: 2048, emptyAllowed: true)
        ownership = try json["ownership"].map { try NativeModelOptionOwnership(json: $0) }
        eligibleTiers = try NativeOptionPresence(json: json["eligibleTiers"]) { value in
            guard let values = value.arrayValue, values.count <= 64,
                  try JSONEncoder().encode(value).count <= 65536 else { throw invalidNativeOption() }
            let tiers = try values.map { try CopilotAutoRoutingTier(json: $0) }
            guard Set(tiers.map(\.id)).count == tiers.count else { throw invalidNativeOption() }
            return tiers
        }
        let string: (JSONValue) throws -> String = { try nativeOptionString($0, maximum: 512) }
        providerDefault = try NativeOptionPresence(json: json["providerDefault"], parse: string)
        currentModel = try NativeOptionPresence(json: json["currentModel"], parse: string)
        effective = try NativeOptionPresence(json: json["effective"], parse: string)
        let nullableString: (JSONValue) throws -> String? = { $0 == .null ? nil : try string($0) }
        pending = try NativeOptionPresence(json: json["pending"], parse: nullableString)
        activating = try NativeOptionPresence(json: json["activating"], parse: nullableString)
    }

    public var tiers: [CopilotAutoRoutingTier] { if case .value(let tiers) = eligibleTiers { tiers } else { [] } }
    public var canChoose: Bool
    {
        guard ownership != nil, available, !requestPending, catalogStatus == "ready", stateStatus == "ready",
              currentModel == .value("auto"), activating == .absent,
              case .value = eligibleTiers else { return false }
        return true
    }

    public func name(for value: String?) -> String
    {
        guard let value else { return "Default" }
        return tiers.first { $0.id == value }?.name ?? value
    }

    public func preferencePayload(_ value: String?) throws -> JSONValue
    {
        guard canChoose, var payload = ownership?.payload,
              value == nil || tiers.contains(where: { $0.id == value && $0.selectable }) else { throw invalidNativeOption() }
        payload["optionId"] = "autoRouting"
        payload["value"] = value.map(JSONValue.string) ?? .null
        return payload
    }

    public func matches(chatID: String, providerID: String, runtimeProviderID: String, protocolKind: String,
                        nativeSessionID: String, executionHostID: String, readOnly: Bool) -> Bool
    {
        let copilotIDs = ["copilot-cli", "copilot", "github-copilot"]
        guard !readOnly, copilotIDs.contains(providerID), copilotIDs.contains(runtimeProviderID), protocolKind == "copilot-sdk" else { return false }
        // An ownerless projection can explain unavailability but cannot supply an action payload.
        guard let ownership else { return true }
        return ownership.chatID == chatID && ownership.nativeSessionID == nativeSessionID && ownership.executionHostID == executionHostID
    }
}

private func invalidNativeOption() -> CompanionError
{ .invalidResponse("The native model options are unavailable or changed. Refresh this chat before trying again.") }

private func nativeOptionString(_ json: JSONValue?, maximum: Int, emptyAllowed: Bool = false) throws -> String
{
    guard let value = json?.stringValue, (emptyAllowed || !value.isEmpty), value.utf8.count <= maximum,
          !value.contains("\0") else { throw invalidNativeOption() }
    return value
}
