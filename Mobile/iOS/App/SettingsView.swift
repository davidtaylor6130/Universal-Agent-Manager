import SwiftUI

struct SettingsView: View
{
    @Environment(MobileAppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @AppStorage("uam.appearance") private var appearance = "dark"

    var body: some View
    {
        NavigationStack
        {
            List
            {
                Section("Connections")
                {
                    NavigationLink { EnvironmentsView() } label: { settingsLabel("Environments", symbol: "desktopcomputer", value: "\(model.connections.count)") }.accessibilityIdentifier("settingsEnvironments")
                }
                Section("Usage")
                {
                    NavigationLink { UsageView(initialTab: "limits") } label: { settingsLabel("Limits", symbol: "gauge.with.dots.needle.50percent") }.accessibilityIdentifier("settingsLimits")
                    NavigationLink { UsageView(initialTab: "usage") } label: { settingsLabel("Usage history", symbol: "chart.bar") }
                }
                Section("Interface")
                {
                    Picker(selection: $appearance)
                    {
                        Text("Dark").tag("dark"); Text("Light").tag("light"); Text("System").tag("system")
                    } label: { settingsLabel("Appearance", symbol: "paintbrush") }
                    NavigationLink { PhoneInputSettingsView() } label: { settingsLabel("Keyboard and text", symbol: "keyboard") }
                }
                Section("Main computer")
                {
                    NavigationLink { MainComputerSettingsView(section: .providers) } label: { settingsLabel("Providers", symbol: "circle.hexagongrid") }
                    NavigationLink { MainComputerSettingsView(section: .shared) } label: { settingsLabel("Agents, skills and MCP", symbol: "person.2") }
                    NavigationLink { MainComputerSettingsView(section: .defaults) } label: { settingsLabel("Chat defaults", symbol: "bubble.left") }
                    NavigationLink { MainComputerSettingsView(section: .memory) } label: { settingsLabel("Durable memory", symbol: "brain") }
                    NavigationLink { MainComputerSettingsView(section: .storage) } label: { settingsLabel("Storage cleanup", symbol: "externaldrive") }
                    NavigationLink { MainComputerSettingsView(section: .hosts) } label: { settingsLabel("Execution environments", symbol: "network") }
                }
                Section
                {
                    LabeledContent("iOS app", value: "0.1.0")
                    if !model.appVersion.isEmpty { LabeledContent("Main UAM", value: model.appVersion) }
                    LabeledContent("Connection", value: model.connected ? "Connected" : "Disconnected")
                } footer: { Text("UAM on your computer owns execution and saved chats. Phone appearance and drafts stay on this device.") }
            }
            .scrollContentBackground(.hidden).background(UAMTheme.background)
            .navigationTitle("Settings").navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
        }.presentationDragIndicator(.visible).presentationCornerRadius(34)
    }

    private func settingsLabel(_ title: String, symbol: String, value: String = "") -> some View
    {
        HStack
        {
            Image(systemName: symbol).frame(width: 28).accessibilityHidden(true)
            Text(title)
            Spacer()
            if !value.isEmpty { Text(value).foregroundStyle(.secondary) }
        }.padding(.vertical, 6)
    }
}

struct EnvironmentsView: View
{
    @Environment(MobileAppModel.self) private var model
    @State private var addOpen = false
    @State private var removing: MobileConnection?
    var body: some View
    {
        List
        {
            if model.connections.isEmpty { EmptyContent(title: "Connect your computer", message: "Scan its pairing code or enter its Phone Access address and token.", symbol: "laptopcomputer.and.iphone") }
            ForEach(model.connections) { connection in
                Section
                {
                    HStack(spacing: 12)
                    {
                        Image(systemName: "desktopcomputer").foregroundStyle(.secondary)
                        VStack(alignment: .leading, spacing: 5)
                        {
                            Text(connection.name).font(.headline)
                            Text(connection.address).font(.caption).foregroundStyle(.secondary).lineLimit(2).textSelection(.enabled)
                            Text(model.activeConnectionID == connection.id ? model.connected ? "Connected" : "Reconnecting" : "Saved")
                                .font(.caption).foregroundStyle(model.activeConnectionID == connection.id && model.connected ? .green : .secondary)
                        }
                        Spacer(minLength: 0)
                        if model.activeConnectionID == connection.id { Image(systemName: "checkmark.circle.fill").foregroundStyle(UAMTheme.accent) }
                    }.padding(.vertical, 9)
                    if model.activeConnectionID != connection.id { Button("Connect") { model.switchConnection(connection.id) } }
                    Button("Forget on this phone", role: .destructive) { removing = connection }
                }
            }
        }
        .scrollContentBackground(.hidden).background(UAMTheme.background)
        .navigationTitle("Environments").navigationBarTitleDisplayMode(.inline)
        .toolbar { ToolbarItem(placement: .primaryAction) { Button("Add environment", systemImage: "plus") { addOpen = true } } }
        .sheet(isPresented: $addOpen) { PairConnectionView() }
        .confirmationDialog("Forget \(removing?.name ?? "this computer")?", isPresented: Binding(get: { removing != nil }, set: { if !$0 { removing = nil } }), titleVisibility: .visible)
        { Button("Forget", role: .destructive) { if let id = removing?.id { model.removeConnection(id) }; removing = nil } }
        message: { Text("This removes its saved credential from this phone. Chats and running work remain on your computer.") }
    }
}

private struct PhoneInputSettingsView: View
{
    @Environment(\.openURL) private var openURL
    var body: some View
    {
        List
        {
            Section("Keyboard")
            {
                Text("Return adds a new line. Use the arrow button to send; messages entered while an agent is working are queued.")
                Text("Tap the composer microphone to dictate on this iPhone. Review the text before sending. You can also use your keyboard’s dictation control.")
            }
            Section("Accessibility")
            {
                Text("UAM follows your iPhone’s text size and accessibility preferences, including Reduce Motion.")
                Button("Open iPhone settings") { if let url = URL(string: UIApplication.openSettingsURLString) { openURL(url) } }
            }
        }.scrollContentBackground(.hidden).background(UAMTheme.background).navigationTitle("Keyboard and text").navigationBarTitleDisplayMode(.inline)
    }
}
