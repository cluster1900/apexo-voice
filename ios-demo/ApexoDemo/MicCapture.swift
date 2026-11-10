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
    /// Protects `session` and `engines` pointers across queues.
    private let lock = NSLock()
    /// Serial queue that performs resampling, `apexo_session_push_pcm`, and `apexo_session_end_segment`.
    /// The real-time audio tap only copies the captured `AVAudioPCMBuffer` and dispatches to this queue.
    private let audioQueue = DispatchQueue(label: "apexo.audio", qos: .userInitiated)
    /// Summaries and the final teardown share this serial queue, so stop() never frees the session under a
    /// summary that is still generating.
    private let summaryQueue = DispatchQueue(label: "apexo.summary", qos: .userInitiated)
    var onText: (String) -> Void = { _ in }

    func openTranscript() -> String {
        ensureSession()
        lock.lock()
        let current = session
        lock.unlock()
        guard let current else { return "" }
        let text = String(cString: apexo_session_display_text(current))
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
            // Real-time audio callback: only copy the PCM buffer and hand it to the serial audioQueue.
            self?.push(buffer, converter: converter, target: target)
        }
        engine.prepare()
        try engine.start()
    }

    private static func copyPCMBuffer(_ buffer: AVAudioPCMBuffer) -> AVAudioPCMBuffer? {
        guard let copy = AVAudioPCMBuffer(pcmFormat: buffer.format, frameCapacity: buffer.frameLength) else {
            return nil
        }
        copy.frameLength = buffer.frameLength
        let frames = Int(buffer.frameLength)
        let channels = Int(buffer.format.channelCount)
        if let src = buffer.floatChannelData, let dst = copy.floatChannelData {
            for ch in 0..<channels {
                dst[ch].update(from: src[ch], count: frames)
            }
            return copy
        }
        if let src = buffer.int16ChannelData, let dst = copy.int16ChannelData {
            for ch in 0..<channels {
                dst[ch].update(from: src[ch], count: frames)
            }
            return copy
        }
        return nil
    }

    func push(_ buffer: AVAudioPCMBuffer, converter: AVAudioConverter, target: AVAudioFormat) {
        guard let copied = Self.copyPCMBuffer(buffer) else { return }
        audioQueue.async { [weak self] in
            self?.processCopiedBuffer(copied, converter: converter, target: target)
        }
    }

    private func processCopiedBuffer(_ buffer: AVAudioPCMBuffer, converter: AVAudioConverter, target: AVAudioFormat) {
        lock.lock()
        let current = session
        lock.unlock()
        guard let current else { return }
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
        let rc = apexo_session_push_pcm(current, samples, Int32(converted.frameLength), Int32(APEXO_SAMPLE_RATE))
        guard rc == 0 || rc == -3 else { return }
        // -3: the speech model files are not in the app bundle. Say so instead of showing nothing.
        let text = rc == -3
            ? "没有找到语音模型：请把 encoder.onnx、decoder.onnx、joiner.onnx、tokens.txt 放进 asr-model 目录"
            : String(cString: apexo_session_display_text(current))
        DispatchQueue.main.async { self.onText(text) }
    }

    func endSegment() {
        if engine.isRunning {
            engine.inputNode.removeTap(onBus: 0)
            engine.stop()
        }
        // Drain all already-queued audio buffers on audioQueue before calling apexo_session_end_segment.
        audioQueue.sync {
            lock.lock()
            let current = session
            lock.unlock()
            guard let current else { return }
            _ = apexo_session_end_segment(current)
            let display = String(cString: apexo_session_display_text(current))
            if Thread.isMainThread {
                onText(display)
            } else {
                DispatchQueue.main.async { self.onText(display) }
            }
        }
    }

    /// Runs off the main thread: drains queued audio, finishes the segment, and generates the full summary.
    func summarize(done: @escaping (String) -> Void) {
        if engine.isRunning {
            engine.inputNode.removeTap(onBus: 0)
            engine.stop()
        }
        audioQueue.async { [weak self] in
            guard let self else { return }
            self.lock.lock()
            let current = self.session
            self.lock.unlock()
            guard let current else {
                DispatchQueue.main.async { done("") }
                return
            }
            _ = apexo_session_end_segment(current)
            self.summaryQueue.async {
                var buffer = [CChar](repeating: 0, count: 262144)
                let rc = apexo_session_summarize(current, &buffer, Int32(buffer.count))
                let text = rc == 0 ? String(cString: buffer) : String(cString: apexo_session_last_error(current))
                DispatchQueue.main.async { done(text) }
            }
        }
    }

    /// Summarizes newly finalized lines since the last summary while microphone capture continues on audioQueue.
    func summarizeNew(done: @escaping (String) -> Void) {
        lock.lock()
        let current = session
        lock.unlock()
        guard let current else { return done("") }
        summaryQueue.async {
            var buffer = [CChar](repeating: 0, count: 262144)
            let rc = apexo_session_summarize_new(current, &buffer, Int32(buffer.count))
            let text = rc == 0 ? String(cString: buffer) : String(cString: apexo_session_last_error(current))
            DispatchQueue.main.async { done(text) }
        }
    }

    /// Summarizes external text without requiring an ASR model or modifying the session transcript.
    func summarizeText(_ input: String, done: @escaping (String) -> Void) {
        ensureSession()
        lock.lock()
        let current = session
        lock.unlock()
        guard let current else { return done("") }
        summaryQueue.async {
            var buffer = [CChar](repeating: 0, count: 262144)
            let rc = input.withCString { cstr in
                apexo_session_summarize_text(current, cstr, &buffer, Int32(buffer.count))
            }
            let text = rc == 0 ? String(cString: buffer) : String(cString: apexo_session_last_error(current))
            DispatchQueue.main.async { done(text) }
        }
    }

    func reset() {
        audioQueue.sync {
            lock.lock()
            let current = session
            lock.unlock()
            guard let current else { return }
            apexo_session_reset(current)
        }
    }

    private func ensureSession() {
        lock.lock()
        defer { lock.unlock() }
        if session == nil {
            let asrDir = Bundle.main.path(forResource: "asr-model", ofType: nil) ?? ""
            let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
            let gemma = documents?.appendingPathComponent("gemma-4-E2B-it.litertlm").path ?? "gemma-4-E2B-it.litertlm"
            let fwDir = Bundle.main.privateFrameworksPath ?? (Bundle.main.bundlePath + "/Frameworks")
            let sherpaCandidate = (fwDir as NSString).appendingPathComponent("SherpaOnnxC.framework/SherpaOnnxC")
            let litertCandidate = (fwDir as NSString).appendingPathComponent("CLiteRTLM.framework/CLiteRTLM")
            let sherpaPath = FileManager.default.fileExists(atPath: sherpaCandidate) ? sherpaCandidate : ""
            let litertPath = FileManager.default.fileExists(atPath: litertCandidate) ? litertCandidate : ""
            var created: UnsafeMutableRawPointer?
            session = DemoCreateSessionWithLibraries(asrDir, gemma, sherpaPath, litertPath, &created)
            engines = created
        }
    }

    func stop() {
        if engine.isRunning {
            engine.inputNode.removeTap(onBus: 0)
            engine.stop()
        }
        // Drain any pending audio callbacks before tearing down the session.
        audioQueue.sync {}
        lock.lock()
        let closing = session
        let closingEngines = engines
        session = nil
        engines = nil
        lock.unlock()
        guard let closing else { return }
        // Queued behind any running summary: the engines must outlive it.
        summaryQueue.async {
            apexo_session_reset(closing)
            apexo_session_destroy(closing)
            DemoDestroyEngines(closingEngines)
        }
    }
}
