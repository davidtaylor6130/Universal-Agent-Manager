import SwiftUI

struct HomeView: View
{
    @Environment(MobileAppModel.self) private var model
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.dynamicTypeSize) private var dynamicTypeSize
    @ScaledMetric(relativeTo: .subheadline) private var groupFirstLineHeight = 18
    @State private var tab = "active"
    @State private var search = ""
    @State private var searching = false
    @State private var projectID = ""
    private enum RootSheet: String, Identifiable
    {
        case settings, newChat, pairing
        var id: String { rawValue }
    }
    @State private var rootSheet: RootSheet?
    // The binding clears when dismissal begins. Keep the presenter occupied
    // until onDismiss so an incoming URL cannot start a competing transition.
    @State private var rootSheetActive = false
    @State private var expandedGroups: [String: Bool] = [:]
    @State private var searchExpandedGroups: [String: Bool] = [:]
    @State private var homeClock = Date.now
    private var query: String { search.trimmingCharacters(in: .whitespacesAndNewlines) }
    @FocusState private var searchFocused: Bool

    var body: some View
    {
        @Bindable var model = model
        ZStack
        {
            UAMTheme.background.ignoresSafeArea()
            if model.activeChatID != nil { ConversationView() }
            else { home }
        }
        .animation(reduceMotion ? nil : .snappy(duration: 0.2), value: model.activeChatID != nil)
        .sheet(item: $rootSheet, onDismiss: rootSheetDismissed)
        { sheet in
            switch sheet
            {
            case .settings: SettingsView()
            case .newChat: NewChatView(initialProjectID: projectID)
            case .pairing: PairConnectionView()
            }
        }
        .onChange(of: model.pendingPairingURL) { presentPendingPairingIfPossible() }
        .onChange(of: searching) { _, open in searchFocused = open }
        .onChange(of: search) { searchExpandedGroups = [:] }
        .onChange(of: model.activeConnectionID)
        { projectID = ""; search = ""; searching = false; searchFocused = false; expandedGroups = [:]; searchExpandedGroups = [:] }
        .onChange(of: scenePhase) { _, phase in model.setForeground(phase == .active); if phase == .active { homeClock = .now } }
        .task { model.setForeground(true); presentPendingPairingIfPossible() }
        .task(id: nextSnoozeWake)
        {
            guard let wake = nextSnoozeWake else { return }
            do { try await Task.sleep(for: .seconds(max(0, wake.timeIntervalSinceNow))) }
            catch { return }
            // Only advances local time. Never sends a wake action or replays input.
            homeClock = .now
        }
    }

    private var nextSnoozeWake: Date?
    {
        let now = Date.now.timeIntervalSince1970 * 1000
        let deadline = model.chats.lazy.filter { $0.familyID == $0.id }
            .map { $0.raw.number("snoozedUntilMs") }.filter { $0 > now }.min()
        return deadline.map { Date(timeIntervalSince1970: $0 / 1000) }
    }

    private func presentRootSheet(_ sheet: RootSheet)
    {
        guard !rootSheetActive, rootSheet == nil else { return }
        rootSheetActive = true
        rootSheet = sheet
    }

    private func presentPendingPairingIfPossible()
    {
        // An existing nested pairing page handles incoming links itself. While
        // Settings or New Chat is open, leave any unconsumed link queued rather
        // than presenting from underneath it. Consume it after actual dismissal.
        guard model.pendingPairingURL != nil else { return }
        presentRootSheet(.pairing)
    }

    private func rootSheetDismissed()
    {
        rootSheet = nil
        rootSheetActive = false
        presentPendingPairingIfPossible()
    }

    private var home: some View
    {
        // One catalogue projection per render. Reuse the same family decision
        // for rows and empty state rather than sorting/grouping again for each.
        let now = max(homeClock, Date.now)
        let matchingIDs: Set<String>? = query.isEmpty ? nil : Set(filteredChats.map(\.id))
        let catalogue = HomeCatalogue.build(chats: model.chats, folders: model.folders,
                                            collections: model.resourceCollections,
                                            projectID: projectID, matchingIDs: matchingIDs, now: now)
        let activityGroups = tab == "active" ? MobileActivityGroup.build(model.chats, now: now, matchingIDs: Set(catalogue.roots.map(\.id))) : []
        return VStack(spacing: 0)
        {
            header.padding(.horizontal, 18).padding(.top, 8).padding(.bottom, 14)
            filters.padding(.bottom, 12)
            if model.connection != nil, !model.connected
            {
                HStack(alignment: .top, spacing: 8)
                {
                    Image(systemName: "wifi.slash")
                    VStack(alignment: .leading, spacing: 5)
                    {
                        Text(model.lastUpdated == nil ? "Connecting to \(model.connection?.name ?? "Universal Agent Manager")…" : "Connection interrupted · Reconnecting…")
                        if let problem = model.connectionProblem { Text(problem).fixedSize(horizontal: false, vertical: true) }
                    }.font(.caption).frame(maxWidth: .infinity, alignment: .leading).accessibilityIdentifier("connectionStatus")
                    VStack(spacing: 4)
                    {
                        Button { Task { await model.refresh(force: true) } } label: {
                            Text("Retry").font(.caption.weight(.medium))
                                .frame(minWidth: 44, minHeight: 44).contentShape(Rectangle())
                        }.accessibilityIdentifier("retryConnection")
                        if model.connectionRequiresPairing
                        {
                            Button { presentRootSheet(.pairing) } label: {
                                Text("Pair again").font(.caption.weight(.medium))
                                    .frame(minWidth: 44, minHeight: 44).contentShape(Rectangle())
                            }.accessibilityIdentifier("pairConnectionAgain")
                        }
                    }
                }.foregroundStyle(.secondary).padding(.horizontal, 20).padding(.vertical, 10)
            }
            if let error = model.error { ErrorStrip(message: error) { model.error = nil } }
            ScrollView
            {
                LazyVStack(spacing: 0)
                {
                    if model.connections.isEmpty
                    {
                        EmptyContent(title: "Your agents, wherever you are", message: "Connect to UAM on your computer to see your projects and continue your chats.", symbol: "laptopcomputer.and.iphone")
                            .padding(.top, 75)
                        Button("Connect a computer", systemImage: "qrcode.viewfinder") { presentRootSheet(.pairing) }
                            .buttonStyle(.borderedProminent).controlSize(.large)
                    }
                    else if model.lastUpdated == nil, !model.connected
                    {
                        let failed = model.connectionRequiresPairing || model.connectionProblem != nil || model.error != nil
                        EmptyContent(title: failed ? "Connection unavailable" : "Connecting to your computer",
                                     message: "Your chats will appear after this phone connects to your computer.",
                                     symbol: "wifi.slash").padding(.top, 65)
                    }
                    else if tab == "active"
                    {
                        if activityGroups.isEmpty
                        {
                            EmptyContent(title: query.isEmpty ? "All quiet" : "No matching active chats",
                                         message: query.isEmpty ? "Running chats, requests for input and new replies appear here." : "Try another name or browse all chats.")
                                .padding(.top, 40)
                            if query.isEmpty
                            { Button("Browse chats") { tab = "chats" }.buttonStyle(.bordered).padding(.bottom, 20) }
                        }
                        ForEach(activityGroups) { group in chatRow(group.root, activity: group.destination, familyCount: group.members.count) }
                        if !catalogue.snoozed.isEmpty
                        {
                            groupHeader("Snoozed Chats", count: catalogue.snoozed.count, key: "snoozed", defaultExpanded: false)
                            if isExpanded("snoozed", default: false)
                            {
                                ForEach(catalogue.snoozed) { chat in browseChatRow(chat, snoozed: true) }
                            }
                        }
                    }
                    else
                    {
                        ForEach(browseRows(catalogue)) { row in browseRow(row) }
                        if catalogue.roots.isEmpty && catalogue.collections.isEmpty && catalogue.workspaces.isEmpty
                        {
                            EmptyContent(title: query.isEmpty ? "No chats yet" : "No matching chats",
                                         message: query.isEmpty ? "Start a chat in one of your projects." : "Try another name or change the project filter.")
                                .padding(.top, 40)
                        }
                    }
                }.id(tab).padding(.horizontal, 19).padding(.bottom, 16)
            }
            .accessibilityIdentifier("homeChatList")
            .scrollDismissesKeyboard(.interactively)
            .refreshable { await model.refresh(force: true) }
        }
        .safeAreaInset(edge: .bottom, spacing: 0)
        {
            HStack
            {
                HStack(spacing: 2)
                {
                    tabButton("Active", symbol: "bubble.left.and.bubble.right.fill", value: "active")
                    tabButton("Chats", symbol: "tray.full", value: "chats")
                }.padding(4).glassEffect(.regular, in: .capsule)
                Spacer(minLength: 18)
                Button { if model.connections.isEmpty { presentRootSheet(.pairing) } else { presentRootSheet(.newChat) } } label: {
                    Image(systemName: "square.and.pencil").font(.system(size: 25)).frame(width: 64, height: 64)
                }.buttonStyle(.plain).glassEffect(.regular.interactive(), in: .circle)
                    .accessibilityLabel("New chat").accessibilityIdentifier("newChat")
            }.padding(.horizontal, 22).padding(.top, 10).padding(.bottom, 9)
        }
    }

    private var header: some View
    {
        HStack(spacing: 10)
        {
            if searching
            {
                HStack(spacing: 10)
                {
                    Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                    TextField("Search chats", text: $search).font(.body).autocorrectionDisabled()
                        .focused($searchFocused).accessibilityIdentifier("chatSearch")
                    if !search.isEmpty { Button { search = "" } label: { Image(systemName: "xmark.circle.fill") }.accessibilityLabel("Clear search") }
                }.padding(.horizontal, 15).frame(height: 48).glassEffect(.regular, in: .capsule)
                RoundButton(title: "Close search", symbol: "xmark") { withAnimation { searching = false; search = "" } }
            }
            else
            {
                HStack(spacing: 7)
                {
                    Image("UAMMark").resizable().scaledToFit().frame(width: 31, height: 31).clipShape(.rect(cornerRadius: 8))
                    Text("UAM").font(.system(size: 23, weight: .semibold))
                }.accessibilityElement(children: .combine).accessibilityLabel("Universal Agent Manager")
                Spacer()
                HStack(spacing: 0)
                {
                    Button { presentRootSheet(.settings) } label: { Image(systemName: "gearshape").font(.system(size: 22)).frame(width: 52, height: 48) }
                        .accessibilityLabel("Settings").accessibilityIdentifier("settings")
                    Button { withAnimation { searching = true } } label: { Image(systemName: "magnifyingglass").font(.system(size: 22)).frame(width: 52, height: 48) }
                        .accessibilityLabel("Search chats")
                }.buttonStyle(.plain).glassEffect(.regular.interactive(), in: .capsule)
            }
        }.frame(minHeight: 48)
    }

    private var filters: some View
    {
        ScrollView(.horizontal, showsIndicators: false)
        {
            HStack(spacing: 9)
            {
                Menu
                {
                    ForEach(model.connections) { connection in
                        Button { model.switchConnection(connection.id) } label: {
                            if model.activeConnectionID == connection.id { Label(connection.name, systemImage: "checkmark") }
                            else { Text(connection.name) }
                        }
                    }
                    Button("Connect a computer", systemImage: "plus") { presentRootSheet(.pairing) }
                } label: {
                    HStack(spacing: 8)
                    {
                        Image(systemName: "laptopcomputer").font(.system(size: 18))
                        if !dynamicTypeSize.isAccessibilitySize { Text(model.connection == nil ? "Connect" : "Computer") }
                        Image(systemName: "chevron.down").font(.system(size: 11))
                    }.font(.subheadline).foregroundStyle(.secondary)
                        .padding(.horizontal, 13).frame(minHeight: 44)
                        .glassEffect(.regular.interactive(), in: .capsule)
                }
                .accessibilityLabel("Choose computer")
                .accessibilityValue(model.connection?.name ?? "No computer connected")
                .accessibilityIdentifier("computerFilter")
                Menu
                {
                    Button("All projects") { projectID = "" }
                    ForEach(model.folders, id: \.selfID) { folder in
                        Button(folder.text("title")) { projectID = folder.text("id") }
                    }
                } label: {
                    if dynamicTypeSize.isAccessibilitySize
                    {
                        HStack(spacing: 8)
                        {
                            Image(systemName: "folder").font(.system(size: 18))
                            Image(systemName: "chevron.down").font(.system(size: 11))
                        }.foregroundStyle(.secondary).padding(.horizontal, 13).frame(minHeight: 44)
                            .glassEffect(.regular.interactive(), in: .capsule)
                    }
                    else { filterLabel(projectID.isEmpty ? "All projects" : model.folderName(projectID)) }
                }.accessibilityLabel("Choose project")
                    .accessibilityValue(projectID.isEmpty ? "All projects" : model.folderName(projectID))
                    .accessibilityIdentifier("projectFilter")
            }.tint(.primary).padding(.horizontal, 18)
        }
    }

    private func filterLabel(_ text: String) -> some View
    {
        HStack(spacing: 8) { Text(text).lineLimit(1); Image(systemName: "chevron.down").font(.caption) }
            .font(.subheadline).foregroundStyle(.secondary).padding(.horizontal, 13).padding(.vertical, 4).frame(minHeight: 44)
            .glassEffect(.regular.interactive(), in: .capsule)
    }

    private var filteredChats: [MobileChat]
    {
        model.chats.filter { chat in
            (query.isEmpty || "\(chat.title) \(model.folderName(chat.folderID)) \(chat.raw.text("workspaceBranchName"))".localizedCaseInsensitiveContains(query))
        }
    }

    private func tabButton(_ title: String, symbol: String, value: String) -> some View
    {
        Button { tab = value } label: {
            VStack(spacing: 4)
            {
                Image(systemName: symbol).font(.system(size: 22)).frame(height: 27)
                Text(title).font(.caption.weight(.medium)).dynamicTypeSize(...DynamicTypeSize.xxxLarge)
            }.foregroundStyle(tab == value ? UAMTheme.accent : .primary)
                .frame(width: 94, height: 54)
                .background { if tab == value { Capsule().fill(UAMTheme.background) } }
        }.buttonStyle(.plain).contentShape(.capsule).accessibilityAddTraits(tab == value ? .isSelected : [])
    }

    private func isExpanded(_ key: String, default defaultValue: Bool = true) -> Bool
    {
        // Searching temporarily reveals matching ancestors without overwriting
        // the phone's own disclosure choices or the desktop's saved layout.
        if !query.isEmpty { return searchExpandedGroups[key] ?? true }
        return expandedGroups[key] ?? defaultValue
    }

    private func groupHeader(_ title: String, count: Int, key: String, symbol: String? = nil, countNoun: String = "chats",
                             defaultExpanded: Bool = true, level: Int = 0) -> some View
    {
        let expanded = isExpanded(key, default: defaultExpanded)
        let noun = count == 1 ? (countNoun == "workspaces" ? "workspace" : "chat") : countNoun
        return Button
        {
            withAnimation(reduceMotion ? nil : .snappy(duration: 0.2))
            {
                if query.isEmpty { expandedGroups[key] = !expanded }
                else { searchExpandedGroups[key] = !expanded }
            }
        } label: {
            HStack(alignment: .top, spacing: 8)
            {
                Image(systemName: expanded ? "chevron.down" : "chevron.right").font(.system(size: 12, weight: .semibold)).frame(width: 12, height: groupFirstLineHeight)
                if let symbol { Image(systemName: symbol).font(.system(size: 18)).frame(width: 18, height: groupFirstLineHeight) }
                VStack(alignment: .leading, spacing: 4)
                {
                    Text(title).font(symbol == nil ? .subheadline.weight(.semibold) : .subheadline.weight(.medium))
                        .multilineTextAlignment(.leading)
                    if dynamicTypeSize.isAccessibilitySize
                    { Text("\(count) \(noun)").font(.caption).foregroundStyle(.secondary) }
                }.frame(maxWidth: .infinity, alignment: .leading)
                if !dynamicTypeSize.isAccessibilitySize
                { Text(countNoun == "chats" ? "\(count)" : "\(count) \(noun)").font(.caption).monospacedDigit().foregroundStyle(.secondary) }
            }.foregroundStyle(Color.primary)
                .padding(.leading, CGFloat(level) * 16).padding(.vertical, 8).frame(minHeight: 44).contentShape(Rectangle())
        }.buttonStyle(.plain).accessibilityIdentifier(key)
            .accessibilityLabel(title).accessibilityValue("\(count) \(noun), \(expanded ? "expanded" : "collapsed")")
            .accessibilityHint(expanded ? "Collapse section" : "Expand section")
    }

    private struct BrowseRow: Identifiable
    {
        enum Kind { case group, chat, empty, more }
        let id: String
        let kind: Kind
        var title = ""
        var count = 0
        var key = ""
        var symbol: String?
        var countNoun = "chats"
        var defaultExpanded = true
        var level = 0
        var chat: MobileChat?
        var showAll = false
    }

    private func browseRows(_ catalogue: HomeCatalogue) -> [BrowseRow]
    {
        // One stable item per visible logical row. Returning a changing number
        // of views per ForEach item breaks lazy layout and disclosure hit frames.
        var rows: [BrowseRow] = []
        func header(_ title: String, _ count: Int, _ key: String, symbol: String? = nil,
                    noun: String = "chats", expanded: Bool = true, level: Int = 0)
        {
            rows.append(BrowseRow(id: "header-" + key, kind: .group, title: title, count: count,
                                  key: key, symbol: symbol, countNoun: noun, defaultExpanded: expanded, level: level))
        }
        func family(_ chat: MobileChat, level: Int, section: String)
        {
            rows.append(BrowseRow(id: "chat-\(section)-\(chat.id)", kind: .chat, level: level, chat: chat))
            // Branch alternatives belong beside their source message. Search may
            // still surface a matching saved branch directly within All Chats.
            guard !query.isEmpty, section != "pinned" else { return }
            for child in (catalogue.membersByRootID[chat.id] ?? []) where child.id != chat.id
            {
                if "\(child.title) \(model.folderName(child.folderID)) \(child.raw.text("workspaceBranchName"))".localizedCaseInsensitiveContains(query)
                { rows.append(BrowseRow(id: "child-\(section)-\(child.id)", kind: .chat, title: chat.title, level: level + 1, chat: child)) }
            }
        }
        func workspace(_ workspace: HomeCatalogue.Workspace, level: Int)
        {
            let key = "workspace-" + workspace.id
            header(workspace.title, workspace.chatCount, key, symbol: "folder", expanded: !workspace.collapsed, level: level)
            guard isExpanded(key, default: !workspace.collapsed) else { return }
            let showAll = !query.isEmpty || expandedGroups["rows-\(workspace.id)"] == true
            for chat in showAll ? workspace.chats : Array(workspace.chats.prefix(5))
            { family(chat, level: level + 1, section: workspace.id) }
            if workspace.chats.count > 5 && query.isEmpty
            {
                rows.append(BrowseRow(id: "more-" + workspace.id, kind: .more, count: workspace.chats.count - 5,
                                      key: workspace.id, level: level + 1, showAll: showAll))
            }
            if workspace.chats.isEmpty
            { rows.append(BrowseRow(id: "empty-" + key, kind: .empty, title: "No chats in this workspace", level: level + 1)) }
        }
        if !catalogue.pinned.isEmpty
        {
            header("Pinned Chats", catalogue.pinned.count, "pinned")
            if isExpanded("pinned") { for chat in catalogue.pinned { family(chat, level: 0, section: "pinned") } }
        }
        header("All Chats", catalogue.roots.count, "all")
        guard isExpanded("all") else { return rows }
        for collection in catalogue.collections
        {
            let key = "collection-" + collection.id
            header(collection.name, collection.workspaces.count, key, symbol: "books.vertical", noun: "workspaces", expanded: !collection.collapsed)
            if isExpanded(key, default: !collection.collapsed)
            {
                for item in collection.workspaces { workspace(item, level: 1) }
                if collection.workspaces.isEmpty
                { rows.append(BrowseRow(id: "empty-" + key, kind: .empty, title: "No workspaces in this collection", level: 1)) }
            }
        }
        for item in catalogue.workspaces { workspace(item, level: 0) }
        if !catalogue.unsorted.isEmpty
        {
            header("Unsorted", catalogue.unsorted.count, "unsorted", symbol: "tray")
            if isExpanded("unsorted") { for chat in catalogue.unsorted { family(chat, level: 1, section: "unsorted") } }
        }
        return rows
    }

    @ViewBuilder private func browseRow(_ row: BrowseRow) -> some View
    {
        switch row.kind
        {
        case .group:
            groupHeader(row.title, count: row.count, key: row.key, symbol: row.symbol,
                        countNoun: row.countNoun, defaultExpanded: row.defaultExpanded, level: row.level)
        case .chat:
            if let chat = row.chat { browseChatRow(chat, level: row.level, branchSource: row.title.isEmpty ? nil : row.title) }
        case .empty:
            emptyGroup(row.title, level: row.level)
        case .more:
            Button(row.showAll ? "Show less" : "Show \(row.count) more")
            { expandedGroups["rows-\(row.key)"] = !row.showAll }
                .font(.caption).frame(minHeight: 44).frame(maxWidth: .infinity, alignment: .leading)
                .padding(.leading, CGFloat(row.level) * 16 + 30).accessibilityIdentifier("more-\(row.key)")
        }
    }

    private func emptyGroup(_ text: String, level: Int) -> some View
    {
        Text(text).font(.caption).foregroundStyle(.secondary)
            .frame(maxWidth: .infinity, alignment: .leading).padding(.leading, CGFloat(level) * 16 + 20).padding(.vertical, 8)
    }

    private func browseChatRow(_ chat: MobileChat, snoozed: Bool = false, level: Int = 0, branchSource: String? = nil) -> some View
    {
        let connectionID = model.activeConnectionID
        return Button { Task { if model.activeConnectionID == connectionID { await model.openChat(chat.id) } } } label: {
            HStack(alignment: .center, spacing: 8)
            {
                ProviderMark(id: chat.providerID, size: 18)
                if chat.pinned { Image(systemName: "pin.fill").font(.system(size: 12)).foregroundStyle(UAMTheme.accent) }
                VStack(alignment: .leading, spacing: 4)
                {
                    Text(chat.title).font(.subheadline.weight(chat.unseen ? .semibold : .regular))
                        .foregroundStyle(.primary).multilineTextAlignment(.leading).lineLimit(dynamicTypeSize.isAccessibilitySize ? nil : 2)
                    if let branchSource
                    { Label("Branch of \(branchSource)", systemImage: "arrow.triangle.branch").font(.caption).foregroundStyle(.secondary) }
                    if snoozed
                    {
                        Text("Wakes \(Date(timeIntervalSince1970: chat.raw.number("snoozedUntilMs") / 1000).formatted(.relative(presentation: .named)))")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }.frame(maxWidth: .infinity, alignment: .leading)
                if !dynamicTypeSize.isAccessibilitySize
                { Text(compactAge(chat.updatedAt)).font(.caption2).foregroundStyle(.secondary).monospacedDigit() }
                if chat.unseen { Circle().fill(UAMTheme.accent).frame(width: 6, height: 6).accessibilityLabel("Unread") }
            }.padding(.leading, CGFloat(level) * 16 + (level > 0 ? 4 : 0)).padding(.trailing, snoozed ? 44 : 0).padding(.vertical, 10)
                .frame(minHeight: 48).contentShape(Rectangle())
        }.buttonStyle(.plain).accessibilityIdentifier("chat-\(chat.id)")
            .contextMenu { chatActions(chat, connectionID: connectionID) }
            .overlay(alignment: .trailing)
            {
                if snoozed && model.supports("setChatSnoozedUntil")
                {
                    Button
                    {
                        Task { if model.activeConnectionID == connectionID { await model.perform("setChatSnoozedUntil", ["chatId": chat.id, "untilMs": 0]) } }
                    } label: { Image(systemName: "sun.max").frame(width: 44, height: 44) }
                        .buttonStyle(.plain).foregroundStyle(UAMTheme.accent)
                        .accessibilityLabel("Wake chat").accessibilityIdentifier("wake-\(chat.id)")
                }
            }
    }

    @ViewBuilder private func chatActions(_ chat: MobileChat, connectionID: String?) -> some View
    {
        Button(chat.pinned ? "Unpin" : "Pin", systemImage: chat.pinned ? "pin.slash" : "pin")
        { Task { if model.activeConnectionID == connectionID { await model.perform("setChatPinned", ["chatId": chat.id, "pinned": !chat.pinned]) } } }
        if model.supports("setChatSnoozedUntil")
        {
            Button(chat.snoozed ? "Wake chat" : "Snooze for one hour", systemImage: chat.snoozed ? "sun.max" : "moon")
            { Task { if model.activeConnectionID == connectionID { await model.perform("setChatSnoozedUntil", ["chatId": chat.id, "untilMs": chat.snoozed ? 0 : Int(Date.now.addingTimeInterval(3600).timeIntervalSince1970 * 1000)]) } } }
        }
    }

    @ViewBuilder private func chatRow(_ chat: MobileChat, activity: MobileChat? = nil, familyCount: Int = 1) -> some View
    {
        let statusChat = activity ?? chat
        let connectionID = model.activeConnectionID
        Button { Task { if model.activeConnectionID == connectionID { await model.openChat(statusChat.id) } } } label: {
            VStack(alignment: .leading, spacing: 7)
            {
                HStack(spacing: 6)
                {
                    Image(systemName: "folder.fill").font(.caption)
                    Text(model.folderName(chat.folderID)).lineLimit(1)
                    Spacer(minLength: 4)
                    if chat.pinned { Image(systemName: "pin").font(.caption2) }
                    if statusChat.active
                    { Text(statusChat.status).foregroundStyle(statusChat.needsInput ? .orange : statusChat.processing || statusChat.unseen ? UAMTheme.accent : .secondary) }
                    else { Text(compactAge(chat.updatedAt)).monospacedDigit().lineLimit(1).accessibilityLabel("Updated \(chat.updatedAt.formatted())") }
                }.font(.caption).foregroundStyle(.secondary)
                Text(chat.title).font(.body.weight(statusChat.unseen ? .semibold : .regular)).foregroundStyle(.primary)
                    .lineLimit(2).multilineTextAlignment(.leading)
                HStack(spacing: 6)
                {
                    if !chat.parentID.isEmpty { Image(systemName: "arrow.triangle.branch").font(.caption2) }
                    Text(chat.raw.text("workspaceBranchName", chat.raw.text("workspaceDirectory").split(separator: "/").last.map(String.init) ?? ""))
                        .font(.system(size: 12, design: .monospaced)).lineLimit(1)
                    Spacer()
                    if familyCount > 1 { Label("\(familyCount)", systemImage: "person.2").font(.caption2) }
                    ProviderMark(id: chat.providerID, size: 16)
                }.foregroundStyle(.secondary)
            }.padding(.vertical, 13).contentShape(.rect)
        }
        .buttonStyle(.plain).accessibilityIdentifier("chat-\(chat.id)")
        .contextMenu { chatActions(chat, connectionID: connectionID) }
        Divider().overlay(.primary.opacity(0.02))
    }

    private func compactAge(_ date: Date) -> String
    {
        guard date != .distantPast else { return "" }
        let seconds = max(0, Int(Date.now.timeIntervalSince(date)))
        if seconds < 60 { return "now" }
        if seconds < 3600 { return "\(seconds / 60)m" }
        if seconds < 86400 { return "\(seconds / 3600)h" }
        if seconds < 604800 { return "\(seconds / 86400)d" }
        return date.formatted(.dateTime.month(.abbreviated).day())
    }
}

extension Dictionary where Key == String, Value == Any
{
    var selfID: String { text("id") }
}

struct ErrorStrip: View
{
    let message: String
    var dismiss: () -> Void
    var body: some View
    {
        HStack(alignment: .top, spacing: 9)
        {
            Image(systemName: "exclamationmark.circle")
            Text(message).font(.caption).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
            Button(action: dismiss) { Image(systemName: "xmark").frame(width: 24, height: 24) }.accessibilityLabel("Dismiss error")
        }.foregroundStyle(.orange).padding(12).background(.orange.opacity(0.07))
    }
}
