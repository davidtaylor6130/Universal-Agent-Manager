import Foundation

/// A phone-owned browse projection of the main computer's existing organisation.
/// Collections reference workspaces; neither references nor browse filters own chats.
struct HomeCatalogue
{
    struct Workspace: Identifiable
    {
        let raw: WireObject
        let chats: [MobileChat]
        var id: String { raw.text("id") }
        var title: String { raw.text("title") }
        var collapsed: Bool { raw.flag("collapsed") }
        var chatCount: Int { chats.count }
    }

    struct Collection: Identifiable
    {
        let raw: WireObject
        let workspaces: [Workspace]
        var id: String { raw.text("id") }
        var name: String { raw.text("name") }
        var collapsed: Bool { raw.flag("collapsed") }
        var chatCount: Int { workspaces.reduce(0) { $0 + $1.chatCount } }
    }

    let roots: [MobileChat]
    let pinned: [MobileChat]
    let snoozed: [MobileChat]
    let collections: [Collection]
    /// Workspaces not claimed by a collection, in the original folder order.
    let workspaces: [Workspace]
    let unsorted: [MobileChat]
    let membersByRootID: [String: [MobileChat]]

    private struct Family
    {
        let root: MobileChat
        let members: [MobileChat]
        let recent: Date
        let rootRecent: Date
        let created: Date
        let snoozed: Bool
    }

    static func build(
        chats: [MobileChat], folders: [WireObject], collections: [WireObject],
        projectID: String = "", matchingIDs: Set<String>? = nil, now: Date = .now
    ) -> HomeCatalogue
    {
        let epoch = Date(timeIntervalSince1970: 0)
        var seenChatIDs = Set<String>()
        let visible = chats.filter {
            !$0.id.isEmpty && $0.raw.text("temporaryParentChatId").isEmpty && seenChatIDs.insert($0.id).inserted
        }
        // Normalize sort keys once; timestamp parsing must never run inside a comparator.
        func recentTime(_ chat: MobileChat) -> Date
        {
            chat.interactionAt ?? (chat.updatedAt == .distantPast ? chat.createdAt : chat.updatedAt)
        }
        let families: [Family] = Dictionary(grouping: visible, by: \.familyID).compactMap { rootID, members in
            guard let root = members.first(where: { $0.id == rootID }),
                  projectID.isEmpty || root.folderID == projectID else { return nil }
            if let matchingIDs, !members.contains(where: { matchingIDs.contains($0.id) }) { return nil }
            let familyNeedsInput = members.contains(where: \.needsInput)
            return Family(
                root: root, members: members,
                recent: members.map(recentTime).max() ?? epoch, rootRecent: recentTime(root),
                created: root.createdAt,
                snoozed: root.raw.number("snoozedUntilMs") > now.timeIntervalSince1970 * 1000 && !familyNeedsInput
            )
        }.sorted {
            if $0.recent != $1.recent { return $0.recent > $1.recent }
            if $0.rootRecent != $1.rootRecent { return $0.rootRecent > $1.rootRecent }
            if $0.created != $1.created { return $0.created > $1.created }
            return $0.root.id < $1.root.id
        }
        let roots = families.map(\.root)
        let members = Dictionary(uniqueKeysWithValues: families.map { ($0.root.id, $0.members) })
        let chatsByFolder = Dictionary(grouping: roots, by: \.folderID)
        var seenFolderIDs = Set<String>()
        let allFolders = folders.filter {
            let id = $0.text("id")
            return !id.isEmpty && seenFolderIDs.insert(id).inserted
        }
        let folderIDs = Set(allFolders.map { $0.text("id") })
        let rows = allFolders.compactMap { folder -> Workspace? in
            let id = folder.text("id")
            guard projectID.isEmpty || id == projectID else { return nil }
            let chats = chatsByFolder[id] ?? []
            // An explicit project may retain its empty workspace; a search only retains matches.
            guard matchingIDs == nil || !chats.isEmpty else { return nil }
            return Workspace(raw: folder, chats: chats)
        }
        let rowsByID = Dictionary(uniqueKeysWithValues: rows.map { ($0.id, $0) })
        var claimed = Set<String>()
        var seenCollectionIDs = Set<String>()
        let collectionRows = collections.compactMap { collection -> Collection? in
            let id = collection.text("id")
            guard !id.isEmpty, seenCollectionIDs.insert(id).inserted else { return nil }
            let workspaces = collection.objects("references").compactMap { reference -> Workspace? in
                guard reference.text("type") == "workspace-folder",
                      let row = rowsByID[reference.text("target")], claimed.insert(row.id).inserted else { return nil }
                return row
            }
            let unfiltered = matchingIDs == nil && projectID.isEmpty
            guard unfiltered || !workspaces.isEmpty else { return nil }
            // Keep raw references for passive resource labels; never invent native launch actions.
            return Collection(raw: collection, workspaces: workspaces)
        }
        // Desktop keeps unfiled roots first, then missing-folder buckets in encounter order.
        var unknownFolderIDs: [String] = []
        var seenUnknownFolderIDs = Set<String>()
        for root in roots where !root.folderID.isEmpty && !folderIDs.contains(root.folderID)
        {
            if seenUnknownFolderIDs.insert(root.folderID).inserted { unknownFolderIDs.append(root.folderID) }
        }
        let unsorted = (chatsByFolder[""] ?? []) + unknownFolderIDs.flatMap { chatsByFolder[$0] ?? [] }
        return HomeCatalogue(
            roots: roots,
            // Pinning survives snooze; both sections are flat views of the same family roots.
            pinned: roots.filter(\.pinned), snoozed: families.filter(\.snoozed).map(\.root),
            collections: collectionRows, workspaces: rows.filter { !claimed.contains($0.id) },
            unsorted: unsorted,
            membersByRootID: members
        )
    }
}
