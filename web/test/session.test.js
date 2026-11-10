import { createSession, resampleTo16k } from "../src/index.js";
import { SUMMARY_CHUNK_CHARS, SUMMARY_INSTRUCTION } from "../src/session.js";

let failures = 0;

function expect(ok, name) {
  if (ok) console.log(`PASS ${name}`);
  else {
    console.log(`FAIL ${name}`);
    failures += 1;
  }
}

function scripted(events) {
  const state = { index: 0, resetCount: 0, loadCount: 0, prompts: [], residentDuring: -1, session: null };
  const engines = {
    asrPush() {
      if (state.index >= events.length) return { isFinal: -1 };
      const event = events[state.index];
      state.index += 1;
      return event;
    },
    asrReset() {
      state.resetCount += 1;
    },
    sumLoad() {
      state.loadCount += 1;
      return 0;
    },
    sumUnload() {},
    sumGenerate(prompt) {
      state.residentDuring = state.session.gemmaResident ? 1 : 0;
      state.prompts.push(prompt);
      return "ok";
    },
  };
  const session = createSession({
    summaryModelPath: "gemma-4-E2B-it-web.litertlm",
    webSummary: true,
    engines,
  });
  state.session = session;
  return { state, session };
}

const merged = scripted([
  { text: "你好", isFinal: 1, startMs: 0, endMs: 1000 },
  { text: "世界", isFinal: 1, startMs: 2500, endMs: 3000 },
  { text: "下一句", isFinal: 1, startMs: 5000, endMs: 5800 },
]);
merged.session.pushPcm(Float32Array.from([0.1]), 16000);
merged.session.pushPcm(Float32Array.from([0.1]), 16000);
const mergedText = merged.session.displayText;
merged.session.pushPcm(Float32Array.from([0.1]), 16000);
expect(mergedText === "你好世界", "merge: finals separated by less than 1.5-2 seconds merge into one display segment");
expect(merged.session.displayText === "你好世界\n下一句", "merge: a gap of 2000 ms or more stays a separate display segment");

const repeated = scripted([
  { text: "重复", isFinal: 1, startMs: 0, endMs: 400 },
  { text: "重复", isFinal: 1, startMs: 800, endMs: 1200 },
  { text: Array.from({ length: 250 }, (_, i) => `w${i}`).join(" "), isFinal: 1, startMs: 4000, endMs: 9000 },
]);
repeated.session.pushPcm(Float32Array.from([0.1]), 16000);
repeated.session.pushPcm(Float32Array.from([0.1]), 16000);
expect(repeated.session.displayText === "重复", "apexo-dedup: overlapping chunk text is not repeated");
repeated.session.pushPcm(Float32Array.from([0.1]), 16000);
expect(!repeated.session.displayText.includes("w0 ") && repeated.session.displayText.includes("w249") && repeated.session.fullTranscript.startsWith("w0") === false && repeated.session.fullTranscript.includes("w0 "), "apexo-transcript-buffer: maxUiLines=4 maxWordCount=200 fillGapMs=2000");

const lines = [];
for (let i = 0; i < 6; i += 1) {
  lines.push({ text: `line-${i}`, isFinal: 1, startMs: i * 3000, endMs: i * 3000 + 500 });
}
const full = scripted(lines);
for (let i = 0; i < 6; i += 1) full.session.pushPcm(Float32Array.from([0.1]), 16000);
expect(full.session.displayText.split("\n").length === 4 && !full.session.displayText.includes("line-0"), "full-transcript: display keeps only recent lines");
expect(full.session.fullTranscript.includes("line-0") && full.session.fullTranscript.includes("line-5"), "full-transcript: the full transcript is kept even when the display keeps only recent lines");

const summary = scripted([{ text: "a".repeat(3200), isFinal: 1, startMs: 0, endMs: 500 }]);
summary.session.pushPcm(Float32Array.from([0.2]), 16000);
summary.session.pushPcm(Float32Array.from([0.2]), 16000);
expect(summary.state.loadCount === 0 && summary.session.gemmaResident === false, "Gemma-not-resident-during-transcription");
const text = summary.session.summarize();
expect(text.startsWith("ok") && summary.state.loadCount === 1 && summary.session.gemmaResident === false, "Gemma-not-resident-during-transcription: Gemma loads only when a summary is requested and unloads after");
expect(summary.state.residentDuring === 1, "Gemma-not-resident-during-transcription: resident only inside summarize");
const marker = "转写：\n";
let chunksOk = summary.state.prompts.length >= 2;
let textOnly = summary.state.prompts.length > 0;
for (const prompt of summary.state.prompts) {
  const pos = prompt.indexOf(marker);
  if (pos < 0 || !prompt.includes("只根据下面的转写写摘要") || !prompt.includes("转写里没有的内容不要补")) {
    chunksOk = false;
    textOnly = false;
  }
  const chunk = prompt.slice(pos + marker.length);
  if (!chunk || chunk.length > SUMMARY_CHUNK_CHARS) chunksOk = false;
  if (prompt.includes("pcm")) textOnly = false;
  if (!prompt.startsWith(SUMMARY_INSTRUCTION)) textOnly = false;
}
expect(chunksOk, "chunk-size: a long transcript is summarized in chunks of about 1500 characters");
expect(textOnly, "text-only-summary: the summarizer is given text only and told not to invent missing content");

const rejected = createSession({
  summaryModelPath: "gemma-4-E2B-it.litertlm",
  webSummary: true,
  engines: { asrPush() { return { isFinal: -1 }; }, sumLoad() { return 0; }, sumGenerate() { return "no"; } },
});
expect(rejected.summarize() === -4, "web summary does not accept the non-web .litertlm");

let seen = 0;
const resampled = createSession({
  webSummary: true,
  summaryModelPath: "gemma-4-E2B-it-web.litertlm",
  engines: {
    asrPush(pcm, sampleRate) {
      seen = pcm.length;
      if (sampleRate !== 16000) failures += 1;
      return { isFinal: -1 };
    },
  },
});
resampled.pushPcm(new Float32Array(48000), 48000);
expect(seen === 16000, "web sdk resamples browser audio to 16 kHz mono float");

const spoken = createSession({
  webSummary: true,
  summaryModelPath: "gemma-4-E2B-it-web.litertlm",
  engines: { asrPush() { return { isFinal: -1 }; }, sumLoad() { return 0; }, sumGenerate() { return "s"; } },
});
spoken.pushAsrEvent({ isFinal: 0, text: "正在说", startMs: 0, endMs: 400 });
expect(spoken.displayText === "正在说", "interim text shows as soon as speech arrives");
spoken.pushAsrEvent({ isFinal: 1, text: "正在说完", startMs: 0, endMs: 800 });
expect(spoken.displayText === "正在说完", "final text replaces the interim line");
spoken.pushAsrEvent({ isFinal: 0, text: "下一句", startMs: 3000, endMs: 3400 });
spoken.endSegment();
expect(spoken.fullTranscript === "正在说完\n下一句", "end keeps the unfinished line for summary");

if (failures !== 0) {
  console.log(`${failures} failure(s)`);
  process.exit(1);
}
console.log("all session assertions passed");
