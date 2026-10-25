import Foundation

public struct ManagedChildRun: Sendable, Equatable, Identifiable
{
    public let id: String
    public let transcriptID: String
    public let providerID: String
    public let modelID: String
    public let title: String
    public let summary: String
    public let status: String
    public let lastReported: Bool
    public let canCancel: Bool
    public let followup: ManagedFollowupState?
    private let startedAt: Date?
    private let finishedAt: Date?

    init(json: JSONValue) throws
    {
        id = try managedID(json["runId"])
        transcriptID = try managedString(json["transcriptChatId"], maximum: 160)
        providerID = try managedString(json["providerId"], maximum: 160)
        modelID = try managedString(json["modelId"], maximum: 512)
        title = try managedString(json["taskTitle"] ?? json["agentId"] ?? "", maximum: 512)
        summary = try managedString(json["summary"] ?? "", maximum: 16384)
        status = try managedString(json["status"], maximum: 128)
        guard let canCancel = json.bool("canCancel"), json["lastReported"] == nil || json.bool("lastReported") != nil else { throw managedInvalid() }
        lastReported = json.bool("lastReported") ?? false
        self.canCancel = !lastReported && canCancel
        followup = lastReported ? nil : json["followup"].flatMap { try? ManagedFollowupState(json: $0) }
        startedAt = managedDate(json, epochKey: "startedAtEpochMs", textKey: "startedAt")
        finishedAt = managedDate(json, epochKey: "finishedAtEpochMs", textKey: "completedAt")
    }

    public var name: String { title.isEmpty ? "Managed subagent" : title }
    private var activeStatus: Bool { ["queued", "pending", "starting", "running", "waiting", "waiting_for_input", "cancelling"].contains(status) }
    public var active: Bool { !lastReported && activeStatus }
    public func elapsed(at now: Date, stale: Bool) -> String
    {
        let live = active && !stale
        guard let startedAt, let end = live ? now : finishedAt, end >= startedAt else
        { return live ? "Timing unavailable" : activeStatus && (stale || lastReported) ? "Timing unavailable until refresh" : "Duration not recorded" }
        let seconds = Int(end.timeIntervalSince(startedAt))
        return seconds < 60 ? "\(seconds)s" : seconds < 3600 ? "\(seconds / 60)m \(seconds % 60)s" : "\(seconds / 3600)h \(seconds % 3600 / 60)m"
    }
    public func statusLabel(stale: Bool) -> String
    {
        let labels = ["queued": "Queued", "pending": "Pending", "starting": "Starting", "running": "Running", "waiting": "Waiting",
                      "waiting_for_input": "Waiting for input", "cancelling": "Stopping", "completed": "Completed", "failed": "Failed",
                      "interrupted": "Interrupted", "cancelled": "Stopped", "canceled": "Stopped", "stopped": "Stopped"]
        return (lastReported || stale ? "Last reported: " : "") + (labels[status] ?? "Status unavailable")
    }
}

public struct ManagedChildPage: Sendable
{
    public let rootID: String
    public let controlScopeID: String
    public let runs: [ManagedChildRun]
    public let offset: Int
    public let nextOffset: Int?
    public let totalCount: Int

    public init(json: JSONValue, rootID: String, offset: Int, limit: Int = 20, runID: String? = nil) throws
    {
        guard (0...10000).contains(offset), (1...100).contains(limit), try managedID(json["chatId"]) == rootID,
              json.int("offset") == offset, let total = json.int("totalCount"), (0...10000).contains(total),
              let rows = json.array("runs"), rows.count <= limit, offset + rows.count <= total,
              json["nextOffset"] == .null || json.int("nextOffset") != nil else { throw managedInvalid() }
        let next = json.int("nextOffset")
        guard next == nil || (!rows.isEmpty && next == offset + rows.count && next! < total) else { throw managedInvalid() }
        let runs = try rows.map { try ManagedChildRun(json: $0) }
        guard Set(runs.map(\.id)).count == runs.count, runID == nil || runs.allSatisfy({ $0.id == runID }) else { throw managedInvalid() }
        self.rootID = rootID; controlScopeID = try managedID(json["controlScopeId"])
        self.runs = runs; self.offset = offset; nextOffset = next; totalCount = total
    }
}

public struct ManagedChildTranscript: Sendable
{
    public let title: String
    public let messages: [JSONValue]
    public let lastReported: Bool

    public init(json: JSONValue, run: ManagedChildRun) throws
    {
        guard json.string("runId") == run.id, json.string("providerId") == run.providerID,
              let messages = json.array("messages"), messages.count <= 100000,
              json["lastReported"] == nil || json.bool("lastReported") != nil else { throw managedInvalid() }
        title = try managedString(json["title"] ?? "", maximum: 16384)
        self.messages = messages
        lastReported = run.lastReported || (json.bool("lastReported") ?? false)
    }
}

public struct ManagedFollowupState: Sendable, Equatable
{
    public let canSend: Bool
    public let code: String
    public let currentTurnID: String
    public let currentTurnNumber: Int
    public var unavailableReason: String { managedFollowupRefusal(code) }

    init(json: JSONValue) throws
    {
        guard let canSend = json.bool("canSend"), let code = json.string("code"), managedFollowupCodes.contains(code),
              let turn = json.string("currentTurnId"), turn.isEmpty || managedUUID(turn),
              let number = json.int("currentTurnNumber"), (0...33).contains(number),
              !canSend || (code == "none" && managedUUID(turn) && (1..<33).contains(number)) else { throw managedInvalid() }
        self.canSend = canSend; self.code = code; currentTurnID = turn; currentTurnNumber = number
    }
}

public struct ManagedChildDraftKey: Hashable, Sendable
{
    public let connectionID: String
    public let rootID: String
    public let runID: String
    public let transcriptID: String
    public let rootIdentity: String
    public init(connectionID: String, rootID: String, runID: String, transcriptID: String, rootIdentity: String = "")
    { self.connectionID = connectionID; self.rootID = rootID; self.runID = runID; self.transcriptID = transcriptID; self.rootIdentity = rootIdentity }
}

public struct ManagedFollowupRequest: Sendable, Equatable
{
    public let key: ManagedChildDraftKey
    public let clientRequestID: String
    public let afterTurnID: String
    public let turnNumber: Int
    public let text: String
    public let draftRevision: UUID

    public init(key: ManagedChildDraftKey, state: ManagedFollowupState, text: String, draftRevision: UUID) throws
    {
        guard state.canSend, managedUUID(key.runID), Self.validText(text) else { throw managedInvalid() }
        _ = try managedID(.string(key.rootID)); _ = try managedID(.string(key.transcriptID))
        self.key = key; clientRequestID = UUID().uuidString.lowercased(); afterTurnID = state.currentTurnID
        turnNumber = state.currentTurnNumber + 1; self.text = text; self.draftRevision = draftRevision
    }

    public static func validText(_ text: String) -> Bool
    { !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && !text.contains("\0") && text.utf8.count <= 32768 }

    public func payload(controlScopeID: String) throws -> JSONValue
    {
        _ = try managedID(.string(controlScopeID))
        return ["chatId": .string(key.rootID), "runId": .string(key.runID), "controlScopeId": .string(controlScopeID),
                "afterTurnId": .string(afterTurnID), "clientRequestId": .string(clientRequestID), "text": .string(text)]
    }

    /// Missing, foreign or uncertain acknowledgements cannot retire the request.
    public func outcome(_ json: JSONValue) -> ManagedFollowupOutcome?
    {
        guard let body = json["followup"], let accepted = body.bool("accepted"), let replayed = body.bool("replayed"),
              let code = body.string("code"), managedFollowupCodes.contains(code), code != "delivery_unknown" else { return nil }
        if !accepted { return body["acceptance"] == .null && code != "none" && !replayed ? .refused(code) : nil }
        guard code == "none", let receipt = body["acceptance"], receipt.string("rootChatId") == key.rootID,
              receipt.string("runId") == key.runID, receipt.string("transcriptChatId") == key.transcriptID,
              receipt.string("clientRequestId") == clientRequestID, let turn = receipt.string("turnId"), managedUUID(turn), turn != afterTurnID,
              receipt.int("turnNumber") == turnNumber, receipt.string("statusAtAdmission") == "queued" else { return nil }
        return .accepted
    }
}

public enum ManagedFollowupOutcome: Sendable, Equatable { case accepted, refused(String) }

private let managedFollowupCodes: Set<String> = ["none", "not_implemented", "invalid_request", "legacy_identity_unverified", "scope_changed", "busy", "cancel_pending",
    "permission_denied", "queue_full", "receipt_limit", "transcript_unavailable", "model_identity_unverified", "native_resume_unavailable",
    "persistence_failed", "delivery_unknown", "idempotency_conflict"]

func managedInvalid() -> CompanionError
{ .invalidResponse("The managed child or its control scope changed. Refresh before trying again.") }

func managedFollowupRefusal(_ code: String) -> String
{
    switch code
    {
    case "busy", "cancel_pending", "queue_full": return "This child is busy or stopping. Wait for its current work to settle."
    case "scope_changed", "permission_denied": return "This child or its permissions changed. Refresh before sending follow-up work."
    case "receipt_limit": return "This child has reached its follow-up limit."
    case "persistence_failed": return "The computer could not save this follow-up. Your draft is kept."
    case "idempotency_conflict": return "The recorded request differs. Check this child's conversation before sending more work."
    default: return "Follow-up is unavailable for this child's current runtime. Your draft is kept."
    }
}

private func managedDate(_ json: JSONValue, epochKey: String, textKey: String) -> Date?
{
    if let value = json[epochKey]?.doubleValue, value.isFinite, value > 0, value < 8640000000000000
    { return Date(timeIntervalSince1970: value / 1000) }
    guard let value = json.string(textKey), value.range(of: #"T\d\d:\d\d:\d\d(?:\.\d+)?(?:Z|[+-]\d\d:\d\d)$"#, options: .regularExpression) != nil else { return nil }
    let parser = ISO8601DateFormatter()
    parser.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
    return parser.date(from: value) ?? ISO8601DateFormatter().date(from: value)
}

private func managedString(_ json: JSONValue?, maximum: Int) throws -> String
{
    guard let text = json?.stringValue, text.utf8.count <= maximum, !text.contains("\0") else { throw managedInvalid() }
    return text
}

func managedID(_ json: JSONValue?) throws -> String
{
    let value = try managedString(json, maximum: 160)
    guard !value.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { throw managedInvalid() }
    return value
}

private func managedUUID(_ text: String) -> Bool
{ UUID(uuidString: text)?.uuidString.lowercased() == text }
