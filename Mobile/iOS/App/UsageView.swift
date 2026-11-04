import SwiftUI
import Charts
import CoreFoundation

struct UsageView: View
{
    @Environment(MobileAppModel.self) private var model
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.dynamicTypeSize) private var dynamicTypeSize
    @State private var tab: String
    @State private var days = 30
    @State private var metric = "cost"
    @State private var history: WireObject = [:]
    @State private var allowances: WireObject = [:]
    @State private var loading = false
    @State private var error: String?
    @State private var refreshAfter = Date.distantPast
    @State private var refreshRevision = 0
    @State private var requestManualRefresh = false
    @State private var loadGeneration = UUID()
    @State private var displayedConnection: String?
    @State private var displayedHistoryDays = 0

    init(initialTab: String = "limits")
    {
        _tab = State(initialValue: initialTab == "usage" ? "usage" : "limits")
    }

    private struct LoadKey: Hashable
    {
        let tab: String
        let days: Int
        let connection: String?
        let foreground: Bool
        let revision: Int
    }

    private var loadKey: LoadKey
    {
        LoadKey(tab: tab, days: tab == "usage" ? days : 0, connection: model.activeConnectionID,
                foreground: scenePhase == .active, revision: refreshRevision)
    }

    var body: some View
    {
        ScrollView
        {
            VStack(alignment: .leading, spacing: 22)
            {
                Picker("Usage view", selection: $tab) { Text("Usage").tag("usage"); Text("Limits").tag("limits") }.pickerStyle(.segmented)
                if displayedConnection == model.activeConnectionID
                {
                    if tab == "usage" { historyContent }
                    else { limitsContent }
                }
                if loading { ProgressView("Refreshing…").frame(maxWidth: .infinity) }
                if let error
                {
                    Label(error, systemImage: "exclamationmark.circle").font(.footnote).foregroundStyle(.orange)
                    Button("Retry") { refreshRevision += 1 }.disabled(loading)
                }
            }.frame(maxWidth: 720).padding(20).frame(maxWidth: .infinity)
        }.background(UAMTheme.background)
            .navigationTitle("Usage").navigationBarTitleDisplayMode(.inline)
            .toolbar
            {
                ToolbarItem(placement: .primaryAction)
                {
                    TimelineView(.periodic(from: .now, by: 1)) { context in
                        let remaining = max(0, Int(ceil(refreshAfter.timeIntervalSince(context.date))))
                        Button("Refresh", systemImage: "arrow.clockwise")
                        {
                            requestManualRefresh = tab == "limits"
                            refreshRevision += 1
                        }
                        .disabled(loading || (tab == "limits" && remaining > 0))
                        .accessibilityHint(tab == "limits" && remaining > 0 ? "Available in \(remaining) seconds" : "Read the latest values from your main computer")
                    }
                }
            }
            .task(id: loadKey) { await monitor(loadKey) }
    }

    private var historyContent: some View
    {
        VStack(alignment: .leading, spacing: 22)
        {
            ViewThatFits(in: .horizontal)
            {
                HStack { periodPicker; metricPicker }
                VStack(alignment: .leading, spacing: 12) { periodPicker; metricPicker }
            }
            if !history.isEmpty, displayedHistoryDays == days
            {
                let totals = history.object("totals")
                VStack(alignment: .leading, spacing: 14)
                {
                    Text(metric == "tokens" ? "Tokens" : totals.number("unpricedTokens") > 0 ? "Known cost" : "Recorded and estimated cost").font(.subheadline).foregroundStyle(.secondary)
                    Text(valueLabel(totals))
                        .font(.system(.largeTitle, design: .default, weight: .semibold)).minimumScaleFactor(0.7).lineLimit(1)
                    if metric == "cost"
                    {
                        Text(totals.number("unpricedTokens") > 0 ? "Excludes \(totals.number("unpricedTokens").formatted(.number.notation(.compactName))) unpriced tokens." : "Includes provider-reported charges and API-rate estimates; this is not your subscription bill.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    if !periods.isEmpty
                    {
                        Chart(Array(periods.enumerated()), id: \.offset) { _, period in
                            BarMark(x: .value("Date", Date(timeIntervalSince1970: period.number("startMs") / 1000)),
                                    y: .value(metric == "tokens" ? "Tokens" : "USD", metric == "tokens" ? period.number("totalTokens") : cost(period)),
                                    width: .ratio(0.72))
                            .foregroundStyle(UAMTheme.accent.gradient).cornerRadius(3)
                            .accessibilityLabel(Date(timeIntervalSince1970: period.number("startMs") / 1000).formatted(date: .abbreviated, time: days == 1 ? .shortened : .omitted))
                            .accessibilityValue(valueLabel(period))
                        }.frame(height: dynamicTypeSize.isAccessibilitySize ? 250 : 190)
                            .chartXAxis { AxisMarks(values: .automatic(desiredCount: 4)) }
                            .chartYAxis { AxisMarks(position: .leading, values: .automatic(desiredCount: 4)) }
                            .accessibilityLabel("Usage over the selected period")
                    }
                    if !history.text("collectionError").isEmpty
                    { Label(history.text("collectionError"), systemImage: "exclamationmark.circle").font(.caption).foregroundStyle(.orange) }
                    if !history.flag("complete") || history.flag("collecting")
                    { Label(history.flag("collecting") ? "Still collecting history" : "Partial history coverage", systemImage: "clock").font(.caption).foregroundStyle(.secondary) }
                }.padding(20).background(UAMTheme.surface, in: .rect(cornerRadius: 27))
                Text("Providers").font(.subheadline).foregroundStyle(.secondary)
                VStack(spacing: 22)
                {
                    ForEach(history.objects("providers"), id: \.usageProviderID) { provider in
                        let value = metric == "tokens" ? provider.number("totalTokens") : cost(provider)
                        let total = metric == "tokens" ? totals.number("totalTokens") : cost(totals)
                        VStack(alignment: .leading, spacing: 9)
                        {
                            HStack
                            {
                                ProviderMark(id: provider.text("providerId"))
                                Text(providerName(provider.text("providerId")))
                                Spacer()
                                Text(valueLabel(provider)).monospacedDigit()
                            }.font(.subheadline)
                            if validNumber(totals, metric == "tokens" ? "totalTokens" : "reportedCostUsd") != nil || (metric == "cost" && validNumber(totals, "estimatedCostUsd") != nil)
                            { ProgressView(value: total > 0 ? min(1, value / total) : 0).tint(UAMTheme.accent) }
                            Text(validNumber(provider, "totalTokens").map { "\($0.formatted(.number.notation(.compactName))) tokens" } ?? "Token count unavailable")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }.padding(20).background(UAMTheme.surface, in: .rect(cornerRadius: 27))
            }
            else if !loading { EmptyContent(title: "Usage is unavailable", message: "Connect to a main UAM instance that supports shared usage history.", symbol: "chart.bar") }
        }
    }

    private var limitsContent: some View
    {
        VStack(alignment: .leading, spacing: 23)
        {
            TimelineView(.periodic(from: .now, by: 1)) { context in
                if refreshAfter > context.date
                {
                    Text("Refresh available in \(max(1, Int(ceil(refreshAfter.timeIntervalSince(context.date)))))s")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            ForEach(allowances.objects("providers"), id: \.usageProviderID) { provider in
                VStack(alignment: .leading, spacing: 13)
                {
                    HStack(spacing: 8) { ProviderMark(id: provider.text("providerId")); Text(providerName(provider.text("providerId"))).font(.headline) }
                    let windows = limitWindows(provider)
                    if windows.isEmpty
                    {
                        VStack(alignment: .leading, spacing: 9)
                        {
                            Text(provider.text("status") == "loading" ? "Checking allowance…" : provider.text("status") == "ready" ? "No allowance window reported" : "Allowance unavailable").font(.subheadline)
                            Text(provider.text("error").isEmpty ? "This provider has not reported an account limit." : provider.text("error")).font(.caption).foregroundStyle(.secondary)
                        }.padding(18).frame(maxWidth: .infinity, alignment: .leading).background(UAMTheme.surface, in: .rect(cornerRadius: 25))
                    }
                    ForEach(Array(windows.enumerated()), id: \.offset) { _, window in
                        let remaining = validNumber(window, "usedPercent").flatMap { (0...100).contains($0) ? 100 - $0 : nil }
                        VStack(alignment: .leading, spacing: 13)
                        {
                            Text(window.text("label", "Reported window")).font(.subheadline)
                            HStack(alignment: .firstTextBaseline, spacing: 7)
                            {
                                Text(remaining.map { "\($0.formatted(.number.precision(.fractionLength(0...1))))%" } ?? "Unavailable")
                                    .font(.system(.largeTitle, design: .default, weight: .semibold)).minimumScaleFactor(0.7)
                                if remaining != nil { Text("left").font(.subheadline).foregroundStyle(.secondary) }
                            }
                            if let remaining
                            {
                                ProgressView(value: remaining, total: 100).tint(remaining <= 5 ? .red : remaining <= 20 ? .orange : UAMTheme.accent)
                                    .accessibilityLabel("Allowance remaining").accessibilityValue("\(remaining.formatted(.number.precision(.fractionLength(0...1)))) percent")
                            }
                            if window.number("resetsAt") > 0
                            {
                                let reset = Date(timeIntervalSince1970: window.number("resetsAt"))
                                if reset <= .now
                                { Text("Provider reset time has passed; awaiting update").font(.caption).foregroundStyle(.secondary) }
                                else
                                { HStack(spacing: 5) { Image(systemName: "arrow.clockwise"); Text("Resets"); Text(reset, style: .relative) }.font(.caption).foregroundStyle(.secondary) }
                            }
                            else { Text("Reset time unavailable").font(.caption).foregroundStyle(.secondary) }
                            if provider.text("status") == "loading" { Text("Updating cached allowance…").font(.caption).foregroundStyle(.secondary) }
                            if !provider.text("error").isEmpty
                            { Text("\(provider.text("error")) Showing cached allowance.").font(.caption).foregroundStyle(.orange) }
                            if provider.object("limits").number("updatedAt") > 0
                            { Text("As of \(Date(timeIntervalSince1970: provider.object("limits").number("updatedAt") / 1000).formatted(date: .abbreviated, time: .shortened))").font(.caption2).foregroundStyle(.secondary) }
                        }.padding(20).frame(maxWidth: .infinity, alignment: .leading).background(UAMTheme.surface, in: .rect(cornerRadius: 27))
                    }
                }
            }
            if allowances.objects("providers").isEmpty, !loading { EmptyContent(title: "Limits are unavailable", message: "Connect to your main UAM computer to read its provider allowances.", symbol: "gauge.with.dots.needle.50percent") }
        }
    }

    private var periods: [WireObject]
    {
        (days == 1 ? history.objects("hours") : history.objects("days")).filter { row in
            guard validNumber(row, "startMs") != nil else { return false }
            return metric == "tokens" ? validNumber(row, "totalTokens") != nil : validNumber(row, "reportedCostUsd") != nil || validNumber(row, "estimatedCostUsd") != nil
        }
    }
    private func cost(_ row: WireObject) -> Double { (validNumber(row, "reportedCostUsd") ?? 0) + (validNumber(row, "estimatedCostUsd") ?? 0) }
    private func limitWindows(_ provider: WireObject) -> [WireObject]
    {
        if !provider.objects("windows").isEmpty { return provider.objects("windows") }
        let limits = provider.object("limits")
        return ["primary", "secondary"].compactMap { key in
            var value = limits.object(key)
            guard value["usedPercent"] is NSNumber else { return nil }
            value["label"] = provider.text(key == "primary" ? "primaryLabel" : "secondaryLabel", key == "primary" ? "Primary window" : "Secondary window")
            return value
        }
    }

    private var periodPicker: some View
    {
        Picker("Period", selection: $days) { Text("24h").tag(1); Text("7d").tag(7); Text("30d").tag(30); Text("90d").tag(90) }.pickerStyle(.segmented)
    }

    private var metricPicker: some View
    {
        Picker("Metric", selection: $metric) { Text("Cost").tag("cost"); Text("Tokens").tag("tokens") }.pickerStyle(.menu).fixedSize()
    }

    private func validNumber(_ row: WireObject, _ key: String) -> Double?
    {
        guard let value = row[key] as? NSNumber, CFGetTypeID(value) != CFBooleanGetTypeID(), value.doubleValue.isFinite, value.doubleValue >= 0 else { return nil }
        return value.doubleValue
    }

    private func valueLabel(_ row: WireObject) -> String
    {
        if metric == "tokens"
        { return validNumber(row, "totalTokens")?.formatted(.number.notation(.compactName)) ?? "Unavailable" }
        guard validNumber(row, "reportedCostUsd") != nil || validNumber(row, "estimatedCostUsd") != nil else { return "Unavailable" }
        return cost(row).formatted(.currency(code: "USD"))
    }

    /// One cancellable reader owns the visible tab. There is no loading guard that can discard
    /// a newly selected period while an obsolete request is still unwinding.
    private func monitor(_ key: LoadKey) async
    {
        guard !Task.isCancelled, loadKey == key else { return }
        let generation = UUID()
        loadGeneration = generation
        guard key.foreground else { loading = false; return }
        if displayedConnection != key.connection
        {
            history = [:]; allowances = [:]; refreshAfter = .distantPast
            displayedConnection = key.connection; displayedHistoryDays = 0
        }
        guard key.connection != nil else { loading = false; return }
        let manual = requestManualRefresh && key.tab == "limits" && refreshAfter <= .now
        requestManualRefresh = false
        error = nil
        defer { if loadGeneration == generation { loading = false } }
        let now = Int64(Date.now.timeIntervalSince1970 * 1000)
        let payload: WireObject = ["executionHostId": "*", "refreshId": UUID().uuidString,
                                   "startMs": now - Int64(key.days) * 86400000, "endMs": now, "windowDays": key.days]
        var firstRead = true
        while isCurrent(key, generation)
        {
            loading = firstRead || (key.tab == "usage" && (history.isEmpty || displayedHistoryDays != key.days))
            do
            {
                let response: WireObject
                let delay: Double
                if key.tab == "limits"
                {
                    response = try await model.request("getProviderAllowances", ["executionHostId": "local", "refresh": firstRead && manual])
                    guard isCurrent(key, generation) else { return }
                    allowances = response
                    refreshAfter = .now.addingTimeInterval(min(30, max(0, response.number("refreshAfterMs") / 1000)))
                    delay = response.flag("refreshing") || response.objects("providers").contains(where: { $0.text("status") == "loading" }) ? 1 : 30
                }
                else
                {
                    response = try await loadHistory(payload, key: key, generation: generation)
                    guard isCurrent(key, generation) else { return }
                    if response.flag("pending")
                    {
                        error = response.text("collectionError").isEmpty ? nil : response.text("collectionError")
                        loading = error == nil
                        delay = error == nil ? 0.5 : 30
                    }
                    else
                    {
                        history = response; displayedHistoryDays = key.days
                        loading = false
                        delay = response.flag("collecting") || response.object("pricing").flag("refreshing") ? 2 : 30
                    }
                }
                if !response.flag("pending") { error = nil; loading = false }
                firstRead = false
                try await Task.sleep(for: .seconds(delay))
            }
            catch
            {
                guard isCurrent(key, generation), !(error is CancellationError) else { return }
                self.error = error.localizedDescription; loading = false
                // Read failures preserve the last snapshot and retry quietly; never start
                // another account acquisition or replay a user refresh automatically.
                firstRead = false
                do { try await Task.sleep(for: .seconds(30)) } catch { return }
            }
        }
    }

    private func isCurrent(_ key: LoadKey, _ generation: UUID) -> Bool
    {
        !Task.isCancelled && loadGeneration == generation && loadKey == key
    }

    private func loadHistory(_ payload: WireObject, key: LoadKey, generation: UUID) async throws -> WireObject
    {
        var response = try await model.request("getUsageHistory", payload)
        guard isCurrent(key, generation) else { throw CancellationError() }
        guard response.number("usagePage") == 1 else { return response }
        var bytes = Data()
        let snapshot = response.text("snapshotId")
        let refreshID = payload.text("refreshId")
        let expected = Int(response.number("totalBytes"))
        guard !snapshot.isEmpty, expected > 0, expected <= 32 * 1024 * 1024 else
        { throw MobileError.message("Usage history is too large or invalid to load on this phone.") }
        while true
        {
            guard isCurrent(key, generation) else { throw CancellationError() }
            guard response.text("snapshotId") == snapshot, response.text("refreshId") == refreshID,
                  Int(response.number("totalBytes")) == expected, Int(response.number("offset")) == bytes.count else
            { throw MobileError.message("Usage history changed while loading. Refresh to try again.") }
            let chunk = Data(response.text("chunk").utf8)
            guard !chunk.isEmpty, bytes.count + chunk.count <= expected else
            { throw MobileError.message("Usage history returned an invalid page.") }
            bytes.append(chunk)
            guard let offset = response["nextOffset"] as? NSNumber else { break }
            guard offset.intValue == bytes.count, bytes.count < expected else
            { throw MobileError.message("Usage history returned an invalid cursor.") }
            var next = payload; next["snapshotId"] = snapshot; next["offset"] = offset.intValue
            response = try await model.request("getUsageHistory", next)
        }
        guard bytes.count == expected, let result = try JSONSerialization.jsonObject(with: bytes) as? WireObject else
        { throw MobileError.message("Usage history is incomplete. Refresh to try again.") }
        return result
    }

}

private extension Dictionary where Key == String, Value == Any
{
    var usageProviderID: String { text("providerId") }
}
