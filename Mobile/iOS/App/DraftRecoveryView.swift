import SwiftUI

/// Recovery is offered only for a known unreadable local draft. Its durable
/// owner and navigation token are checked again at each explicit action.
struct DraftRecoveryView: View
{
    @Environment(MobileAppModel.self) private var model
    let context: DraftContext
    let contextID: String
    let isContextCurrent: (DraftContext, String) -> Bool
    var onRecovered: (ComposerDraft) -> Void = { _ in }
    @State private var discardOpen = false
    @State private var discardOriginID: String?

    private var current: Bool
    { context.connectionID == model.activeConnectionID && isContextCurrent(context, contextID) }

    var body: some View
    {
        VStack(alignment: .leading, spacing: 10)
        {
            Label("Saved draft unavailable", systemImage: "exclamationmark.doc")
                .font(.subheadline.weight(.medium))
            Text("This iPhone’s unsent draft could not be read. Retry it, or discard only this local draft and its attachments. Your computer’s chat history is kept.")
                .font(.footnote).foregroundStyle(.secondary)
            ViewThatFits(in: .horizontal)
            {
                HStack(spacing: 12) { retryButton; discardButton }
                VStack(alignment: .leading, spacing: 8) { retryButton; discardButton }
            }
        }
        .padding(14).frame(maxWidth: .infinity, alignment: .leading)
        .background(UAMTheme.surface, in: .rect(cornerRadius: 16))
        .padding(.horizontal, 14)
        .disabled(!current)
        .alert("Discard this iPhone draft?", isPresented: $discardOpen)
        {
            Button("Discard local draft", role: .destructive)
            {
                guard discardOriginID == contextID, current,
                      let recovered = model.discardUnreadableDraft(context: context), current else { return }
                onRecovered(recovered)
            }.accessibilityIdentifier("confirmDiscardUnreadableDraft")
            Button("Keep draft", role: .cancel) { discardOriginID = nil }
        } message: {
            Text("This permanently discards only the unreadable unsent draft and its attachments saved on this iPhone. It does not delete chats or history on your computer.")
        }
        .onChange(of: contextID) { _, _ in discardOpen = false; discardOriginID = nil }
    }

    private var retryButton: some View
    {
        Button
        {
            guard current, let recovered = model.reloadSavedDraft(context: context), current else { return }
            onRecovered(recovered)
        } label: { Label("Retry saved draft", systemImage: "arrow.clockwise").font(.footnote).frame(minHeight: 44) }
        .buttonStyle(.bordered).accessibilityIdentifier("retrySavedDraft")
    }

    private var discardButton: some View
    {
        Button(role: .destructive)
        {
            guard current else { return }
            discardOriginID = contextID
            discardOpen = true
        } label: { Text("Discard unreadable draft").font(.footnote).frame(minHeight: 44) }
        .buttonStyle(.borderless).accessibilityIdentifier("discardUnreadableDraft")
    }
}
