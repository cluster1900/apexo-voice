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
  { text: "重复", isFinal: 1, startMs: 200, endMs: 600 },
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

const engines = { asrPush() { return { isFinal: -1 }; }, sumLoad() { return 0; }, sumGenerate() { return "ok"; } };
const { createSession: createCoreSession } = await import("../src/session.js");
const noName = createCoreSession({ summaryModelPath: "", webSummary: true, engines });
expect(noName.summarize() === -4, "web summary needs a model name");
const notBundle = createSession({ summaryModelPath: "model.bin", webSummary: true, engines });
expect(notBundle.summarize() === -4, "the summary model must be a .litertlm file");
const gpuName = createSession({ summaryModelPath: "gemma-4-E2B-it-gpu.litertlm", webSummary: true, engines });
gpuName.pushAsrEvent({ isFinal: 1, text: "有内容", startMs: 0, endMs: 100 });
expect(gpuName.summarize() === "ok", "the build of the bundle is not guessed from its file name");

let seen = 0;
const resampled = createSession({
  webSummary: true,
  summaryModelPath: "gemma-4-E2B-it-web.litertlm",
  engines: {
    asrPush(pcm, sampleRate) {
      seen += pcm.length;
      if (sampleRate !== 16000) failures += 1;
      return { isFinal: -1 };
    },
  },
});
resampled.pushPcm(new Float32Array(48000), 48000);
// The low-pass filter reaches a few samples ahead, so the last ~0.5 ms of a push waits for the next one.
expect(seen <= 16000 && seen >= 15984, "web sdk resamples browser audio to 16 kHz mono float");
resampled.endSegment();
expect(seen === 16000, "endSegment flushes the remaining resampled tail");

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

// Silence endpoints deliver finals with no words. They must not leave blank lines or empty display lines.
{
  const quiet = createSession({
    summaryModelPath: "gemma-4-E2B-it-web.litertlm",
    engines: { asrPush() { return { isFinal: -1 }; } },
  });
  quiet.pushAsrEvent({ isFinal: 1, text: "你好", startMs: 0, endMs: 500 });
  quiet.pushAsrEvent({ isFinal: 1, text: "", startMs: 2400, endMs: 2500 });
  quiet.pushAsrEvent({ isFinal: 1, text: "", startMs: 4800, endMs: 4900 });
  quiet.pushAsrEvent({ isFinal: 1, text: "世界", startMs: 9000, endMs: 9500 });
  expect(quiet.fullTranscript === "你好\n世界", "empty-final: finals without words are not added to the transcript");
  expect(quiet.displayText === "你好\n世界", "empty-final: finals without words do not take a display line");
}

// Latin words that merge onto one line keep a space between them.
{
  const latin = createSession({ summaryModelPath: "gemma-4-E2B-it-web.litertlm", engines: {} });
  latin.pushAsrEvent({ isFinal: 1, text: "HELLO WORLD", startMs: 0, endMs: 1000 });
  latin.pushAsrEvent({ isFinal: 1, text: "HOW ARE YOU", startMs: 1500, endMs: 2500 });
  expect(latin.displayText === "HELLO WORLD HOW ARE YOU", "merge: latin finals are joined with a space");
}

// The interim line counts toward the line limit.
{
  const many = createSession({ summaryModelPath: "gemma-4-E2B-it-web.litertlm", engines: {} });
  for (let i = 0; i < 6; i += 1) many.pushAsrEvent({ isFinal: 1, text: `L${i}`, startMs: i * 5000, endMs: i * 5000 + 100 });
  many.pushAsrEvent({ isFinal: 0, text: "interim", startMs: 40000, endMs: 40100 });
  expect(many.displayText.split("\n").length === 4, "display: interim plus finals stay within the line limit");
}

// Nothing was said: Gemma is not loaded. A second summarize while one runs is refused.
{
  let loads = 0;
  const idle = createSession({
    summaryModelPath: "gemma-4-E2B-it-web.litertlm",
    engines: { sumLoad() { loads += 1; return Promise.resolve(0); }, sumGenerate() { return "s"; } },
  });
  expect(idle.summarize() === "" && loads === 0, "empty-transcript: summarize does not load Gemma");
  idle.pushAsrEvent({ isFinal: 1, text: "有内容", startMs: 0, endMs: 500 });
  const first = idle.summarize();
  expect(idle.gemmaResident === true, "Gemma is resident from the start of the load");
  expect(idle.summarize() === -5, "a second summarize while one runs is refused");
  expect(idle.pushPcm(new Float32Array(160), 16000) === -5, "pcm is refused while Gemma loads");
  const summary = await first;
  expect(summary === "s" && loads === 1 && idle.gemmaResident === false, "the first summarize finishes and unloads");
}

// A missing summary model is -3 and nothing is invented.
{
  let unloads = 0;
  const gone = createSession({
    summaryModelPath: "gemma-4-E2B-it-gpu.litertlm",
    engines: { sumLoad() { return -3; }, sumUnload() { unloads += 1; }, sumGenerate() { return "never"; } },
  });
  gone.pushAsrEvent({ isFinal: 1, text: "有内容", startMs: 0, endMs: 100 });
  expect(gone.summarize() === -3 && gone.lastError.includes("not found") && gone.gemmaResident === false && unloads === 1,
         "a missing summary model returns -3 and leaves Gemma unloaded");
}

// Chunks end on a sentence when there is one in the second half of the chunk.
{
  const { chunkText } = await import("../src/session.js");
  const text = "Sentence number one is right here. ".repeat(130);
  const parts = chunkText(text, 1500);
  expect(parts.length >= 3 && parts.every((part) => [...part].length <= 1500), "chunk-size: chunks hold at most 1500 characters");
  expect(parts.slice(0, -1).every((part) => part.trimEnd().endsWith(".")), "chunk-boundary: chunks are cut after a sentence end");
  expect(parts.join("") === text, "chunking loses no text");
}

// Calling summarize without endSegment still keeps the unfinished interim line.
{
  const prompts = [];
  const direct = createSession({
    summaryModelPath: "gemma-4-E2B-it-gpu.litertlm",
    engines: {
      sumLoad() { return 0; },
      sumGenerate(prompt) { prompts.push(prompt); return "ok"; },
    },
  });
  direct.pushAsrEvent({ isFinal: 0, text: "直接总结这句", startMs: 0, endMs: 600 });
  expect(direct.summarize() === "ok" && direct.fullTranscript === "直接总结这句" && prompts.length === 1 && prompts[0].includes("直接总结这句"),
         "summarize promotes an unfinished interim line before summarizing");
}

// Rolling incremental summary: includes the line still being recognized, sends only new text on later calls, and does
// not block pushPcm while running.
{
  const prompts = [];
  let loads = 0;
  let resolveGen;
  const rolling = createSession({
    summaryModelPath: "gemma-4-E2B-it-gpu.litertlm",
    engines: {
      asrPush() { return { isFinal: 1, text: "边总结边录的新句子", startMs: 6000, endMs: 6500 }; },
      sumLoad() { loads += 1; return 0; },
      sumGenerate(prompt) {
        prompts.push(prompt);
        return new Promise((resolve) => { resolveGen = resolve; });
      },
    },
  });
  rolling.pushAsrEvent({ isFinal: 1, text: "第一段定稿", startMs: 0, endMs: 500 });
  rolling.pushAsrEvent({ isFinal: 0, text: "第二段还在说", startMs: 3000, endMs: 3400 });
  const pending1 = rolling.summarizeNew();
  // While summarizeNew is running, live pushPcm is NOT blocked!
  expect(rolling.pushPcm(new Float32Array(160), 16000) === 0, "summarizeNew does not block live pushPcm while running");
  resolveGen("摘要1");
  const sum1 = await pending1;
  expect(sum1 === "摘要1" && rolling.summaryText === "摘要1", "summarizeNew returns and stores the first summary");
  expect(prompts.length === 1 && prompts[0].includes("第一段定稿") && prompts[0].includes("第二段还在说") &&
         rolling.fullTranscript.startsWith("第一段定稿\n第二段还在说"),
         "summarizeNew includes the unfinished line and keeps it in the transcript");

  const pending2 = rolling.summarizeNew();
  resolveGen("摘要2");
  const sum2 = await pending2;
  expect(sum2 === "摘要1\n摘要2" && rolling.summaryText === "摘要1\n摘要2", "summarizeNew appends new summary to summaryText");
  expect(prompts.length === 2 && !prompts[1].includes("第一段定稿") && prompts[1].includes("边总结边录的新句子"),
         "summarizeNew sends only the newly finalized delta on the next call");
  expect(rolling.summarizeNew() === "摘要1\n摘要2" && loads === 2,
         "summarizeNew does not reload Gemma when there is no new finalized text");
}

// Separate identical utterances (after endSegment or >= mergeGapMs) are both retained.
{
  const s = createSession({ summaryModelPath: "gemma-4-E2B-it-gpu.litertlm" });
  s.pushAsrEvent({ isFinal: 1, text: "同样一句话", startMs: 0, endMs: 500 });
  s.endSegment();
  s.pushAsrEvent({ isFinal: 1, text: "同样一句话", startMs: 0, endMs: 500 });
  s.pushAsrEvent({ isFinal: 1, text: "同样一句话", startMs: 3000, endMs: 3500 });
  expect(s.fullTranscript === "同样一句话\n同样一句话\n同样一句话",
         "dedup: separate identical utterances after endSegment or >= mergeGapMs are retained");
}

// Two close utterances that read the same, with no audio overlap, are both kept even without endSegment.
{
  const s = createSession({ summaryModelPath: "gemma-4-E2B-it-gpu.litertlm" });
  s.pushAsrEvent({ isFinal: 1, text: "确认", startMs: 0, endMs: 500 });
  s.pushAsrEvent({ isFinal: 1, text: "确认", startMs: 1800, endMs: 2300 });
  expect(s.fullTranscript === "确认\n确认", "dedup: close but non-overlapping identical finals are both kept");
}

// PCM stays refused until an asynchronous unload has really finished.
{
  let finishUnload;
  const s = createSession({
    summaryModelPath: "gemma-4-E2B-it-gpu.litertlm",
    engines: {
      sumLoad() { return 0; },
      sumGenerate() { return "摘要"; },
      sumUnload() { return new Promise((resolve) => { finishUnload = resolve; }); },
      asrPush() { return { isFinal: -1 }; },
    },
  });
  s.pushAsrEvent({ isFinal: 1, text: "要总结的话", startMs: 0, endMs: 500 });
  const pending = s.summarize();
  expect(s.gemmaResident === true && s.pushPcm(Float32Array.from([0.1]), 16000) === -5,
         "async unload: still resident and refusing PCM while the unload is pending");
  finishUnload();
  const result = await pending;
  expect(result === "摘要" && s.gemmaResident === false && s.pushPcm(Float32Array.from([0.1]), 16000) === 0,
         "async unload: PCM is accepted again only after the unload finished");
}

// release() during a summary leaves the engine to that summary: no unload under a running generation, and PCM
// stays refused until the summary has unloaded.
{
  let resolveGen;
  let unloads = 0;
  const s = createSession({
    summaryModelPath: "gemma-4-E2B-it-gpu.litertlm",
    engines: {
      sumLoad() { return 0; },
      sumGenerate() { return new Promise((resolve) => { resolveGen = resolve; }); },
      sumUnload() { unloads += 1; },
      asrPush() { return { isFinal: -1 }; },
    },
  });
  s.pushAsrEvent({ isFinal: 1, text: "要总结的话", startMs: 0, endMs: 500 });
  const pending = s.summarize();
  s.release();
  expect(unloads === 0 && s.gemmaResident === true && s.pushPcm(Float32Array.from([0.1]), 16000) === -5,
         "release during summarize does not unload the engine under the running generation");
  resolveGen("旧摘要");
  await pending;
  expect(unloads === 1 && s.gemmaResident === false && s.pushPcm(Float32Array.from([0.1]), 16000) === 0,
         "the summary unloads once when it finishes after release");
}

// A heading the model writes itself is dropped; ordinary text is kept.
{
  const cases = [["摘要：\n\n要点一", "要点一"], ["**总结**: 要点二", "要点二"], ["## 摘要\n要点三", "要点三"],
    ["Summary: point", "point"], ["总结一下今天的会", "总结一下今天的会"], ["转写内容摘要：  要点四", "要点四"],
    ["以下是会议总结：\n要点五", "要点五"], ["总结，今天先到这里", "总结，今天先到这里"]];
  let ok = true;
  for (const [reply, want] of cases) {
    const s = createSession({
      summaryModelPath: "gemma-4-E2B-it-gpu.litertlm",
      engines: { sumLoad() { return 0; }, sumGenerate() { return reply; }, sumUnload() {} },
    });
    s.pushAsrEvent({ isFinal: 1, text: "会议内容", startMs: 0, endMs: 500 });
    const got = s.summarize();
    if (got !== want) { ok = false; console.log(`  label case [${reply}] -> [${got}]`); }
  }
  expect(ok, "summary: a leading label written by the model is removed");
}

// Calling release() while summarize or summarizeNew is running does not write back stale summaries.
{
  let resolveGen;
  const s = createSession({
    summaryModelPath: "gemma-4-E2B-it-gpu.litertlm",
    engines: {
      sumLoad() { return 0; },
      sumGenerate() { return new Promise((resolve) => { resolveGen = resolve; }); },
    },
  });
  s.pushAsrEvent({ isFinal: 1, text: "旧会话内容", startMs: 0, endMs: 500 });
  const p1 = s.summarize();
  s.release();
  resolveGen("旧摘要");
  expect((await p1) === "" && s.summaryText === "", "release during summarize does not write back stale summary");

  s.pushAsrEvent({ isFinal: 1, text: "另一段旧内容", startMs: 0, endMs: 500 });
  const p2 = s.summarizeNew();
  s.release();
  s.pushAsrEvent({ isFinal: 1, text: "新会话更长的一段内容", startMs: 0, endMs: 800 });
  resolveGen("旧增量摘要");
  expect((await p2) === "" && s.summaryText === "", "release during summarizeNew does not write back stale summary");
}

if (failures !== 0) {
  console.log(`${failures} failure(s)`);
  process.exit(1);
}
console.log("all session assertions passed");
