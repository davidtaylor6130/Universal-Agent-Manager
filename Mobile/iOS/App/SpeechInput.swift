import Foundation
import Observation
import Speech
import AVFoundation

/// Audio callbacks execute off the main actor; the request feed is explicitly locked.
private final class SpeechAudioFeed: @unchecked Sendable
{
    private let lock = NSLock()
    private let request: SFSpeechAudioBufferRecognitionRequest
    private var ended = false

    init(_ request: SFSpeechAudioBufferRecognitionRequest) { self.request = request }
    func append(_ buffer: AVAudioPCMBuffer)
    { lock.withLock { if !ended { request.append(buffer) } } }
    func end()
    { lock.withLock { if !ended { ended = true; request.endAudio() } } }
}

@MainActor @Observable
final class SpeechInput
{
    private(set) var isStarting = false
    private(set) var isRecording = false
    private(set) var isFinishing = false
    private(set) var transcript = ""
    var error: String?
    var isActive: Bool { isStarting || isRecording || isFinishing }

    @ObservationIgnored private let engine = AVAudioEngine()
    @ObservationIgnored private var recognizer: SFSpeechRecognizer?
    @ObservationIgnored private var recognitionTask: SFSpeechRecognitionTask?
    @ObservationIgnored private var feed: SpeechAudioFeed?
    @ObservationIgnored private var deadline: Task<Void, Never>?
    @ObservationIgnored private var epoch = UUID()
    @ObservationIgnored private var installedTap = false
    @ObservationIgnored private var activatedSession = false

    func start() async
    {
        guard !isActive, !Task.isCancelled else { return }
        stop()
        let stamp = epoch
        isStarting = true; transcript = ""; error = nil
        guard let recognizer = SFSpeechRecognizer(locale: .current), recognizer.supportsOnDeviceRecognition else
        {
            fail("On-device dictation is unavailable for this iPhone or language. Use the microphone on your iPhone keyboard instead.")
            return
        }
        let authorization = await withCheckedContinuation { continuation in
            SFSpeechRecognizer.requestAuthorization { continuation.resume(returning: $0) }
        }
        guard epoch == stamp, !Task.isCancelled else { return }
        guard authorization == .authorized else
        {
            fail(authorization == .restricted
                 ? "Speech recognition is restricted on this iPhone. You can type your message instead."
                 : "Speech recognition permission is off. Enable Speech Recognition for UAM in iPhone Settings, or type your message.")
            return
        }
        let microphoneAllowed = await AVAudioApplication.requestRecordPermission()
        guard epoch == stamp, !Task.isCancelled else { return }
        guard microphoneAllowed else
        {
            fail("Microphone permission is off. Enable Microphone for UAM in iPhone Settings, or type your message.")
            return
        }
        guard recognizer.isAvailable else
        {
            fail("On-device dictation is currently unavailable. Try again later or use your iPhone keyboard.")
            return
        }
        self.recognizer = recognizer
        do
        {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.record, mode: .measurement, options: .duckOthers)
            try session.setActive(true, options: .notifyOthersOnDeactivation)
            activatedSession = true
            let request = SFSpeechAudioBufferRecognitionRequest()
            request.requiresOnDeviceRecognition = true
            request.shouldReportPartialResults = true
            request.addsPunctuation = true
            let feed = SpeechAudioFeed(request)
            self.feed = feed
            let input = engine.inputNode
            let format = input.outputFormat(forBus: 0)
            guard format.sampleRate > 0, format.channelCount > 0 else
            {
                fail("No microphone input is available. Check your audio connection and try again.")
                return
            }
            input.installTap(onBus: 0, bufferSize: 1024, format: format) { buffer, _ in feed.append(buffer) }
            installedTap = true
            recognitionTask = recognizer.recognitionTask(with: request) { [weak self] result, failure in
                // Copy only Sendable values from framework callbacks before actor hopping.
                let text = result?.bestTranscription.formattedString
                let final = result?.isFinal == true
                let failed = failure != nil
                Task { @MainActor [weak self] in
                    guard let self, self.epoch == stamp else { return }
                    if let text { self.transcript = text }
                    if final { self.stop() }
                    else if failed
                    { self.fail("Dictation stopped. Any recognized text remains in your draft; you can continue typing or try again.") }
                }
            }
            engine.prepare()
            try engine.start()
            isStarting = false; isRecording = true
            deadline = Task { [weak self] in
                do { try await Task.sleep(for: .seconds(55)) } catch { return }
                self?.finish()
            }
        }
        catch { fail("The microphone could not start. Check your audio connection and try again, or use your iPhone keyboard.") }
    }

    /// Stop capturing immediately and briefly allow a final transcription to arrive.
    func finish()
    {
        guard isRecording else { stop(); return }
        stopCapture()
        isRecording = false; isFinishing = true
        feed?.end()
        recognitionTask?.finish()
        deadline?.cancel()
        deadline = Task { [weak self] in
            do { try await Task.sleep(for: .seconds(2)) } catch { return }
            self?.stop()
        }
    }

    /// Navigation/background cancellation never changes the text already transcribed.
    func stop()
    {
        epoch = UUID()
        deadline?.cancel(); deadline = nil
        stopCapture()
        feed?.end(); feed = nil
        recognitionTask?.cancel(); recognitionTask = nil; recognizer = nil
        isStarting = false; isRecording = false; isFinishing = false
    }

    private func stopCapture()
    {
        if engine.isRunning { engine.stop() }
        if installedTap { engine.inputNode.removeTap(onBus: 0); installedTap = false }
        if activatedSession
        {
            try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
            activatedSession = false
        }
    }

    private func fail(_ message: String)
    {
        stop()
        error = message
    }
}
