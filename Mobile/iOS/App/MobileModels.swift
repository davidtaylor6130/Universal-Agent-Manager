import Foundation
import UAMKit

typealias WireObject = [String: Any]

extension Dictionary where Key == String, Value == Any
{
    func text(_ key: String, _ fallback: String = "") -> String { self[key] as? String ?? fallback }
    func flag(_ key: String) -> Bool { self[key] as? Bool ?? false }
    func number(_ key: String) -> Double { (self[key] as? NSNumber)?.doubleValue ?? 0 }
    func object(_ key: String) -> WireObject { self[key] as? WireObject ?? [:] }
    func objects(_ key: String) -> [WireObject] { self[key] as? [WireObject] ?? [] }
}

struct MobileConnection: Codable, Identifiable, Equatable
{
    var id = UUID().uuidString
    var name: String
    var address: String
    var routes: [String] = []
    var pairingCertificate: String?

    init(id: String = UUID().uuidString, name: String, address: String, routes: [String] = [], pairingCertificate: String? = nil)
    { self.id = id; self.name = name; self.address = address; self.routes = routes; self.pairingCertificate = pairingCertificate }

    private enum CodingKeys: String, CodingKey { case id, name, address, routes, pairingCertificate }
    init(from decoder: any Decoder) throws
    {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        id = try values.decode(String.self, forKey: .id)
        name = try values.decode(String.self, forKey: .name)
        address = try values.decode(String.self, forKey: .address)
        routes = try values.decodeIfPresent([String].self, forKey: .routes) ?? []
        pairingCertificate = try values.decodeIfPresent(String.self, forKey: .pairingCertificate)
    }

    static func paired(name: String, address: URL, saved: [MobileConnection], preferredID: String?) throws -> MobileConnection
    {
        let endpoint = try CompanionEndpoint(baseURL: address).apiURL
        let matches = saved.filter { connection in
            guard let url = URL(string: connection.address), let old = try? CompanionEndpoint(baseURL: url).apiURL else { return false }
            return old.scheme == endpoint.scheme && old.host == endpoint.host && (old.port ?? 443) == (endpoint.port ?? 443)
        }
        var item = matches.first { $0.id == preferredID } ?? matches.first ?? MobileConnection(name: name, address: address.absoluteString)
        item.name = name; item.address = address.absoluteString
        return item
    }
}

struct MobileChat: Identifiable
{
    let raw: WireObject
    let updatedAt: Date
    let interactionAt: Date?
    let settledAt: Date?
    let createdAt: Date

    init(raw: WireObject)
    {
        self.raw = raw
        let created = parseWireDate(raw.text("createdAt")) ?? Date(timeIntervalSince1970: 0)
        createdAt = created
        let updated = parseWireDate(raw.text("updatedAt"))
        updatedAt = updated ?? .distantPast
        if let interaction = raw["interactionAt"] as? String
        {
            // Empty means imported history without UAM interaction; absent supports old hosts.
            interactionAt = interaction.isEmpty ? nil : parseWireDate(interaction) ?? created
        }
        else { interactionAt = updated ?? created }
        settledAt = parseWireDate(raw.text("settledAt"))
    }

    var id: String { raw.text("id") }
    var title: String { raw.text("title", "New chat") }
    var folderID: String { raw.text("folderId") }
    var providerID: String { raw.text("providerId") }
    var hostID: String { raw.text("executionHostId", "local") }
    var modelID: String { raw.text("modelId") }
    var runtime: WireObject { raw.object("acpSession") }
    var parentID: String { raw.text("parentChatId") }
    var familyID: String
    {
        let root = raw.text("branchRootChatId")
        return root.isEmpty ? parentID.isEmpty ? id : parentID : root
    }
    var pinned: Bool { raw.flag("pinned") }
    var readOnly: Bool { raw.flag("importedReadOnly") }
    var snoozed: Bool { isSnoozed(at: .now, needsInput: needsInput) }
    var processing: Bool
    {
        let terminal = raw.object("cliTerminal")
        return runtime.flag("processing") || runtime.number("backgroundTaskCount") > 0 ||
            ["starting", "waitingPermission"].contains(runtime.text("lifecycleState")) ||
            terminal.flag("processing") || ["starting", "busy", "shuttingDown"].contains(terminal.text("lifecycleState"))
    }
    var needsInput: Bool
    {
        let attention = runtime.text("attentionKind")
        if !attention.isEmpty { return attention != "error" }
        return !runtime.object("pendingPermission").isEmpty || !runtime.object("pendingUserInput").isEmpty
    }
    var unseen: Bool
    {
        if raw["attentionRevision"] is String { return !raw.text("attentionRevision").isEmpty }
        return runtime.flag("readySinceLastSelect") || raw.object("cliTerminal").flag("readySinceLastSelect")
    }
    var active: Bool { isActive(at: .now) }
    func isActive(at now: Date) -> Bool
    { includesActivity(latestInteraction: interactionAt, rank: activityRank, now: now) }
    var status: String { needsInput ? "Needs input" : processing ? "Working" : unseen ? "Ready to review" : "Idle" }

    fileprivate var activityRank: Int
    {
        let ready = runtime.flag("readySinceLastSelect") || raw.object("cliTerminal").flag("readySinceLastSelect")
        return needsInput ? 3 : processing ? 2 : unseen && ready ? 1 : 0
    }
    fileprivate func isSnoozed(at now: Date, needsInput: Bool) -> Bool
    { raw.number("snoozedUntilMs") > now.timeIntervalSince1970 * 1000 && !needsInput }
    fileprivate func includesActivity(latestInteraction: Date?, rank: Int, now: Date) -> Bool
    {
        if isSnoozed(at: now, needsInput: rank == 3) { return false }
        func unsettled(_ date: Date?) -> Bool
        {
            guard let date, date >= now.addingTimeInterval(-3 * 24 * 60 * 60) else { return false }
            return settledAt == nil || date > settledAt!
        }
        let wakeMilliseconds = raw.number("snoozedUntilMs")
        let wake = wakeMilliseconds > 0 ? Date(timeIntervalSince1970: wakeMilliseconds / 1000) : nil
        // Done is organisation, not acknowledgement: a settled unread reply stays unread.
        return rank >= 2 || unsettled(latestInteraction) || unsettled(wake) ||
            (rank == 1 && raw.text("settledAt").isEmpty)
    }
}

struct MobileActivityGroup: Identifiable
{
    let root: MobileChat
    let destination: MobileChat
    let members: [MobileChat]
    let activityAt: Date
    var id: String { root.familyID }

    static func build(_ chats: [MobileChat], now: Date = .now, matchingIDs: Set<String>? = nil) -> [MobileActivityGroup]
    {
        let visible = chats.filter { $0.raw.text("temporaryParentChatId").isEmpty }
        return Dictionary(grouping: visible, by: \.familyID).values.compactMap { family in
            guard let root = family.first(where: { $0.id == $0.familyID }) else { return nil }
            if let matchingIDs, !family.contains(where: { matchingIDs.contains($0.id) }) { return nil }
            let latestInteraction = family.compactMap(\.interactionAt).max()
            let rank = family.map(\.activityRank).max() ?? 0
            guard root.includesActivity(latestInteraction: latestInteraction, rank: rank, now: now) else { return nil }
            let destination = family.sorted {
                if $0.activityRank != $1.activityRank { return $0.activityRank > $1.activityRank }
                let left = $0.interactionAt ?? Date(timeIntervalSince1970: 0)
                let right = $1.interactionAt ?? Date(timeIntervalSince1970: 0)
                return left == right ? $0.id < $1.id : left > right
            }
            let wake = root.raw.number("snoozedUntilMs")
            let activityAt = max(latestInteraction ?? Date(timeIntervalSince1970: 0), Date(timeIntervalSince1970: max(wake, 0) / 1000))
            return MobileActivityGroup(root: root, destination: destination[0], members: family, activityAt: activityAt)
        }.sorted { $0.activityAt == $1.activityAt ? $0.id < $1.id : $0.activityAt > $1.activityAt }
    }
}

struct MobileMessage: Identifiable
{
    let index: Int
    let raw: WireObject
    var id: Int { index }
    var role: String { raw.text("role") }
    var content: String { raw.text("content") }
    var tools: [WireObject] { raw.objects("toolCalls") }
    var attachments: [WireObject] { raw.objects("attachments") }
}

struct ComposerAttachment: Codable, Identifiable, Equatable, Sendable
{
    var id = UUID().uuidString
    let name: String
    let mimeType: String
    let data: Data
    var isImage: Bool { mimeType.hasPrefix("image/") }
    var sizeLabel: String { ByteCountFormatter.string(fromByteCount: Int64(data.count), countStyle: .file) }
}

struct ComposerDraft: Codable, Equatable, Sendable
{
    var revision = UUID()
    var text = "" { didSet { if text != oldValue { revision = UUID() } } }
    var attachments: [ComposerAttachment] = [] { didSet { if attachments != oldValue { revision = UUID() } } }

    init(text: String = "", attachments: [ComposerAttachment] = [])
    {
        self.text = text
        self.attachments = attachments
    }

    private enum CodingKeys: String, CodingKey { case revision, text, attachments }
    init(from decoder: any Decoder) throws
    {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        revision = try values.decodeIfPresent(UUID.self, forKey: .revision) ?? UUID()
        text = try values.decode(String.self, forKey: .text)
        attachments = try values.decode([ComposerAttachment].self, forKey: .attachments)
    }
}

private let fractionalWireDateStyle = Date.ISO8601FormatStyle(includingFractionalSeconds: true)
private let wholeWireDateStyle = Date.ISO8601FormatStyle(includingFractionalSeconds: false)

func parseWireDate(_ string: String) -> Date?
{
    let value = string.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !value.isEmpty else { return nil }
    if let date = (try? fractionalWireDateStyle.parse(value)) ?? (try? wholeWireDateStyle.parse(value))
    { return date }
    // Retain the existing parser for unusual legacy forms, including long fractions.
    let fractional = ISO8601DateFormatter()
    fractional.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
    return fractional.date(from: string) ?? ISO8601DateFormatter().date(from: string)
}

func shortModelName(_ string: String) -> String
{
    string.split(separator: "/").last.map(String.init) ?? string
}
