import SwiftUI

struct ContentView: View {
    @State private var transcript = "等待麦克风"
    @State private var running = false
    private let mic = MicCapture()

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text(transcript)
                .frame(maxWidth: .infinity, minHeight: 160, alignment: .topLeading)
                .padding()
                .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.gray))
                .accessibilityIdentifier("transcript")
            HStack {
                Button(running ? "录音中" : "开始麦克风") {
                    mic.onText = { transcript = $0 }
                    try? mic.start()
                    running = true
                }
                Button("结束一段") { mic.endSegment() }
                Button("总结") { transcript = mic.summarize() }
                    .accessibilityIdentifier("summarize")
            }
        }
        .padding()
        .onAppear {
            mic.onText = { transcript = $0 }
            transcript = mic.openTranscript()
        }
    }
}
