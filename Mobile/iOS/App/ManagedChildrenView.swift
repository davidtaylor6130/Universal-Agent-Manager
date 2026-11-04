import SwiftUI
import UAMKit

struct ManagedChildrenView: View
{
    @Environment(MobileAppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase
    let chatID: String
    let contextID: String
    let parentName: String
    @State private var session: ManagedChildSession?
    @State private var inputOpen = false
    @State private var inputHidden = false
    @State private var presentationID: UUID?

    var body: some View
    {
        NavigationStack
        {
            Group
            {
                if let session
                {
                    if let run = session.selected { conversation(session, run: run).id([run.id, run.transcriptID]) }
                    else { catalogue(session) }
                }
                else { ContentUnavailableView("Subagents unavailable", systemImage: "person.2", description: Text(model.managedChildReaderError ?? "This computer must advertise managed-child reads.")) }
            }
            .background(UAMTheme.background)
            .navigationTitle(session?.selected?.name ?? "Subagents").navigationBarTitleDisplayMode(.inline)
            .toolbar
            {
                ToolbarItem(placement: .cancellationAction)
                { Button("Return to parent chat") { dismiss() }.accessibilityIdentifier("returnFromManagedChild") }
                ToolbarItemGroup(placement: .primaryAction)
                {
                    if let session, session.selected != nil
                    {
                        Button("All subagents") { session.showList(); inputOpen = false; inputHidden = false; Task { await session.refresh() } }
                        Button("Refresh conversation", systemImage: "arrow.clockwise") { Task { await session.refresh() } }
                            .disabled(session.reading || !model.connected)
                    }
                }
            }
            .safeAreaInset(edge: .bottom)
            {
                if let session, let run = session.selected { followup(session, run: run) }
            }
            .task(id: scenePhase)
            {
                guard !Task.isCancelled else { return }
                session = model.managedChildrenSession(chatID: chatID, contextID: contextID)
                guard scenePhase == .active else { if let presentationID { session?.deactivate(presentationID) }; return }
                guard let session else { return }
                let presentation = session.activate(); presentationID = presentation
                defer { session.deactivate(presentation) }
                while !Task.isCancelled, model.draftContextID == contextID
                {
                    await session.refresh()
                    do { try await Task.sleep(for: session.error == nil ? .seconds(5) : .seconds(15)) } catch { return }
                }
            }
            .onDisappear { if let presentationID { session?.deactivate(presentationID) } }
            .onChange(of: model.draftContextID) { dismiss() }
        }
        .presentationDragIndicator(.visible).presentationCornerRadius(34)
    }

    private func catalogue(_ session: ManagedChildSession) -> some View
    {
        List
        {
            Section
            {
                Text("Subagents of \(parentName)").font(.footnote).foregroundStyle(.secondary)
                if let error = session.error { Text(error).font(.footnote).foregroundStyle(.red) }
                if session.reading { ProgressView("Reading subagents…") }
                if session.runs.isEmpty, !session.reading
                { Text(session.stale ? "No accepted subagent listing is available." : "No managed subagents.").foregroundStyle(.secondary) }
                ForEach(session.runs) { run in
                    VStack(alignment: .leading, spacing: 8)
                    {
                        Button
                        {
                            session.select(run); inputOpen = false; inputHidden = false
                            Task { await session.refresh() }
                        } label: {
                            HStack(spacing: 12)
                            {
                                ProviderMark(id: run.providerID)
                                VStack(alignment: .leading, spacing: 4)
                                {
                                    Text(run.name).foregroundStyle(.primary).lineLimit(2)
                                    Text(run.statusLabel(stale: session.displayLastReported)).font(.caption).foregroundStyle(.secondary)
                                    timing(session, run: run)
                                    if !run.summary.isEmpty { Text(run.summary).font(.caption).foregroundStyle(.secondary).lineLimit(2) }
                                }
                                Spacer(); Image(systemName: "chevron.right").font(.caption).foregroundStyle(.secondary)
                            }.frame(minHeight: 44).contentShape(Rectangle())
                        }.buttonStyle(.plain).disabled(!session.canOpen(run))
                            .accessibilityLabel("Open subagent: \(run.name)")
                        stop(session, run: run)
                    }
                }
            }
            Section
            {
                HStack
                {
                    Button { Task { await session.showPage(max(0, session.offset - 20)) } } label: {
                        Text("Previous").frame(minWidth: 44, minHeight: 44).contentShape(Rectangle())
                    }.buttonStyle(.borderless).disabled(session.offset == 0 || session.reading)
                    Spacer()
                    Text("\(session.totalCount) subagents").font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Button { if let next = session.nextOffset { Task { await session.showPage(next) } } } label: {
                        Text("Next").frame(minWidth: 44, minHeight: 44).contentShape(Rectangle())
                    }.buttonStyle(.borderless).disabled(session.nextOffset == nil || session.reading)
                }.frame(minHeight: 44)
            }
        }.scrollContentBackground(.hidden)
    }

    private func conversation(_ session: ManagedChildSession, run: ManagedChildRun) -> some View
    {
        ScrollView
        {
            LazyVStack(alignment: .leading, spacing: 24)
            {
                VStack(alignment: .leading, spacing: 8)
                {
                    Text(run.name).font(.title3.weight(.semibold)).accessibilityAddTraits(.isHeader)
                    Text("Subagent of \(parentName)").font(.footnote).foregroundStyle(.secondary)
                    Text(run.statusLabel(stale: session.displayLastReported || session.transcript?.lastReported == true)).font(.caption).foregroundStyle(.secondary)
                    timing(session, run: run)
                    Text("\(providerName(run.providerID)) · \(run.modelID)").font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                    if let error = session.error { Text(error).font(.footnote).foregroundStyle(.red) }
                    if session.reading { ProgressView("Reading conversation…") }
                    stop(session, run: run)
                }
                if let transcript = session.transcript
                {
                    if transcript.messages.isEmpty { Text("No stored messages yet.").foregroundStyle(.secondary) }
                    ForEach(Array(transcript.messages.enumerated()), id: \.offset) { index, value in
                        if let raw = wire(value)
                        {
                            MessageContent(message: MobileMessage(index: index, raw: raw), chatID: run.transcriptID, contextID: contextID,
                                           branchesBusy: false, onSelectBranch: { _ in }, storedChild: true).id(index)
                        }
                        else { Text("This stored message could not be read.").font(.footnote).foregroundStyle(.secondary).id(index) }
                    }
                }
                else if !session.reading { Text("The stored conversation is unavailable. Refresh to try again.").foregroundStyle(.secondary) }
            }.scrollTargetLayout().padding(20).frame(maxWidth: 720).frame(maxWidth: .infinity)
        }
        .scrollPosition(id: Binding<Int?>(get: { session.readingPositions[run.transcriptID] }, set: { if let position = $0 { session.readingPositions[run.transcriptID] = position } }), anchor: .top)
    }

    @ViewBuilder private func timing(_ session: ManagedChildSession, run: ManagedChildRun) -> some View
    {
        let stale = session.displayLastReported || session.transcript?.lastReported == true
        if run.active, !stale
        {
            TimelineView(.periodic(from: .now, by: 1)) { timeline in
                HStack(spacing: 8) { ProgressView().controlSize(.mini); Text(run.elapsed(at: timeline.date, stale: false)) }
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        else { Text(run.elapsed(at: .now, stale: true)).font(.caption).foregroundStyle(.secondary) }
    }

    @ViewBuilder private func stop(_ session: ManagedChildSession, run: ManagedChildRun) -> some View
    {
        let key = session.key(run)
        if run.canCancel || session.stops[key] != nil
        {
            Button(role: .destructive) { Task { await session.stop(run) } } label: {
                Label(session.stops[key] == .uncertain ? "Retry Stop" : "Stop subagent", systemImage: "stop.circle")
                    .frame(minHeight: 44).contentShape(Rectangle())
            }.buttonStyle(.borderless).disabled(!session.canStop(run))
            if let feedback = session.stopFeedback[key] { Text(feedback).font(.caption).foregroundStyle(.secondary) }
        }
    }

    @ViewBuilder private func followup(_ session: ManagedChildSession, run: ManagedChildRun) -> some View
    {
        let entry = session.draft(run)
        let retained = entry.map { !$0.text.isEmpty || $0.request != nil || !$0.feedback.isEmpty } ?? false
        VStack(alignment: .leading, spacing: 8)
        {
            if !inputHidden, let entry, inputOpen || retained
            {
                HStack
                {
                    Text("Follow-up message").font(.footnote.weight(.medium))
                    Spacer()
                    Button { inputHidden = true; inputOpen = false } label: {
                        Text("Hide input").frame(minHeight: 44).contentShape(Rectangle())
                    }
                }
                TextEditor(text: Binding(get: { entry.text }, set: { entry.edit($0) })).frame(minHeight: 70, maxHeight: 130)
                    .accessibilityLabel("Follow-up message").scrollContentBackground(.hidden)
                Text("Text only, up to 32 KiB.").font(.caption).foregroundStyle(.secondary)
                if !entry.feedback.isEmpty { Text(entry.feedback).font(.caption).foregroundStyle(.secondary) }
                if entry.phase == .pending { ProgressView("Recording follow-up work…") }
                else if entry.phase == .uncertain
                {
                    Button { Task { await session.sendFollowup(run, retry: true) } } label: {
                        Text("Retry same request").frame(minHeight: 44).contentShape(Rectangle())
                    }.disabled(!session.canFollowup(run, retry: true))
                }
                else
                {
                    Button { Task { await session.sendFollowup(run) } } label: {
                        Text("Send follow-up").frame(minHeight: 44).contentShape(Rectangle())
                    }.disabled(!session.canFollowup(run) || !ManagedFollowupRequest.validText(entry.text))
                }
            }
            else if retained || session.canFollowup(run)
            {
                Button { if session.beginFollowup(run) != nil { inputOpen = true; inputHidden = false } } label: {
                    Text("Follow up").frame(minHeight: 44).contentShape(Rectangle())
                }
            }
            else { Text(run.followup?.unavailableReason ?? "Follow-up is unavailable for this child.").font(.caption).foregroundStyle(.secondary) }
        }.padding(.horizontal, 20).padding(.bottom, 10).frame(maxWidth: 720).frame(maxWidth: .infinity).background(UAMTheme.background)
    }

    private func wire(_ value: JSONValue) -> WireObject?
    { (try? JSONSerialization.jsonObject(with: JSONEncoder().encode(value))) as? WireObject }
}
