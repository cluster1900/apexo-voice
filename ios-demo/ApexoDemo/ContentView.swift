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
                    do {
                        try mic.start()
                        running = true
                    } catch {
                        running = false
                        transcript = "麦克风没有打开：\(error.localizedDescription)"
                    }
                }
                Button("结束一段") {
                    running = false
                    mic.endSegment()
                }
                Button("阶段摘要") {
                    mic.summarizeNew { summary in
                        if !summary.isEmpty {
                            transcript = summary
                        }
                    }
                }
                    .accessibilityIdentifier("summarizeNew")
                Button("总结") {
                    running = false
                    transcript = "正在总结"
                    mic.summarize { transcript = $0 }
                }
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
