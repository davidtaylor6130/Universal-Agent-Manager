import SwiftUI
import PhotosUI
import UniformTypeIdentifiers
import AVFoundation

struct ComposerView: View
{
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Binding var draft: ComposerDraft
    var modelName: String
    var contextID = ""
    var isContextCurrent: (String) -> Bool = { _ in true }
    var forceExpanded = false
    var processing = false
    var sending = false
    var readOnly = false
    var onSend: () -> Void
    var onStop: () -> Void
    var onModels: () -> Void
    var onDraftChanged: () -> Void
    var onError: (String) -> Void
    @State private var focused = false
    @State private var filesOpen = false
    @State private var photosOpen = false
    @State private var cameraOpen = false
    @State private var photoItems: [PhotosPickerItem] = []
    @State private var importing = false
    @State private var attachmentNotice: String?
    @State private var importEpoch = UUID()
    @State private var speech = SpeechInput()
    @State private var dictationTask: Task<Void, Never>?
    @State private var dictationAttempt = UUID()
    @State private var dictationPrefix = ""
    @State private var dictationAppliedText = ""
    @State private var dictationContext = ""

    private var expanded: Bool { speech.isActive || forceExpanded || focused || !draft.text.isEmpty || !draft.attachments.isEmpty }
    private var canSend: Bool { !draft.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || !draft.attachments.isEmpty }

    var body: some View
    {
        VStack(alignment: .leading, spacing: expanded ? 12 : 0)
        {
            if !draft.attachments.isEmpty
            {
                attachmentStrip
                if let attachmentNotice
                { Text(attachmentNotice).font(.caption).foregroundStyle(.secondary).padding(.horizontal, 8) }
            }
            if expanded
            {
                editor.padding(.horizontal, 4).padding(.top, 3)
                HStack(spacing: 10)
                {
                    attachmentButton
                    Spacer(minLength: 0)
                    Button(action: onModels)
                    {
                        HStack(spacing: 5)
                        {
                            Text(modelName.isEmpty ? "Choose model" : shortModelName(modelName)).lineLimit(1)
                            Image(systemName: "chevron.down").font(.caption2)
                        }.font(.subheadline).foregroundStyle(.primary).frame(minHeight: 44)
                    }.buttonStyle(.plain).accessibilityLabel("Model and agent settings")
                    microphone
                    sendButton
                }
            }
            else
            {
                HStack(spacing: 11)
                {
                    attachmentButton
                    editor
                    microphone
                    sendButton
                }
            }
            if importing { ProgressView("Adding attachment…").font(.caption).padding(.horizontal, 8) }
            if speech.isActive
            {
                HStack(spacing: 8)
                {
                    if speech.isStarting || speech.isFinishing { ProgressView().controlSize(.mini) }
                    else { Image(systemName: "waveform").foregroundStyle(UAMTheme.accent) }
                    Text(speech.isStarting ? "Allow microphone and speech access" : speech.isFinishing ? "Finishing dictation…" : "Listening on this iPhone · Tap mic to stop")
                }.font(.caption).foregroundStyle(.secondary).padding(.horizontal, 8).accessibilityElement(children: .combine)
            }
        }
        .padding(.horizontal, 12).padding(.vertical, expanded ? 12 : 5)
        .glassEffect(.regular, in: .rect(cornerRadius: expanded ? 28 : 34))
        .frame(maxWidth: 720)
        .padding(.horizontal, 13).padding(.bottom, 6)
        .frame(maxWidth: .infinity)
        .disabled(readOnly)
        .photosPicker(isPresented: $photosOpen, selection: $photoItems, maxSelectionCount: 8, matching: .images, preferredItemEncoding: .current)
        .fileImporter(isPresented: $filesOpen, allowedContentTypes: [.item], allowsMultipleSelection: true) { result in
            Task { await importFiles(result) }
        }
        .sheet(isPresented: $cameraOpen) { CameraPhotoPicker { image in
            guard isContextCurrent(contextID) else { return }
            guard let data = image.jpegData(compressionQuality: 0.95) else { onError("The camera photo could not be read."); return }
            addAttachment(ComposerAttachment(name: "Photo-\(Int(Date.now.timeIntervalSince1970)).jpg", mimeType: "image/jpeg", data: data))
        }.ignoresSafeArea() }
        .onChange(of: photoItems) { _, items in Task { await importPhotos(items) } }
        .onChange(of: draft.text) {
            if speech.isActive, draft.text != dictationAppliedText { stopDictation() }
            onDraftChanged()
        }
        .onChange(of: speech.transcript) { _, text in applyDictation(text) }
        .onChange(of: speech.error) { _, message in if let message { onError(message); speech.error = nil } }
        .onChange(of: contextID) { attachmentNotice = nil; stopDictation() }
        .onChange(of: readOnly) { if readOnly { stopDictation() } }
        .onChange(of: scenePhase) { _, phase in if phase == .background { stopDictation() } }
        .onReceive(NotificationCenter.default.publisher(for: AVAudioSession.interruptionNotification)) { _ in stopDictation() }
        .onChange(of: draft.attachments.count) { onDraftChanged() }
        .onDisappear { importEpoch = UUID(); importing = false; stopDictation() }
        .animation(reduceMotion ? nil : .snappy(duration: 0.18), value: expanded)
    }

    private var editor: some View
    {
        ZStack(alignment: .topLeading)
        {
            if draft.text.isEmpty
            {
                Text(readOnly ? "Read-only chat" : "Ask anything…").foregroundStyle(.secondary)
                    .font(.system(size: 17)).padding(.top, 3).allowsHitTesting(false)
            }
            GrowingComposerText(text: $draft.text, focused: $focused)
                .frame(minHeight: expanded ? 56 : 27, maxHeight: 120)
                .fixedSize(horizontal: false, vertical: true)
                .accessibilityIdentifier("composerInput")
        }
    }

    private var attachmentButton: some View
    {
        Menu
        {
            Button("Choose files", systemImage: "folder") { filesOpen = true }
            Button("Photo library", systemImage: "photo") { photosOpen = true }
            if UIImagePickerController.isSourceTypeAvailable(.camera)
            { Button("Take photo", systemImage: "camera") { cameraOpen = true } }
        } label: { Image(systemName: "plus").font(.system(size: 24, weight: .light)).frame(width: 36, height: 44) }
        .accessibilityLabel("Add attachment").accessibilityIdentifier("addAttachment").disabled(sending || importing || speech.isActive)
    }

    private var microphone: some View
    {
        Button
        {
            if speech.isStarting { stopDictation() }
            else if speech.isActive { speech.finish() }
            else
            {
                guard dictationTask == nil else { return }
                focused = false
                dictationContext = contextID
                dictationPrefix = draft.text
                dictationAppliedText = draft.text
                let attempt = UUID()
                dictationAttempt = attempt
                dictationTask = Task {
                    defer { if dictationAttempt == attempt { dictationTask = nil } }
                    guard !Task.isCancelled, dictationAttempt == attempt, isContextCurrent(dictationContext) else { return }
                    await speech.start()
                }
            }
        } label: {
            Image(systemName: speech.isRecording ? "mic.fill" : "mic")
                .font(.system(size: 21)).foregroundStyle(speech.isRecording ? UAMTheme.accent : .primary)
                .frame(width: 34, height: 44)
        }.buttonStyle(.plain)
            .accessibilityLabel(speech.isActive ? "Stop dictation" : "Dictate a message on this iPhone")
            .accessibilityIdentifier("dictateMessage")
            .disabled(sending || importing || speech.isFinishing)
    }

    private func stopDictation()
    {
        dictationAttempt = UUID()
        dictationTask?.cancel(); dictationTask = nil
        speech.stop()
    }

    private func applyDictation(_ text: String)
    {
        guard !text.isEmpty else { return }
        guard isContextCurrent(dictationContext), dictationContext == contextID,
              draft.text == dictationAppliedText else { stopDictation(); return }
        let separator = dictationPrefix.isEmpty || dictationPrefix.last?.isWhitespace == true ? "" : " "
        let updated = dictationPrefix + separator + text
        dictationAppliedText = updated
        draft.text = updated
    }

    private var sendButton: some View
    {
        Button
        {
            if processing && !canSend { onStop() }
            else { focused = false; onSend() }
        } label: {
            ZStack
            {
                Circle().fill(processing && !canSend ? Color.red.opacity(0.17) : UAMTheme.accent.opacity(canSend ? 1 : 0.16))
                if sending { ProgressView().tint(.white) }
                else { Image(systemName: processing && !canSend ? "stop.fill" : "arrow.up").font(.system(size: 21, weight: .semibold)).foregroundStyle(processing && !canSend ? .red : canSend ? .white : .secondary) }
            }.frame(width: 36, height: 36).padding(4)
        }.buttonStyle(.plain).disabled(sending || importing || speech.isActive || (!canSend && !processing))
            .accessibilityLabel(processing && !canSend ? "Stop response" : processing ? "Queue message" : "Send message")
            .accessibilityIdentifier("sendMessage")
    }

    private var attachmentStrip: some View
    {
        ScrollView(.horizontal)
        {
            HStack(spacing: 8)
            {
                ForEach(draft.attachments) { attachment in
                    HStack(spacing: 8)
                    {
                        if attachment.isImage, let image = UIImage(data: attachment.data)
                        { Image(uiImage: image).resizable().scaledToFill().frame(width: 38, height: 38).clipShape(.rect(cornerRadius: 7)) }
                        else { Image(systemName: "doc").frame(width: 28) }
                        VStack(alignment: .leading, spacing: 2)
                        {
                            Text(attachment.name).font(.caption).lineLimit(1).truncationMode(.middle)
                            HStack(spacing: 5)
                            {
                                let fileType = (attachment.name as NSString).pathExtension.uppercased()
                                if !fileType.isEmpty
                                { Text(String(fileType.prefix(8))).font(.caption2.weight(.semibold)) }
                                Text(attachment.sizeLabel).font(.caption2)
                            }.foregroundStyle(.secondary)
                        }.frame(maxWidth: 135, alignment: .leading)
                        Button { draft.attachments.removeAll { $0.id == attachment.id }; attachmentNotice = nil } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary).frame(width: 30, height: 40) }.accessibilityLabel("Remove \(attachment.name)")
                    }.padding(8).background(UAMTheme.surface, in: .rect(cornerRadius: 14))
                }
            }
        }.scrollIndicatorsFlash(trigger: draft.attachments.count)
            .accessibilityIdentifier("draftAttachments")
    }

    private func addAttachment(_ attachment: ComposerAttachment)
    {
        do
        {
            draft.attachments = try AttachmentPreparation.appending([attachment], to: draft.attachments)
            onDraftChanged()
        }
        catch { onError(error.localizedDescription) }
    }

    private func importFiles(_ result: Result<[URL], Error>) async
    {
        let epoch = UUID()
        importEpoch = epoch
        let context = contextID
        importing = true
        defer { if importEpoch == epoch { importing = false } }
        do
        {
            for url in try result.get()
            {
                guard importEpoch == epoch, isContextCurrent(context) else { return }
                let prepared = try await Task.detached(priority: .userInitiated) {
                    try AttachmentPreparation.prepare(AttachmentPreparation.readFile(url))
                }.value
                guard importEpoch == epoch, isContextCurrent(context) else { return }
                draft.attachments = try AttachmentPreparation.appending(prepared, to: draft.attachments)
                if prepared.count == 2 { attachmentNotice = AttachmentPreparation.preservationNotice }
                onDraftChanged()
            }
        }
        catch { if importEpoch == epoch, isContextCurrent(context) { onError(error.localizedDescription) } }
    }

    private func importPhotos(_ items: [PhotosPickerItem]) async
    {
        guard !items.isEmpty else { return }
        let epoch = UUID()
        importEpoch = epoch
        let context = contextID
        importing = true
        defer { if importEpoch == epoch { importing = false; photoItems = [] } }
        do
        {
            for item in items
            {
                guard let data = try await item.loadTransferable(type: Data.self) else { throw MobileError.message("This photo could not be loaded. Try another image.") }
                guard importEpoch == epoch, isContextCurrent(context) else { return }
                let type = item.supportedContentTypes.first ?? .image
                let attachment = ComposerAttachment(name: "Image-\(UUID().uuidString.prefix(8)).\(type.preferredFilenameExtension ?? "jpg")", mimeType: type.preferredMIMEType ?? "image/jpeg", data: data)
                let prepared = try await Task.detached(priority: .userInitiated) { try AttachmentPreparation.prepare(attachment) }.value
                guard importEpoch == epoch, isContextCurrent(context) else { return }
                draft.attachments = try AttachmentPreparation.appending(prepared, to: draft.attachments)
                if prepared.count == 2 { attachmentNotice = AttachmentPreparation.preservationNotice }
                onDraftChanged()
            }
        }
        catch { if importEpoch == epoch, isContextCurrent(context) { onError(error.localizedDescription) } }
    }
}

struct GrowingComposerText: UIViewRepresentable
{
    @Binding var text: String
    @Binding var focused: Bool
    func makeCoordinator() -> Coordinator { Coordinator(self) }
    func makeUIView(context: Context) -> UITextView
    {
        let view = UITextView()
        view.font = .preferredFont(forTextStyle: .body)
        view.adjustsFontForContentSizeCategory = true
        view.backgroundColor = .clear
        view.textColor = .label
        view.tintColor = UIColor(UAMTheme.accent)
        view.textContainerInset = UIEdgeInsets(top: 2, left: 0, bottom: 2, right: 0)
        view.textContainer.lineFragmentPadding = 0
        view.isScrollEnabled = false
        view.delegate = context.coordinator
        view.accessibilityLabel = "Message"
        view.accessibilityIdentifier = "composerInput"
        view.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        view.setContentHuggingPriority(.required, for: .vertical)
        return view
    }
    func updateUIView(_ view: UITextView, context: Context)
    {
        context.coordinator.parent = self
        if view.text != text { view.text = text }
        let coordinator = context.coordinator
        if focused != view.isFirstResponder
        {
            Task { @MainActor [weak view, weak coordinator] in
                guard let view, let coordinator else { return }
                if coordinator.parent.focused && !view.isFirstResponder { view.becomeFirstResponder() }
                if !coordinator.parent.focused && view.isFirstResponder { view.resignFirstResponder() }
            }
        }
    }
    func sizeThatFits(_ proposal: ProposedViewSize, uiView: UITextView, context: Context) -> CGSize?
    {
        guard let width = proposal.width, width > 0 else { return nil }
        let size = uiView.sizeThatFits(CGSize(width: width, height: .greatestFiniteMagnitude))
        uiView.isScrollEnabled = size.height > 120
        return CGSize(width: width, height: min(120, max(27, size.height)))
    }
    @MainActor final class Coordinator: NSObject, UITextViewDelegate
    {
        var parent: GrowingComposerText
        init(_ parent: GrowingComposerText) { self.parent = parent }
        func textViewDidChange(_ textView: UITextView) { parent.text = textView.text }
        func textViewDidBeginEditing(_ textView: UITextView) { parent.focused = true }
        func textViewDidEndEditing(_ textView: UITextView) { parent.focused = false }
    }
}

private struct CameraPhotoPicker: UIViewControllerRepresentable
{
    @Environment(\.dismiss) private var dismiss
    var onPhoto: (UIImage) -> Void
    func makeCoordinator() -> Coordinator { Coordinator(self) }
    func makeUIViewController(context: Context) -> UIImagePickerController
    {
        let picker = UIImagePickerController(); picker.sourceType = .camera; picker.delegate = context.coordinator; return picker
    }
    func updateUIViewController(_ controller: UIImagePickerController, context: Context) { }
    @MainActor final class Coordinator: NSObject, UINavigationControllerDelegate, UIImagePickerControllerDelegate
    {
        let parent: CameraPhotoPicker
        init(_ parent: CameraPhotoPicker) { self.parent = parent }
        func imagePickerControllerDidCancel(_ picker: UIImagePickerController) { parent.dismiss() }
        func imagePickerController(_ picker: UIImagePickerController, didFinishPickingMediaWithInfo info: [UIImagePickerController.InfoKey: Any])
        { if let image = info[.originalImage] as? UIImage { parent.onPhoto(image) }; parent.dismiss() }
    }
}
