import Foundation

/// Object projections keep every unknown key and nested provider payload when re-encoded.
public protocol JSONRecord: Codable, Sendable
{
    var fields: [String: JSONValue] { get }
    init(fields: [String: JSONValue]) throws
}

extension JSONRecord
{
    public init(from decoder: any Decoder) throws
    {
        try self.init(fields: decoder.singleValueContainer().decode([String: JSONValue].self))
    }
    public func encode(to encoder: any Encoder) throws
    {
        var container = encoder.singleValueContainer()
        try container.encode(fields)
    }
    public init(json: JSONValue) throws
    {
        guard let fields = json.objectValue else { throw CompanionError.invalidResponse("Expected a JSON object.") }
        try self.init(fields: fields)
    }
    public var jsonValue: JSONValue { .object(fields) }
    public subscript(_ key: String) -> JSONValue? { fields[key] }
}

public struct ChatSummary: JSONRecord, Identifiable, Equatable
{
    public let fields: [String: JSONValue]
    public init(fields: [String: JSONValue]) throws
    {
        for key in ["id", "title", "providerId", "folderId", "createdAt", "updatedAt"]
        {
            guard fields[key]?.stringValue != nil else { throw CompanionError.invalidResponse("Chat summary is missing a required string field.") }
        }
        self.fields = fields
    }
    public var id: String { fields["id"]!.stringValue! }
    public var title: String { fields["title"]!.stringValue! }
    public var providerId: String { fields["providerId"]!.stringValue! }
    public var folderId: String { fields["folderId"]!.stringValue! }
    public var createdAt: String { fields["createdAt"]!.stringValue! }
    public var updatedAt: String { fields["updatedAt"]!.stringValue! }
    public var executionHostId: String? { fields["executionHostId"]?.stringValue }
    public var remoteRecoveryState: String? { fields["remoteRecoveryState"]?.stringValue }
    public var routingMode: String? { fields["routingMode"]?.stringValue }
    public var routingSourceFolderId: String? { fields["routingSourceFolderId"]?.stringValue }
    public var routingSourceDirectory: String? { fields["routingSourceDirectory"]?.stringValue }
    public var routingAssignedConnectionIdentity: String? { fields["routingAssignedConnectionIdentity"]?.stringValue }
    public var routingAssignedProviderConfigurationIdentity: String? { fields["routingAssignedProviderConfigurationIdentity"]?.stringValue }
    public var routingWaitReason: String? { fields["routingWaitReason"]?.stringValue }
    public var temporaryParentChatId: String? { fields["temporaryParentChatId"]?.stringValue }
    public var parentChatId: String? { fields["parentChatId"]?.stringValue }
    public var branchRootChatId: String? { fields["branchRootChatId"]?.stringValue }
    public var modelId: String? { fields["modelId"]?.stringValue }
    public var reviewerModelId: String? { fields["reviewerModelId"]?.stringValue }
    public var reasoningEffort: String? { fields["reasoningEffort"]?.stringValue }
    public var serviceTier: String? { fields["serviceTier"]?.stringValue }
    public var approvalMode: String? { fields["approvalMode"]?.stringValue }
    public var uamAgentId: String? { fields["uamAgentId"]?.stringValue }
    public var commandSafetyTier: String? { fields["commandSafetyTier"]?.stringValue }
    public var computerUseBackend: String? { fields["computerUseBackend"]?.stringValue }
    public var computerUseEffectiveBackend: String? { fields["computerUseEffectiveBackend"]?.stringValue }
    public var computerUseTargetKind: String? { fields["computerUseTargetKind"]?.stringValue }
    public var computerUseTargetId: String? { fields["computerUseTargetId"]?.stringValue }
    public var computerUseTargetTitle: String? { fields["computerUseTargetTitle"]?.stringValue }
    public var computerUseTargetInputMode: String? { fields["computerUseTargetInputMode"]?.stringValue }
    public var memoryLevel: String? { fields["memoryLevel"]?.stringValue }
    public var memoryLastProcessedAt: String? { fields["memoryLastProcessedAt"]?.stringValue }
    public var workspaceDirectory: String? { fields["workspaceDirectory"]?.stringValue }
    public var workspaceIsolationKind: String? { fields["workspaceIsolationKind"]?.stringValue }
    public var workspaceSourceDirectory: String? { fields["workspaceSourceDirectory"]?.stringValue }
    public var workspaceBaseRef: String? { fields["workspaceBaseRef"]?.stringValue }
    public var gitBaseBranch: String? { fields["gitBaseBranch"]?.stringValue }
    public var workspaceBranchName: String? { fields["workspaceBranchName"]?.stringValue }
    public var workspaceWorktreeDirectory: String? { fields["workspaceWorktreeDirectory"]?.stringValue }
    public var interactionAt: String? { fields["interactionAt"]?.stringValue }
    public var lastOpenedAt: String? { fields["lastOpenedAt"]?.stringValue }
    public var attentionRevision: String? { fields["attentionRevision"]?.stringValue }
    public var settledAt: String? { fields["settledAt"]?.stringValue }
    public var messagesDigest: String? { fields["messagesDigest"]?.stringValue }
    public var activeGoalId: String? { fields["activeGoalId"]?.stringValue }
    public var remoteRecoveryEnabled: Bool? { fields["remoteRecoveryEnabled"]?.boolValue }
    public var routingReservationPending: Bool? { fields["routingReservationPending"]?.boolValue }
    public var routingUnresolvedReservation: Bool? { fields["routingUnresolvedReservation"]?.boolValue }
    public var pinned: Bool? { fields["pinned"]?.boolValue }
    public var sideCleanupRequested: Bool? { fields["sideCleanupRequested"]?.boolValue }
    public var branchMessageEdited: Bool? { fields["branchMessageEdited"]?.boolValue }
    public var serviceTierExplicit: Bool? { fields["serviceTierExplicit"]?.boolValue }
    public var uamControlEnabled: Bool? { fields["uamControlEnabled"]?.boolValue }
    public var computerUseEnabled: Bool? { fields["computerUseEnabled"]?.boolValue }
    public var computerUseProviderAvailable: Bool? { fields["computerUseProviderAvailable"]?.boolValue }
    public var memoryEnabled: Bool? { fields["memoryEnabled"]?.boolValue }
    public var smallModelMode: Bool? { fields["smallModelMode"]?.boolValue }
    public var importedReadOnly: Bool? { fields["importedReadOnly"]?.boolValue }
    public var goalMode: Bool? { fields["goalMode"]?.boolValue }
    public var branchFromMessageIndex: Int? { fields["branchFromMessageIndex"]?.intValue }
    public var memoryLastProcessedMessageCount: Int? { fields["memoryLastProcessedMessageCount"]?.intValue }
    public var snoozedUntilMs: Int? { fields["snoozedUntilMs"]?.intValue }
    public var messageCount: Int? { fields["messageCount"]?.intValue }
    public var customIcon: JSONValue? { fields["customIcon"] }
    public var computerUse: JSONValue? { fields["computerUse"] }
    public var cliTerminal: JSONValue? { fields["cliTerminal"] }
    public var acpSession: JSONValue? { fields["acpSession"] }
    public var goals: JSONValue? { fields["goals"] }
    public var messages: JSONValue? { fields["messages"] }
}

public struct ChatMessage: JSONRecord, Equatable
{
    public let fields: [String: JSONValue]
    public init(fields: [String: JSONValue]) throws
    {
        for key in ["role", "content", "createdAt"]
        {
            guard fields[key]?.stringValue != nil else { throw CompanionError.invalidResponse("Message is missing a required string field.") }
        }
        self.fields = fields
    }
    /// Strings remain open to future provider roles and states.
    public var role: String { fields["role"]!.stringValue! }
    public var content: String { fields["content"]!.stringValue! }
    public var createdAt: String { fields["createdAt"]!.stringValue! }
    public var providerId: String? { fields["providerId"]?.stringValue }
    public var modelId: String? { fields["modelId"]?.stringValue }
    public var thoughts: String? { fields["thoughts"]?.stringValue }
    public var planSummary: String? { fields["planSummary"]?.stringValue }
    public var stopReason: String? { fields["stopReason"]?.stringValue }
    public var checkpointSha: String? { fields["checkpointSha"]?.stringValue }
    public var checkpointParentSha: String? { fields["checkpointParentSha"]?.stringValue }
    public var interrupted: Bool? { fields["interrupted"]?.boolValue }
    public var acpPromptNotSent: Bool? { fields["acpPromptNotSent"]?.boolValue }
    public var acpPromptDiscarded: Bool? { fields["acpPromptDiscarded"]?.boolValue }
    public var prioritySteer: Bool? { fields["prioritySteer"]?.boolValue }
    public var continuesTurn: Bool? { fields["continuesTurn"]?.boolValue }
    public var processingTimeMs: Double? { fields["processingTimeMs"]?.doubleValue }
    public var planEntries: [JSONValue]? { fields["planEntries"]?.arrayValue }
    public var toolCalls: [JSONValue]? { fields["toolCalls"]?.arrayValue }
    public var blocks: [JSONValue]? { fields["blocks"]?.arrayValue }
    public var markdownStoreFiles: [JSONValue]? { fields["markdownStoreFiles"]?.arrayValue }
    public var attachments: [JSONValue]? { fields["attachments"]?.arrayValue }
}

public struct InitialState: JSONRecord, Equatable
{
    public let fields: [String: JSONValue]
    public let chats: [ChatSummary]
    public init(fields: [String: JSONValue]) throws
    {
        self.fields = fields
        // A revision poll may deliberately return only unchanged/revision identity.
        if fields["unchanged"]?.boolValue == true { chats = []; return }
        guard let value = fields["chats"], value.arrayValue != nil else { throw CompanionError.invalidResponse("Initial state has no chat list.") }
        chats = try value.decode([ChatSummary].self)
    }
    public var unchanged: Bool { fields["unchanged"]?.boolValue ?? false }
    public var stateRevision: Int? { fields["stateRevision"]?.intValue }
    public var bootId: String? { fields["bootId"]?.stringValue }
    public var selectedChatId: String? { fields["selectedChatId"]?.stringValue }
    public var folders: [JSONValue] { fields["folders"]?.arrayValue ?? [] }
    public var providers: [JSONValue] { fields["providers"]?.arrayValue ?? [] }
    public var settings: JSONValue? { fields["settings"] }
}

public struct ChatMessagesPage: JSONRecord, Equatable
{
    public let fields: [String: JSONValue]
    /// nil for unchanged responses. An empty array is a changed, empty page.
    public let messages: [ChatMessage]?
    public init(fields: [String: JSONValue]) throws
    {
        self.fields = fields
        messages = try fields["messages"].map { try $0.decode([ChatMessage].self) }
        if let value = fields["startIndex"], value.intValue == nil || value.intValue! < 0
        {
            throw CompanionError.invalidResponse("Message page has an invalid start index.")
        }
        if let value = fields["totalCount"], value.intValue == nil || value.intValue! < 0
        {
            throw CompanionError.invalidResponse("Message page has an invalid total count.")
        }
        if let start = fields["startIndex"]?.intValue, let total = fields["totalCount"]?.intValue
        {
            guard start <= total, (messages?.count ?? 0) <= total - start else { throw CompanionError.invalidResponse("Message page exceeds its transcript bounds.") }
        }
        for key in ["unsentMessageIndices", "recoverableUnsentMessageIndices", "userMessageIndices"]
        {
            guard let value = fields[key], value != .null else { continue }
            guard let array = value.arrayValue, array.allSatisfy({ item in
                guard let index = item.intValue, index >= 0 else { return false }
                return fields["totalCount"]?.intValue.map { index < $0 } ?? true
            }) else { throw CompanionError.invalidResponse("Message page contains invalid navigation indices.") }
        }
    }
    public var chatId: String? { fields["chatId"]?.stringValue }
    public var messagesDigest: String? { fields["messagesDigest"]?.stringValue }
    public var unchanged: Bool { fields["unchanged"]?.boolValue ?? false }
    public var startIndex: Int? { fields["startIndex"]?.intValue }
    public var totalCount: Int? { fields["totalCount"]?.intValue }
    public var olderPageBefore: Int? { startIndex.flatMap { $0 > 0 ? $0 : nil } }
    public var userMessageIndices: [Int]? { fields["userMessageIndices"]?.arrayValue?.compactMap(\.intValue) }
    public var unsentMessageIndices: [Int]? { fields["unsentMessageIndices"]?.arrayValue?.compactMap(\.intValue) }
    public var recoverableUnsentMessageIndices: [Int]? { fields["recoverableUnsentMessageIndices"]?.arrayValue?.compactMap(\.intValue) }
}

/// `before` is an exclusive global message index, not a page number.
public struct ChatMessagesRequest: Sendable, Encodable
{
    public let chatId: String
    public let limit: Int
    public let before: Int?
    public let messagesDigest: String?
    public let deferToolCallContent: Bool
    public let refreshNative: Bool

    public init(chatId: String, limit: Int = 100, before: Int? = nil, messagesDigest: String? = nil, deferToolCallContent: Bool = true, refreshNative: Bool = false) throws
    {
        guard !chatId.isEmpty, (1...200).contains(limit), before.map({ $0 >= 0 }) ?? true else
        {
            throw CompanionError.invalidRequest("Provide a chat ID, a limit from 1 to 200, and a nonnegative before index.")
        }
        self.chatId = chatId
        self.limit = limit
        self.before = before
        self.messagesDigest = messagesDigest
        self.deferToolCallContent = deferToolCallContent
        self.refreshNative = refreshNative
    }
    public var payload: JSONValue
    {
        var object: [String: JSONValue] = ["chatId": .string(chatId), "limit": .integer(Int64(limit)), "deferToolCallContent": .bool(deferToolCallContent), "refreshNative": .bool(refreshNative)]
        if let before { object["before"] = .integer(Int64(before)) }
        if let messagesDigest { object["messagesDigest"] = .string(messagesDigest) }
        return .object(object)
    }
}
