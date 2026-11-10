import AVFoundation
import Foundation

enum MicCaptureError: LocalizedError {
    case noInputFormat

    var errorDescription: String? {
        switch self {
        case .noInputFormat: return "没有可用的麦克风输入格式"
        }
    }
}

final class MicCapture {
    let engine = AVAudioEngine()
    private var session: OpaquePointer?
    private var engines: UnsafeMutableRawPointer?
    /// The audio tap runs on its own thread. This lock keeps stop() from handing the session to teardown while a
    /// push is still inside it.
    private let lock = NSLock()
    /// Summaries and the final teardown share this serial queue, so stop() never frees the session under a
    /// summary that is still generating.
    private let summaryQueue = DispatchQueue(label: "apexo.summary", qos: .userInitiated)
    var onText: (String) -> Void = { _ in }

    func openTranscript() -> String {
        ensureSession()
        guard let session else { return "" }
        let text = String(cString: apexo_session_display_text(session))
        return text.isEmpty ? "等待麦克风" : text
    }

    func start() throws {
        if engine.isRunning { return }
        let audio = AVAudioSession.sharedInstance()
        try audio.setCategory(.playAndRecord, mode: .measurement, options: [.defaultToSpeaker])
        try audio.setActive(true)
        ensureSession()
        let input = engine.inputNode
        let format = input.outputFormat(forBus: 0)
        guard format.sampleRate > 0,
              let target = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 16000, channels: 1, interleaved: false),
              let converter = AVAudioConverter(from: format, to: target) else {
            throw MicCaptureError.noInputFormat
        }
        input.removeTap(onBus: 0)
        input.installTap(onBus: 0, bufferSize: 4096, format: format) { [weak self] buffer, _ in
            self?.push(buffer, converter: converter, target: target)
        }
        engine.prepare()
        try engine.start()
    }

    func push(_ buffer: AVAudioPCMBuffer, converter: AVAudioConverter, target: AVAudioFormat) {
        lock.lock()
        defer { lock.unlock() }
        guard let session else { return }
        let ratio = 16000.0 / buffer.format.sampleRate
        let capacity = AVAudioFrameCount(Double(buffer.frameLength) * ratio) + 8
        guard let converted = AVAudioPCMBuffer(pcmFormat: target, frameCapacity: capacity) else { return }
        var consumed = false
        var error: NSError?
        converter.convert(to: converted, error: &error) { _, status in
            if consumed {
                status.pointee = .noDataNow
                return nil
            }
            consumed = true
            status.pointee = .haveData
            return buffer
        }
        guard let samples = converted.floatChannelData?[0] else { return }
        let rc = apexo_session_push_pcm(session, samples, Int32(converted.frameLength), Int32(APEXO_SAMPLE_RATE))
        guard rc == 0 || rc == -3 else { return }
        // -3: the speech model files are not in the app bundle. Say so instead of showing nothing.
        let text = rc == -3
            ? "没有找到语音模型：请把 encoder.onnx、decoder.onnx、joiner.onnx、tokens.txt 放进 asr-model 目录"
            : String(cString: apexo_session_display_text(session))
        DispatchQueue.main.async { self.onText(text) }
    }

    func endSegment() {
        guard let session else { return }
        _ = apexo_session_end_segment(session)
        onText(String(cString: apexo_session_display_text(session)))
    }

    /// Runs off the main thread: loading Gemma and generating takes seconds. `done` is called on the main thread.
    func summarize(done: @escaping (String) -> Void) {
        guard let session else { return done("") }
        if engine.isRunning {
            engine.inputNode.removeTap(onBus: 0)
            engine.stop()
        }
        _ = apexo_session_end_segment(session)
        summaryQueue.async {
            var buffer = [CChar](repeating: 0, count: 262144)
            let rc = apexo_session_summarize(session, &buffer, Int32(buffer.count))
            let text = rc == 0 ? String(cString: buffer) : String(cString: apexo_session_last_error(session))
            DispatchQueue.main.async { done(text) }
        }
    }

    /// Summarizes what was said since the last summary (including the line still being recognized) while
    /// microphone capture keeps running.
    func summarizeNew(done: @escaping (String) -> Void) {
        guard let session else { return done("") }
        summaryQueue.async {
            var buffer = [CChar](repeating: 0, count: 262144)
            let rc = apexo_session_summarize_new(session, &buffer, Int32(buffer.count))
            let text = rc == 0 ? String(cString: buffer) : String(cString: apexo_session_last_error(session))
            DispatchQueue.main.async { done(text) }
        }
    }

    private func ensureSession() {
        lock.lock()
        defer { lock.unlock() }
        if session == nil {
            let asrDir = Bundle.main.path(forResource: "asr-model", ofType: nil) ?? ""
            let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
            let gemma = documents?.appendingPathComponent("gemma-4-E2B-it.litertlm").path ?? "gemma-4-E2B-it.litertlm"
            var created: UnsafeMutableRawPointer?
            session = DemoCreateSession(asrDir, gemma, &created)
            engines = created
        }
    }

    func stop() {
        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        lock.lock()
        let closing = session
        let closingEngines = engines
        session = nil
        engines = nil
        lock.unlock()
        guard let closing else { return }
        // Queued behind any running summary: the engines must outlive it.
        summaryQueue.async {
            apexo_session_release(closing)
            apexo_session_destroy(closing)
            DemoDestroyEngines(closingEngines)
        }
    }
}
