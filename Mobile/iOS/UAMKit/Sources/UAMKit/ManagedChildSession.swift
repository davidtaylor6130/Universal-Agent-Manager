import Foundation
import Observation

@MainActor @Observable public final class ManagedChildDraft
{
    public enum Phase: Sendable, Equatable { case idle, pending, uncertain }
    public private(set) var text = ""
    public private(set) var revision = UUID()
    public fileprivate(set) var request: ManagedFollowupRequest?
    public fileprivate(set) var phase = Phase.idle
    public fileprivate(set) var feedback = ""
    @ObservationIgnored fileprivate var feedbackNeedsRetention = true

    public func edit(_ value: String)
    {
        feedbackNeedsRetention = true
        guard value.utf8.count <= 32768, !value.contains("\0") else { feedback = "Follow-up text must fit within 32 KiB and contain no NUL characters."; return }
        text = value; revision = UUID()
        if phase == .idle { feedback = "" }
    }
    fileprivate var protected: Bool { !text.isEmpty || request != nil || phase != .idle || (!feedback.isEmpty && feedbackNeedsRetention) }
}

/// App-lifetime inert input, bounded without evicting authored text or uncertain requests.
@MainActor public final class ManagedChildDraftStore
{
    private var entries: [ManagedChildDraftKey: ManagedChildDraft] = [:]
    private let capacity: Int
    public init(capacity: Int = 32) { self.capacity = max(1, capacity) }
    public func existing(_ key: ManagedChildDraftKey) -> ManagedChildDraft? { entries[key] }
    public func acquire(_ key: ManagedChildDraftKey) -> ManagedChildDraft?
    {
        if let entry = entries[key] { return entry }
        if entries.count >= capacity
        {
            guard let empty = entries.first(where: { !$0.value.protected })?.key else { return nil }
            entries.removeValue(forKey: empty)
        }
        let entry = ManagedChildDraft(); entries[key] = entry; return entry
    }
}

@MainActor @Observable public final class ManagedChildSession
{
    public typealias Request = @MainActor (String, JSONValue) async throws -> JSONValue
    public enum StopPhase: Sendable, Equatable { case pending, uncertain, accepted }
    public let connectionID: String
    public let rootID: String
    public let rootIdentity: String
    public private(set) var runs: [ManagedChildRun] = []
    public private(set) var selected: ManagedChildRun?
    public private(set) var transcript: ManagedChildTranscript?
    public private(set) var reading = false
    public private(set) var stale = true
    public private(set) var error: String?
    public private(set) var offset = 0
    public private(set) var nextOffset: Int?
    public private(set) var totalCount = 0
    public private(set) var stops: [ManagedChildDraftKey: StopPhase] = [:]
    public private(set) var stopFeedback: [ManagedChildDraftKey: String] = [:]
    public var readingPositions: [String: Int] = [:]
    @ObservationIgnored private let request: Request
    @ObservationIgnored private let supports: @MainActor (String) -> Bool
    @ObservationIgnored private let originCurrent: @MainActor () -> Bool
    @ObservationIgnored private let writable: @MainActor () -> Bool
    @ObservationIgnored private let drafts: ManagedChildDraftStore
    @ObservationIgnored private var epoch = UUID()
    @ObservationIgnored private var presentation: UUID?
    @ObservationIgnored private var visible = false
    @ObservationIgnored private var physicalRead = false
    @ObservationIgnored private var mutations = 0
    @ObservationIgnored private var controlScopeID: String?

    public init(connectionID: String, rootID: String, rootIdentity: String = "", drafts: ManagedChildDraftStore, request: @escaping Request,
                supports: @escaping @MainActor (String) -> Bool, originCurrent: @escaping @MainActor () -> Bool,
                writable: @escaping @MainActor () -> Bool)
    {
        self.connectionID = connectionID; self.rootID = rootID; self.rootIdentity = rootIdentity; self.drafts = drafts; self.request = request
        self.supports = supports; self.originCurrent = originCurrent; self.writable = writable
    }

    @discardableResult public func activate() -> UUID
    {
        let token = UUID(); presentation = token
        visible = true; epoch = UUID(); stale = true; controlScopeID = nil
        return token
    }
    public func deactivate(_ token: UUID? = nil)
    {
        guard token == nil || token == presentation else { return }
        visible = false; presentation = nil; epoch = UUID(); stale = true; controlScopeID = nil
    }
    private var current: Bool { visible && originCurrent() }
    public var displayLastReported: Bool { stale || !current }
    public var canRetire: Bool
    {
        !visible && !physicalRead && mutations == 0 &&
            !stops.values.contains(where: { $0 == .pending || $0 == .uncertain })
    }
    public func key(_ run: ManagedChildRun) -> ManagedChildDraftKey
    { .init(connectionID: connectionID, rootID: rootID, runID: run.id, transcriptID: run.transcriptID, rootIdentity: rootIdentity) }
    public func draft(_ run: ManagedChildRun) -> ManagedChildDraft? { drafts.existing(key(run)) }

    public func canOpen(_ run: ManagedChildRun) -> Bool
    { current && !run.transcriptID.isEmpty && supports("getManagedAgentTranscript") }

    public func select(_ run: ManagedChildRun)
    {
        guard canOpen(run), runs.contains(where: { $0.id == run.id && $0.transcriptID == run.transcriptID }) else { return }
        epoch = UUID(); selected = run; transcript = nil; stale = true; controlScopeID = nil; error = nil
    }
    public func showList()
    { epoch = UUID(); selected = nil; transcript = nil; stale = true; controlScopeID = nil; error = nil }

    public func showPage(_ offset: Int) async
    {
        guard !reading, selected == nil, (0...10000).contains(offset) else { return }
        self.offset = offset; stale = true; controlScopeID = nil
        await refresh()
    }

    /// At most one underlying read per root. Retiring a view does not free a still-running transport slot.
    public func refresh() async
    {
        guard current, supports("listManagedSubagents"), !physicalRead, mutations == 0 else { return }
        physicalRead = true; reading = true
        let token = epoch, target = selected
        defer { physicalRead = false; reading = false }
        do
        {
            var payload: JSONValue = ["chatId": .string(rootID), "offset": .integer(Int64(target == nil ? offset : 0)), "limit": 20]
            if let target { payload["runId"] = .string(target.id) }
            let page = try ManagedChildPage(json: await request("listManagedSubagents", payload), rootID: rootID,
                                            offset: target == nil ? offset : 0, runID: target?.id)
            guard current, epoch == token, !Task.isCancelled else { return }
            if let target
            {
                guard supports("getManagedAgentTranscript"), let run = page.runs.first(where: { $0.id == target.id }),
                      run.transcriptID == target.transcriptID, run.providerID == target.providerID else { throw managedInvalid() }
                let value = try await request("getManagedAgentTranscript", ["chatId": .string(rootID), "transcriptChatId": .string(run.transcriptID)])
                let conversation = try ManagedChildTranscript(json: value, run: run)
                guard current, epoch == token, !Task.isCancelled else { return }
                selected = run; transcript = conversation
            }
            else { runs = page.runs; nextOffset = page.nextOffset; totalCount = page.totalCount }
            // Acknowledgement fences the old snapshot, not every later turn of this child.
            // Only a validated current read retires it; unknown requests remain explicit.
            for run in page.runs where !run.lastReported && stops[key(run)] == .accepted
            {
                stops.removeValue(forKey: key(run)); stopFeedback.removeValue(forKey: key(run))
            }
            controlScopeID = page.controlScopeID; stale = false; error = nil
        }
        catch
        {
            guard current, epoch == token, !Task.isCancelled else { return }
            // Keep readable content, but never keep controls or live-state indicators from a failed read.
            stale = true; controlScopeID = nil; self.error = error.localizedDescription
        }
    }

    private func controlled(_ run: ManagedChildRun) -> Bool
    {
        guard current, writable(), !stale, !reading, controlScopeID != nil else { return false }
        if let selected { return selected == run && transcript?.lastReported == false }
        return runs.contains(run)
    }
    public func canStop(_ run: ManagedChildRun) -> Bool
    {
        controlled(run) && !run.lastReported && run.canCancel && supports("cancelManagedSubagent") &&
            stops[key(run)] != .pending && stops[key(run)] != .accepted && draft(run)?.phase != .pending
    }
    public func canFollowup(_ run: ManagedChildRun, retry: Bool = false) -> Bool
    {
        guard controlled(run), selected?.id == run.id, !run.lastReported, supports("submitManagedSubagentFollowup"),
              run.followup != nil, stops[key(run)] != .pending, draft(run)?.phase != .pending else { return false }
        return retry ? draft(run)?.request != nil && draft(run)?.phase == .uncertain : run.followup?.canSend == true && draft(run)?.request == nil
    }
    @discardableResult public func beginFollowup(_ run: ManagedChildRun) -> ManagedChildDraft?
    {
        if let existing = draft(run) { return existing }
        guard canFollowup(run), let entry = drafts.acquire(key(run)) else
        { error = "Follow-up draft storage is full or this child cannot accept input. Existing drafts have been kept."; return nil }
        return entry
    }

    public func stop(_ run: ManagedChildRun) async
    {
        guard canStop(run), let scope = controlScopeID else { return }
        let identity = key(run), token = epoch
        stops[identity] = .pending; stopFeedback[identity] = "Requesting Stop…"; mutations += 1
        defer { mutations -= 1 }
        do
        {
            let reply = try await request("cancelManagedSubagent", ["chatId": .string(rootID), "runId": .string(run.id), "controlScopeId": .string(scope)])
            guard current, epoch == token, reply.string("runId") == run.id else { throw managedInvalid() }
            stops[identity] = .accepted; stopFeedback[identity] = "Stop requested; waiting for the child's reported state."
        }
        catch { stops[identity] = .uncertain; stopFeedback[identity] = "Stop was not confirmed. Refresh, then explicitly retry Stop if still available." }
        if current, epoch == token { stale = true; controlScopeID = nil }
    }

    public func sendFollowup(_ run: ManagedChildRun, retry: Bool = false) async
    {
        guard canFollowup(run, retry: retry), let scope = controlScopeID, let state = run.followup,
              let entry = draft(run) ?? beginFollowup(run) else { return }
        let token = epoch
        do
        {
            let envelope: ManagedFollowupRequest
            if retry, let retained = entry.request { envelope = retained }
            else { envelope = try ManagedFollowupRequest(key: key(run), state: state, text: entry.text, draftRevision: entry.revision) }
            guard envelope.key == key(run) else { throw managedInvalid() }
            entry.feedbackNeedsRetention = true
            entry.request = envelope; entry.phase = .pending; entry.feedback = "Recording follow-up work…"; mutations += 1
            defer { mutations -= 1 }
            do
            {
                let response = try await request("submitManagedSubagentFollowup", envelope.payload(controlScopeID: scope))
                guard current, epoch == token, let outcome = envelope.outcome(response) else { throw managedInvalid() }
                switch outcome
                {
                case .accepted:
                    if entry.revision == envelope.draftRevision { entry.edit("") }
                    entry.request = nil; entry.phase = .idle; entry.feedback = "Follow-up accepted."
                    entry.feedbackNeedsRetention = false
                case .refused(let code):
                    entry.request = nil; entry.phase = .idle; entry.feedback = managedFollowupRefusal(code)
                }
            }
            catch
            {
                entry.phase = .uncertain
                entry.feedback = "Delivery is unconfirmed. Retry same request sends the original message; any edits stay in this draft."
            }
            if current, epoch == token { stale = true; controlScopeID = nil }
        }
        catch { entry.feedbackNeedsRetention = true; entry.feedback = "Enter nonblank follow-up text of at most 32 KiB. Your draft is kept." }
    }
}
