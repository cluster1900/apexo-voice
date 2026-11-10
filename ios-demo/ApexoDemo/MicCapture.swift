import AVFoundation
import Foundation

final class MicCapture {
    let engine = AVAudioEngine()
    private var session: OpaquePointer?
    var onText: (String) -> Void = { _ in }

    func openTranscript() -> String {
        ensureSession()
        pushSilence()
        pushSilence()
        guard let session else { return "" }
        return String(cString: apexo_session_display_text(session))
    }

    func start() throws {
        let audio = AVAudioSession.sharedInstance()
        try audio.setCategory(.playAndRecord, mode: .measurement, options: [.defaultToSpeaker])
        try audio.setActive(true)
        ensureSession()
        let input = engine.inputNode
        let format = input.outputFormat(forBus: 0)
        guard let target = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 16000, channels: 1, interleaved: false),
              let converter = AVAudioConverter(from: format, to: target) else {
            return
        }
        input.installTap(onBus: 0, bufferSize: 4096, format: format) { [weak self] buffer, _ in
            self?.push(buffer, converter: converter, target: target)
        }
        engine.prepare()
        try engine.start()
    }

    func push(_ buffer: AVAudioPCMBuffer, converter: AVAudioConverter, target: AVAudioFormat) {
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
        _ = apexo_session_push_pcm(session, samples, Int32(converted.frameLength), Int32(APEXO_SAMPLE_RATE))
        let text = String(cString: apexo_session_display_text(session))
        DispatchQueue.main.async { self.onText(text) }
    }

    func endSegment() {
        guard let session else { return }
        _ = apexo_session_end_segment(session)
        onText(String(cString: apexo_session_display_text(session)))
    }

    func summarize() -> String {
        guard let session else { return "" }
        let residentBefore = apexo_session_gemma_resident(session)
        var buffer = [CChar](repeating: 0, count: 8192)
        let rc = apexo_session_summarize(session, &buffer, Int32(buffer.count))
        let residentAfter = apexo_session_gemma_resident(session)
        if rc != 0 { return String(cString: apexo_session_last_error(session)) }
        return "before=\(residentBefore) after=\(residentAfter)\n" + String(cString: buffer)
    }

    private func ensureSession() {
        if session == nil {
            session = DemoCreateSession("gemma-4-E2B-it.litertlm")
        }
    }

    private func pushSilence() {
        guard let session else { return }
        let frame = [Float](repeating: 0, count: 1600)
        frame.withUnsafeBufferPointer { pointer in
            guard let base = pointer.baseAddress else { return }
            _ = apexo_session_push_pcm(session, base, Int32(frame.count), Int32(APEXO_SAMPLE_RATE))
        }
    }

    func stop() {
        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        if let session {
            apexo_session_release(session)
            apexo_session_destroy(session)
            DemoDestroyEngines()
        }
        session = nil
    }
}
