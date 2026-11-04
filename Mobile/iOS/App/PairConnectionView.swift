import SwiftUI
import VisionKit
import AVFoundation

struct PairConnectionView: View
{
    @Environment(MobileAppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var flow = PairingFlow()
    @State private var scanning = false
    @State private var scanError: String?
    @State private var closing = false
    @State private var ignoredPairingLink = false
    @FocusState private var focusedField: Field?
    private enum Field { case name, address, token }

    var body: some View
    {
        NavigationStack
        {
            // Keep actions in the ordinary layout tree so result hit areas do
            // not depend on the replaced entry's animated keyboard inset.
            VStack(spacing: 0)
            {
                Group
                {
                    switch flow.phase
                    {
                    case .entry: entry
                    case .checking: checking
                    case .success, .failure: result
                    }
                }.frame(maxWidth: .infinity, maxHeight: .infinity)
                actions
            }
            .background(UAMTheme.background)
            .navigationTitle(flow.phase == .entry ? "Add environment" : flow.phase == .checking ? "Checking connection" : flow.phase == .success ? "Connected" : "Connection failed")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar
            {
                if flow.phase == .entry || flow.phase == .checking
                { ToolbarItem(placement: .cancellationAction) { Button("Cancel") { close() } } }
            }
            .task { consumePendingCode() }
            .onChange(of: model.pendingPairingURL) { consumePendingCode() }
            .onChange(of: flow.phase)
            {
                if flow.phase != .entry { scanning = false; focusedField = nil }
            }
        }.presentationDragIndicator(.visible).presentationCornerRadius(34)
            .interactiveDismissDisabled(flow.phase == .checking)
            .onDisappear { closing = true; flow.invalidate(); scanning = false }
    }

    private var entry: some View
    {
        @Bindable var flow = flow
        return ScrollView
        {
            VStack(alignment: .leading, spacing: 24)
            {
                scanArea
                VStack(alignment: .leading, spacing: 12)
                {
                    Text("Or enter it manually").font(.subheadline).foregroundStyle(.secondary)
                    VStack(spacing: 0)
                    {
                        fieldRow("Name") { TextField("My computer", text: $flow.name).textContentType(.nickname).focused($focusedField, equals: .name).submitLabel(.next).onSubmit { focusedField = .address } }
                        Divider().padding(.leading, 16)
                        fieldRow("Address") { TextField("Address", text: $flow.address, prompt: Text(verbatim: "https://your-computer:58950").foregroundStyle(.secondary)).keyboardType(.URL).textContentType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled().accessibilityIdentifier("pairAddress").focused($focusedField, equals: .address).submitLabel(.next).onSubmit { focusedField = .token } }
                        Divider().padding(.leading, 16)
                        fieldRow("Token") { SecureField("Desktop access token", text: $flow.token).textInputAutocapitalization(.never).autocorrectionDisabled().accessibilityIdentifier("pairToken").focused($focusedField, equals: .token).submitLabel(.go).onSubmit { if flow.canSubmit { submit() } } }
                    }.background(UAMTheme.surface, in: .rect(cornerRadius: 24))
                    Text("On your computer, open Universal Agent Manager → Settings → Defaults → Phone Access. Use its current HTTPS address and access token.")
                        .font(.footnote).foregroundStyle(.secondary)
                    Text("Pairing an existing address updates its saved credential and keeps your local drafts.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }.padding(20)
        }.scrollDismissesKeyboard(.interactively)
    }

    private var checking: some View
    {
        ScrollView
        {
            VStack(spacing: 20)
            {
                ProgressView().progressViewStyle(.circular).controlSize(.large).padding(.top, 36)
                    .accessibilityLabel("Connecting securely and checking Universal Agent Manager")
                Text("Connecting securely and loading your chats…").font(.title2.weight(.semibold)).accessibilityIdentifier("pairChecking")
                Text(flow.checkedName).font(.headline)
                targetAddress
                Text("Keep Universal Agent Manager open and Phone Access enabled on your computer.")
                    .foregroundStyle(.secondary)
                incomingLinkNotice
            }.multilineTextAlignment(.center).frame(maxWidth: .infinity).padding(24)
        }
    }

    private var result: some View
    {
        ScrollView
        {
            VStack(spacing: 20)
            {
                Image(systemName: flow.phase == .success ? "checkmark.circle" : "exclamationmark.circle")
                    .font(.system(size: 46, weight: .light)).foregroundStyle(flow.phase == .success ? UAMTheme.accent : .orange)
                    .accessibilityHidden(true).padding(.top, 30)
                if flow.phase == .success
                {
                    Text("Connected to \(flow.checkedName)").font(.title2.weight(.semibold)).accessibilityIdentifier("pairSuccess")
                    targetAddress
                    Text("This computer is saved on your iPhone.").foregroundStyle(.secondary)
                }
                else if let problem = flow.problem
                {
                    Text(problem.title).font(.title2.weight(.semibold)).accessibilityIdentifier("pairFailure")
                    if !flow.checkedAddress.isEmpty { targetAddress }
                    Text(problem.message).foregroundStyle(.secondary)
                    if let detail = problem.detail
                    { DisclosureGroup("Technical details") { Text(detail).font(.caption.monospaced()).textSelection(.enabled).padding(.top, 8) }.font(.footnote) }
                }
                incomingLinkNotice
            }.multilineTextAlignment(.center).frame(maxWidth: .infinity).padding(24)
        }
    }

    private var targetAddress: some View
    { Text(flow.checkedAddress).font(.callout.monospaced()).foregroundStyle(.secondary).textSelection(.enabled).fixedSize(horizontal: false, vertical: true) }

    @ViewBuilder private var incomingLinkNotice: some View
    {
        if ignoredPairingLink
        { Text("Another pairing link arrived. Close this page, then open that link again.").font(.footnote).foregroundStyle(.secondary).accessibilityIdentifier("pairIgnoredLink") }
    }

    @ViewBuilder private var actions: some View
    {
        if flow.phase != .checking
        {
            VStack(spacing: 10)
            {
                if flow.phase == .entry
                {
                    Button(action: submit)
                    { Label("Check connection", systemImage: "arrow.right").foregroundStyle(.black).frame(maxWidth: .infinity, minHeight: 44).contentShape(.rect) }
                    .buttonStyle(.borderedProminent).disabled(!flow.canSubmit).accessibilityIdentifier("connectEnvironment")
                }
                else
                {
                    Button(action: close)
                    { Text("Close").foregroundStyle(.black).frame(maxWidth: .infinity, minHeight: 44).contentShape(.rect) }
                    .buttonStyle(.borderedProminent).accessibilityIdentifier("pairClose")
                    if flow.phase == .failure
                    {
                        Button { ignoredPairingLink = false; flow.editDetails() } label:
                        { Text("Edit details").foregroundStyle(Color(uiColor: .label)).frame(maxWidth: .infinity, minHeight: 44).contentShape(.rect) }
                        .buttonStyle(.bordered).accessibilityIdentifier("pairEditDetails")
                    }
                }
            }.controlSize(.large).frame(maxWidth: .infinity).padding(.horizontal, 20).padding(.vertical, 14).background(.bar)
        }
    }

    private var scanArea: some View
    {
        VStack(spacing: 14)
        {
            if scanning, DataScannerViewController.isSupported, DataScannerViewController.isAvailable
            { PairCodeScanner(onCode: { code in receiveCode(code); scanning = false }, onError: {
                guard !closing, flow.phase == .entry else { return }
                scanning = false; scanError = "The camera could not scan this code. Try scanning again or enter the connection details manually."
            }).frame(height: 220).clipShape(.rect(cornerRadius: 22)) }
            else
            {
                Image(systemName: "qrcode.viewfinder").font(.system(size: 42, weight: .ultraLight)).foregroundStyle(.secondary).accessibilityHidden(true)
                Button("Scan pairing code", systemImage: "camera") { Task { await startScanning() } }.buttonStyle(.bordered).controlSize(.large)
            }
            Text("Scan the code shown in Universal Agent Manager’s Phone Access settings.").font(.footnote).foregroundStyle(.secondary).multilineTextAlignment(.center)
            if let scanError { Text(scanError).font(.footnote).foregroundStyle(.orange).accessibilityIdentifier("pairScanError") }
        }.padding(22).frame(maxWidth: .infinity).background(UAMTheme.surface, in: .rect(cornerRadius: 28))
    }

    private func fieldRow<Content: View>(_ label: String, @ViewBuilder content: () -> Content) -> some View
    {
        HStack(spacing: 12)
        {
            Text(label).foregroundStyle(.secondary).fixedSize(horizontal: true, vertical: false).frame(minWidth: 64, alignment: .leading)
            content().frame(maxWidth: .infinity)
        }.font(.body).padding(17)
    }

    private func startScanning() async
    {
        guard !closing, flow.phase == .entry else { return }
        guard DataScannerViewController.isSupported else { scanError = "QR scanning is unavailable on this device. Enter the connection details manually."; return }
        let allowed = await AVCaptureDevice.requestAccess(for: .video)
        guard !closing, flow.phase == .entry else { return }
        guard allowed else { scanError = "Camera access is off. Enable it in iPhone Settings or enter the details manually."; return }
        guard DataScannerViewController.isAvailable else { scanError = "The camera is currently unavailable. Enter the details manually or try again."; return }
        scanError = nil; focusedField = nil; scanning = true
    }

    private func submit()
    { guard !closing else { return }; scanError = nil; focusedField = nil; scanning = false; ignoredPairingLink = false; flow.submit { [model] in try await model.addConnection(name: $0.name, address: $0.address, token: $0.token, pairingCertificate: $0.pairingCertificate, clearSavedCertificate: $0.clearSavedCertificate) } }

    private func receiveCode(_ code: String)
    { guard !closing else { return }; scanError = nil; focusedField = nil; scanning = false; ignoredPairingLink = false; flow.acceptCode(code) { [model] in try await model.addConnection(name: $0.name, address: $0.address, token: $0.token, pairingCertificate: $0.pairingCertificate, clearSavedCertificate: $0.clearSavedCertificate) } }

    private func consumePendingCode()
    {
        guard !closing, model.pendingPairingURL != nil else { return }
        if flow.phase != .entry { ignoredPairingLink = true }
        else { scanError = nil; focusedField = nil; scanning = false; ignoredPairingLink = false }
        flow.consumePendingCode(&model.pendingPairingURL) { [model] in try await model.addConnection(name: $0.name, address: $0.address, token: $0.token, pairingCertificate: $0.pairingCertificate, clearSavedCertificate: $0.clearSavedCertificate) }
    }

    private func close() { closing = true; flow.invalidate(); dismiss() }
}

struct PairCodeScanner: UIViewControllerRepresentable
{
    var onCode: (String) -> Void
    var onError: () -> Void
    func makeCoordinator() -> Coordinator { Coordinator(onCode: onCode, onError: onError) }
    func makeUIViewController(context: Context) -> DataScannerViewController
    {
        let scanner = DataScannerViewController(recognizedDataTypes: [.barcode(symbologies: [.qr])], qualityLevel: .balanced, recognizesMultipleItems: false, isHighFrameRateTrackingEnabled: false, isPinchToZoomEnabled: true, isGuidanceEnabled: true, isHighlightingEnabled: true)
        scanner.delegate = context.coordinator
        do { try scanner.startScanning() }
        catch { context.coordinator.scannerFailed() }
        return scanner
    }
    func updateUIViewController(_ scanner: DataScannerViewController, context: Context) { }
    static func dismantleUIViewController(_ scanner: DataScannerViewController, coordinator: Coordinator) { scanner.stopScanning() }
    @MainActor final class Coordinator: NSObject, DataScannerViewControllerDelegate
    {
        let onCode: (String) -> Void
        let onError: () -> Void
        private var delivered = false
        init(onCode: @escaping (String) -> Void, onError: @escaping () -> Void) { self.onCode = onCode; self.onError = onError }
        func scannerFailed()
        {
            guard !delivered else { return }
            delivered = true
            // Scanner startup happens during UIViewController creation. Deliver the
            // fallback after that update, rather than mutating SwiftUI state inside it.
            Task { @MainActor [onError] in onError() }
        }
        func dataScanner(_ scanner: DataScannerViewController, becameUnavailableWithError error: DataScannerViewController.ScanningUnavailable)
        { scannerFailed() }
        func dataScanner(_ scanner: DataScannerViewController, didAdd addedItems: [RecognizedItem], allItems: [RecognizedItem])
        {
            guard !delivered else { return }
            for case .barcode(let barcode) in addedItems
            {
                if let code = barcode.payloadStringValue { delivered = true; onCode(code); return }
            }
        }
    }
}
