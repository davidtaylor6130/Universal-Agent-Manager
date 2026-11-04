import Foundation
import CryptoKit

/// A context describes durable ownership; it never follows the selected screen.
struct DraftContext: Codable, Hashable, Sendable
{
    let connectionID: String
    let kind: String
    let itemID: String

    static func chat(connectionID: String, chatID: String) -> Self
    { Self(connectionID: connectionID, kind: "chat", itemID: chatID) }

    static func newChat(connectionID: String, projectID: String) -> Self
    { Self(connectionID: connectionID, kind: "new", itemID: projectID) }
}

struct DraftStore
{
    let directory: URL

    init(directory: URL = URL.applicationSupportDirectory.appending(path: "Drafts", directoryHint: .isDirectory))
    { self.directory = directory }

    private struct Manifest: Codable
    {
        let format: String
        let version: Int
        let revision: UUID
        let text: String
        let attachments: [BlobReference]
    }

    private struct BlobReference: Codable
    {
        let id: String
        let name: String
        let mimeType: String
        let blob: String
        let byteCount: Int
    }

    private struct FormatProbe: Decodable { let format: String? }
    private static let manifestFormat = "uam-native-draft"

    private func digest(_ data: Data) -> String
    { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }

    private func key(for context: DraftContext) throws -> String
    {
        // Structured identity prevents delimiter collisions and untrusted path components.
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return digest(try encoder.encode(context))
    }

    private func url(for context: DraftContext) throws -> URL
    { directory.appendingPathComponent(try key(for: context) + ".json") }

    private func blobs(for context: DraftContext) throws -> URL
    { directory.appendingPathComponent(try key(for: context) + ".attachments", isDirectory: true) }

    private func validBlobName(_ name: String) -> Bool
    { name.utf8.count == 64 && name.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) } }

    private func checkedBlob(_ url: URL, expectedBytes: Int) throws
    {
        let resource = try url.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey])
        guard expectedBytes >= 0, resource.isRegularFile == true, resource.isSymbolicLink != true,
              resource.fileSize == expectedBytes else { throw DraftError.attachmentUnavailable }
    }

    private func checkBlobDirectory(_ url: URL) throws
    {
        let resource = try url.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
        guard resource.isDirectory == true, resource.isSymbolicLink != true else { throw DraftError.attachmentUnavailable }
    }

    func load(_ context: DraftContext) throws -> ComposerDraft
    {
        let url = try url(for: context)
        if FileManager.default.fileExists(atPath: url.path)
        {
            let data = try Data(contentsOf: url)
            let decoder = JSONDecoder()
            guard let format = try decoder.decode(FormatProbe.self, from: data).format else
            { return try decoder.decode(ComposerDraft.self, from: data) }
            guard format == Self.manifestFormat else { throw DraftError.unsupportedFormat }
            let manifest = try decoder.decode(Manifest.self, from: data)
            guard manifest.version == 1 else { throw DraftError.unsupportedFormat }
            let folder = try blobs(for: context)
            if !manifest.attachments.isEmpty { try checkBlobDirectory(folder) }
            var attachments: [ComposerAttachment] = []
            for reference in manifest.attachments
            {
                guard validBlobName(reference.blob) else { throw DraftError.attachmentUnavailable }
                let blob = folder.appendingPathComponent(reference.blob)
                try checkedBlob(blob, expectedBytes: reference.byteCount)
                let bytes = try Data(contentsOf: blob)
                guard bytes.count == reference.byteCount, digest(bytes) == reference.blob else
                { throw DraftError.attachmentUnavailable }
                attachments.append(ComposerAttachment(id: reference.id, name: reference.name, mimeType: reference.mimeType, data: bytes))
            }
            // Set revision last: normal attachment/text mutation intentionally makes a new one.
            var draft = ComposerDraft(text: manifest.text, attachments: attachments)
            draft.revision = manifest.revision
            return draft
        }
        // Preserve drafts saved by the initial native-client preview. A canonical manifest,
        // including an empty settled draft, always wins over these historical files.
        if context.kind == "chat"
        {
            let identity = Data("\(context.connectionID):\(context.itemID)".utf8)
            let keys = [digest(identity), identity.base64EncodedString().replacingOccurrences(of: "/", with: "_")]
            for key in keys
            {
                let legacy = directory.appendingPathComponent(key + ".json")
                if FileManager.default.fileExists(atPath: legacy.path)
                {
                    let draft = try JSONDecoder().decode(ComposerDraft.self, from: Data(contentsOf: legacy))
                    try save(draft, in: context)
                    return draft
                }
            }
        }
        return ComposerDraft()
    }

    func save(_ draft: ComposerDraft, in context: DraftContext) throws
    {
        let files = FileManager.default
        try files.createDirectory(at: directory, withIntermediateDirectories: true)
        let folder = try blobs(for: context)
        if !draft.attachments.isEmpty
        {
            try files.createDirectory(at: folder, withIntermediateDirectories: true)
            try checkBlobDirectory(folder)
        }
        var references: [BlobReference] = []
        for attachment in draft.attachments
        {
            let name = digest(attachment.data)
            let blob = folder.appendingPathComponent(name)
            if files.fileExists(atPath: blob.path)
            {
                // Immutable content is written once. Load verifies the full digest; a text
                // edit need not read/re-encode/rewrite all attachment bytes on the UI actor.
                try checkedBlob(blob, expectedBytes: attachment.data.count)
            }
            else
            {
                try attachment.data.write(to: blob, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
            }
            references.append(BlobReference(id: attachment.id, name: attachment.name, mimeType: attachment.mimeType, blob: name, byteCount: attachment.data.count))
        }
        let manifest = Manifest(format: Self.manifestFormat, version: 1, revision: draft.revision, text: draft.text, attachments: references)
        // Publish only after every required blob is durable. Failed publication leaves the
        // prior manifest and its referenced blobs intact; orphan new blobs are safe to retain.
        try JSONEncoder().encode(manifest).write(to: url(for: context), options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
        let retained = Set(references.map(\.blob))
        // Cleanup is scoped to this context and happens only after the authoritative commit.
        // Cleanup failure may retain unused bytes; it must not report a committed draft as lost.
        if (try? checkBlobDirectory(folder)) != nil,
           let children = try? files.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)
        {
            for child in children where validBlobName(child.lastPathComponent) && !retained.contains(child.lastPathComponent)
            {
                if let resource = try? child.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey]),
                   resource.isRegularFile == true, resource.isSymbolicLink != true
                { try? files.removeItem(at: child) }
            }
        }
    }

    enum DraftError: LocalizedError
    {
        case attachmentUnavailable, unsupportedFormat
        var errorDescription: String?
        {
            switch self
            {
            case .attachmentUnavailable: "A saved draft attachment is missing or damaged. The draft was kept; restore the attachment before sending."
            case .unsupportedFormat: "This saved draft uses an unsupported format. It has been kept without changes."
            }
        }
    }

    /// Only a definite acknowledgement consumes the exact submitted revision.
    @discardableResult
    func settle(_ context: DraftContext, sentRevision: UUID) throws -> ComposerDraft
    {
        let current = try load(context)
        guard current.revision == sentRevision else { return current }
        let cleared = ComposerDraft()
        try save(cleared, in: context)
        return cleared
    }
}

struct HistoryPageMerge
{
    let messages: [MobileMessage]
    let digest: String
    let needsTailRefresh: Bool

    init(existing: [MobileMessage], existingDigest: String, loaded: [MobileMessage], pageDigest: String, older: Bool)
    {
        if older
        {
            // Global indices are meaningful only within the same transcript revision.
            guard !existingDigest.isEmpty, pageDigest == existingDigest,
                  let first = existing.first, loaded.last.map({ $0.index + 1 == first.index }) ?? false else
            {
                messages = existing; digest = ""; needsTailRefresh = true
                return
            }
            messages = loaded + existing
            digest = existingDigest
            needsTailRefresh = false
            return
        }
        // A changed digest can include edits/removal before this window. Discard cached
        // pages rather than claiming a disconnected or stale prefix belongs to this revision.
        if !existingDigest.isEmpty, existingDigest == pageDigest,
           let start = loaded.first?.index,
           existing.last.map({ $0.index + 1 >= start }) == true
        { messages = existing.filter { $0.index < start } + loaded }
        else { messages = loaded }
        digest = pageDigest
        needsTailRefresh = false
    }
}
