import { createSession, resampleTo16k, ERR_BUSY } from "../src/index.js";
import {
  SUMMARY_CHUNK_CHARS,
  SUMMARY_CHUNK_INSTRUCTION,
  SUMMARY_SYNTHESIS_INSTRUCTION,
} from "../src/session.js";

let failures = 0;

function expect(ok, name) {
  if (ok) console.log(`PASS ${name}`);
  else {
    console.log(`FAIL ${name}`);
    failures += 1;
  }
}

async function expectRejectCode(promise, code, name) {
  try {
    await promise;
    expect(false, `${name} (resolved instead of rejecting with ${code})`);
  } catch (err) {
    expect(err instanceof Error && err.code === code, name);
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
expect(
  !repeated.session.displayText.includes("w0 ") &&
    repeated.session.displayText.includes("w249") &&
    repeated.session.fullTranscript.startsWith("w0") === false &&
    repeated.session.fullTranscript.includes("w0 "),
  "apexo-transcript-buffer: maxUiLines=4 maxWordCount=200 fillGapMs=2000",
);

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
const text = await summary.session.summarize();
expect(text.startsWith("ok") && summary.state.loadCount === 1 && summary.session.gemmaResident === false, "Gemma-not-resident-during-transcription: Gemma loads only when a summary is requested and unloads after");
expect(summary.state.residentDuring === 1, "Gemma-not-resident-during-transcription: resident only inside summarize");
const marker = "转写：\n";
let chunksOk = summary.state.prompts.length >= 3;
let textOnly = summary.state.prompts.length > 0;
for (let i = 0; i + 1 < summary.state.prompts.length; i += 1) {
  const prompt = summary.state.prompts[i];
  const pos = prompt.indexOf(marker);
  if (pos < 0 || !prompt.includes("仅依据以下转写提取简洁事实摘要") || !prompt.includes("没有明确给出的信息不要补充")) {
    chunksOk = false;
    textOnly = false;
  }
  const chunk = prompt.slice(pos + marker.length);
  if (!chunk || chunk.length > SUMMARY_CHUNK_CHARS) chunksOk = false;
  if (prompt.includes("pcm")) textOnly = false;
  if (!prompt.startsWith(SUMMARY_CHUNK_INSTRUCTION)) textOnly = false;
}
if (!summary.state.prompts.at(-1)?.startsWith(SUMMARY_SYNTHESIS_INSTRUCTION)) {
  chunksOk = false;
  textOnly = false;
}
expect(chunksOk, "chunk-size: a long transcript is summarized in chunks of about 1500 characters and synthesized");
expect(textOnly, "text-only-summary: the summarizer is given text only and told not to invent missing content");

const engines = { asrPush() { return { isFinal: -1 }; }, sumLoad() { return 0; }, sumGenerate() { return "ok"; } };
const { createSession: createCoreSession } = await import("../src/session.js");
const noName = createCoreSession({ summaryModelPath: "", webSummary: true, engines });
await expectRejectCode(noName.summarize("需要总结"), -4, "web summary needs a model name");
const notBundle = createSession({ summaryModelPath: "model.bin", webSummary: true, engines });
await expectRejectCode(notBundle.summarize("需要总结"), -4, "the summary model must be a .litertlm file");
const gpuName = createSession({ summaryModelPath: "gemma-4-E2B-it-gpu.litertlm", webSummary: true, engines });
gpuName.pushAsrEvent({ isFinal: 1, text: "有内容", startMs: 0, endMs: 100 });
expect((await gpuName.summarize()) === "ok", "the build of the bundle is not guessed from its file name");

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
await resampled.endSegment();
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
await spoken.endSegment();
expect(spoken.fullTranscript === "正在说完\n下一句", "endSegment fallback keeps the unfinished line when asrFinish is absent");

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

// Nothing was said: Gemma is not loaded. A second summarize while one runs is refused with -6 (ERR_BUSY), while PCM continues to enter ASR.
{
  let loads = 0;
  let resolveLoad;
  const idle = createSession({
    summaryModelPath: "gemma-4-E2B-it-web.litertlm",
    engines: {
      asrPush() { return { isFinal: -1 }; },
      sumLoad() {
        loads += 1;
        return new Promise((resolve) => { resolveLoad = resolve; });
      },
      sumGenerate() { return "s"; },
    },
  });
  expect((await idle.summarize()) === "" && loads === 0, "empty-transcript: summarize does not load Gemma");
  idle.pushAsrEvent({ isFinal: 1, text: "有内容", startMs: 0, endMs: 500 });
  const first = idle.summarize();
  expect(idle.gemmaResident === true, "Gemma is resident from the start of the load");
  await expectRejectCode(idle.summarize(), ERR_BUSY, "a second summarize while one runs is refused with -6");
  await expectRejectCode(idle.summarizeNew(), ERR_BUSY, "summarizeNew while summarize runs is refused with -6");
  await expectRejectCode(idle.summarize("外部文本"), ERR_BUSY, "summarize(text) while summarize runs is refused with -6");
  expect(idle.pushPcm(new Float32Array(160), 16000) === 0, "pcm is accepted while Gemma loads");
  resolveLoad(0);
  const summaryOut = await first;
  expect(summaryOut === "s" && loads === 1 && idle.gemmaResident === false, "the first summarize finishes and unloads");
}

// A missing summary model rejects with -3 and nothing is invented.
{
  let unloads = 0;
  const gone = createSession({
    summaryModelPath: "gemma-4-E2B-it-gpu.litertlm",
    engines: { sumLoad() { return -3; }, sumUnload() { unloads += 1; }, sumGenerate() { return "never"; } },
  });
  gone.pushAsrEvent({ isFinal: 1, text: "有内容", startMs: 0, endMs: 100 });
  await expectRejectCode(gone.summarize(), -3, "a missing summary model rejects with code -3");
  expect(gone.lastError.includes("not found") && gone.gemmaResident === false && unloads === 1,
         "missing summary model sets lastError and leaves Gemma unloaded");
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

// T01: Calling summarize without endSegment does NOT promote interim or reset ASR; fuller final enters once.
{
  const prompts = [];
  let resets = 0;
  const direct = createSession({
    summaryModelPath: "gemma-4-E2B-it-gpu.litertlm",
    engines: {
      asrReset() { resets += 1; },
      sumLoad() { return 0; },
      sumGenerate(prompt) { prompts.push(prompt); return "ok"; },
    },
  });
  direct.pushAsrEvent({ isFinal: 0, text: "直接总结", startMs: 0, endMs: 600 });
  const r1 = await direct.summarize();
  expect(r1 === "" && direct.fullTranscript === "" && direct.displayText === "直接总结" && prompts.length === 0 && resets === 0,
         "T01: summarize does not promote interim or reset ASR");
  direct.pushAsrEvent({ isFinal: 1, text: "直接总结这句完整的话", startMs: 0, endMs: 1200 });
  expect(direct.fullTranscript === "直接总结这句完整的话" && direct.displayText === "直接总结这句完整的话",
         "T01: fuller final enters transcript once without duplication");
  const r2 = await direct.summarize();
  expect(r2 === "ok" && prompts.length === 1 && prompts[0].includes("直接总结这句完整的话") && resets === 0,
         "T01: summarize summarizes the finalized utterance without resetting ASR");
}

// T01/T02: Rolling incremental summary (summarizeNew): summarizes only finalized segments, preserves interim & ASR state,
// sends only new finalized text on later calls, and does not block pushPcm while running.
{
  const prompts = [];
  let loads = 0;
  let resets = 0;
  let resolveGen;
  const rolling = createSession({
    summaryModelPath: "gemma-4-E2B-it-gpu.litertlm",
    engines: {
      asrPush() { return { isFinal: 1, text: "边总结边录的新句子", startMs: 6000, endMs: 6500 }; },
      asrReset() { resets += 1; },
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
  await new Promise((r) => setTimeout(r, 0));
  // While summarizeNew is running, live pushPcm is NOT blocked!
  expect(rolling.pushPcm(new Float32Array(160), 16000) === 0, "summarizeNew does not block live pushPcm while running");
  resolveGen("摘要1");
  const sum1 = await pending1;
  expect(sum1 === "摘要1" && rolling.summaryText === "摘要1", "summarizeNew returns and stores the first summary");
  expect(prompts.length === 1 && prompts[0].includes("第一段定稿") && !prompts[0].includes("第二段还在说") &&
         !prompts[0].includes("边总结边录的新句子") &&
         rolling.fullTranscript === "第一段定稿\n边总结边录的新句子" && resets === 0,
         "summarizeNew summarizes only the snapshot of finalized text without promoting interim or resetting ASR");

  const pending2 = rolling.summarizeNew();
  await new Promise((r) => setTimeout(r, 0));
  resolveGen("摘要2");
  const sum2 = await pending2;
  expect(sum2 === "摘要1\n摘要2" && rolling.summaryText === "摘要1\n摘要2", "summarizeNew appends new summary to summaryText");
  expect(prompts.length === 2 && !prompts[1].includes("第一段定稿") && prompts[1].includes("边总结边录的新句子"),
         "summarizeNew sends only the newly finalized delta on the next call");
  expect((await rolling.summarizeNew()) === "摘要1\n摘要2" && loads === 2 && resets === 0,
         "summarizeNew does not reload Gemma when there is no new finalized text");
}

// Separate identical utterances (after endSegment or >= mergeGapMs) are both retained.
{
  const s = createSession({ summaryModelPath: "gemma-4-E2B-it-gpu.litertlm" });
  s.pushAsrEvent({ isFinal: 1, text: "同样一句话", startMs: 0, endMs: 500 });
  await s.endSegment();
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

// T02: PCM is accepted across load, generate, and async unload phases; gemmaResident stays true until unload finishes.
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
  await new Promise((r) => setTimeout(r, 0));
  expect(s.gemmaResident === true && s.pushPcm(Float32Array.from([0.1]), 16000) === 0,
         "async unload: still resident while unload is pending, and PCM continues to be accepted");
  finishUnload();
  const result = await pending;
  expect(result === "摘要" && s.gemmaResident === false && s.pushPcm(Float32Array.from([0.1]), 16000) === 0,
         "async unload: gemmaResident becomes false after unload finishes");
}

// T08: reset() / release() and dispose() during a summary leave the engine to that summary: no double unload,
// and PCM continues to be accepted before dispose.
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
  await new Promise((r) => setTimeout(r, 0));
  s.reset();
  expect(unloads === 0 && s.gemmaResident === true && s.pushPcm(Float32Array.from([0.1]), 16000) === 0,
         "reset during summarize does not unload the engine under the running generation and allows PCM");
  resolveGen("旧摘要");
  await pending;
  expect(unloads === 1 && s.gemmaResident === false,
         "the summary unloads once when it finishes after reset");

  // Also test dispose() while a summary is generating: dispose() waits for the summary to unload once.
  s.pushAsrEvent({ isFinal: 1, text: "关闭前的话", startMs: 0, endMs: 500 });
  const pending2 = s.summarize();
  await new Promise((r) => setTimeout(r, 0));
  const disposing = s.dispose();
  expect(unloads === 1, "dispose() during summarize does not prematurely call sumUnload()");
  resolveGen("关闭时摘要");
  await disposing;
  await pending2;
  expect(unloads === 2 && s.gemmaResident === false, "dispose() waits for the running summary to unload once");
  await expectRejectCode(s.summarize("已关闭"), -1, "summarize after dispose rejects with -1");
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
    const got = await s.summarize();
    if (got !== want) { ok = false; console.log(`  label case [${reply}] -> [${got}]`); }
  }
  expect(ok, "summary: a leading label written by the model is removed");
}

// Calling release() / reset() while summarize or summarizeNew is running does not write back stale summaries.
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
  await new Promise((r) => setTimeout(r, 0));
  s.release();
  resolveGen("旧摘要");
  expect((await p1) === "" && s.summaryText === "", "release during summarize does not write back stale summary");

  s.pushAsrEvent({ isFinal: 1, text: "另一段旧内容", startMs: 0, endMs: 500 });
  const p2 = s.summarizeNew();
  await new Promise((r) => setTimeout(r, 0));
  s.reset();
  s.pushAsrEvent({ isFinal: 1, text: "新会话更长的一段内容", startMs: 0, endMs: 800 });
  resolveGen("旧增量摘要");
  expect((await p2) === "" && s.summaryText === "", "reset during summarizeNew does not write back stale summary");
}

// T03, T04, T05, T07, T09, D07 additional contract checks:
{
  // T03: Alternating summarizeNew and summarize
  let reply = "摘A";
  let loads = 0;
  const prompts = [];
  const s = createSession({
    summaryModelPath: "gemma-4-E2B-it-gpu.litertlm",
    engines: {
      sumLoad() { loads += 1; return 0; },
      sumGenerate(p) { prompts.push(p); return reply; },
    },
  });
  s.pushAsrEvent({ isFinal: 1, text: "阶段A", startMs: 0, endMs: 500 });
  expect((await s.summarizeNew()) === "摘A", "T03: summarizeNew summarizes A");
  s.pushAsrEvent({ isFinal: 1, text: "阶段B", startMs: 3000, endMs: 3500 });
  reply = "全AB";
  expect((await s.summarize()) === "全AB" && s.summaryText === "全AB", "T03: summarize() summarizes A+B and updates cursor");
  const loadsBefore = loads;
  expect((await s.summarizeNew()) === "全AB" && loads === loadsBefore, "T03: summarizeNew after summarize() does not reload model");
  s.pushAsrEvent({ isFinal: 1, text: "阶段C", startMs: 6000, endMs: 6500 });
  reply = "摘C";
  expect((await s.summarizeNew()) === "全AB\n摘C" && prompts.at(-1).includes("阶段C") && !prompts.at(-1).includes("阶段A"),
         "T03: subsequent summarizeNew summarizes only C and appends");

  // T04: Failure after existing summary preserves old summary and cursor
  s.pushAsrEvent({ isFinal: 1, text: "阶段D将失败", startMs: 9000, endMs: 9500 });
  reply = -5;
  await expectRejectCode(s.summarizeNew(), -5, "T04: generation failure rejects with -5");
  expect(s.summaryText === "全AB\n摘C", "T04: generation failure preserves previous summaryText");
  reply = "摘D";
  expect((await s.summarizeNew()) === "全AB\n摘C\n摘D" && s.lastError === "",
         "T04: retry after failure summarizes the unsummarized delta and clears lastError");

  // T05: External text summary (summarize(text))
  reply = "外部摘要";
  const ext = await s.summarize("外部独立文本内容");
  expect(ext === "外部摘要" && s.summaryText === "全AB\n摘C\n摘D" && !s.fullTranscript.includes("外部独立文本"),
         "T05: summarize(text) returns summary without modifying session transcript, summaryText, or cursor");
  const loadsBeforeBlank = loads;
  expect((await s.summarize("   \n\t ")) === "" && loads === loadsBeforeBlank,
         "T05/T06: blank external text returns empty string without loading Gemma");
  await expectRejectCode(s.summarize(null), -1, "T05: summarize(null) rejects with -1");
  await expectRejectCode(s.summarize(123), -1, "T05: summarize(number) rejects with -1");
}

// T09: Cross-chunk synthesis non-convergence guard (-5) and convergence
{
  let shrink = false;
  const s = createSession({
    summaryModelPath: "gemma-4-E2B-it-gpu.litertlm",
    engines: {
      sumLoad() { return 0; },
      sumGenerate(prompt) {
        if (shrink && prompt.startsWith(SUMMARY_SYNTHESIS_INSTRUCTION)) return "收敛后的最终总结";
        return "x".repeat(900);
      },
    },
  });
  s.pushAsrEvent({ isFinal: 1, text: "b".repeat(3200), startMs: 0, endMs: 500 });
  await expectRejectCode(s.summarize(), -5, "T09: non-converging multi-round synthesis rejects with -5");
  shrink = true;
  expect((await s.summarize()) === "收敛后的最终总结", "T09: shrinking multi-round synthesis converges");
}

// T07 & D07: asrFinish and onTranscript listener
{
  const events = [];
  let finishCalled = 0;
  let resetCalled = 0;
  const s = createSession({
    summaryModelPath: "gemma-4-E2B-it-gpu.litertlm",
    onTranscript(ev) { events.push(ev); },
    engines: {
      asrFinish() {
        finishCalled += 1;
        return { isFinal: 1, text: "收尾完整句", startMs: 2000, endMs: 2600 };
      },
      asrReset() { resetCalled += 1; },
    },
  });
  s.pushAsrEvent({ isFinal: 0, text: "草稿", startMs: 0, endMs: 200 });
  s.pushAsrEvent({ isFinal: 1, text: "第一句", startMs: 0, endMs: 500 });
  s.pushAsrEvent({ isFinal: 1, text: "第一句", startMs: 100, endMs: 600 }); // overlapping dedup
  s.pushAsrEvent({ isFinal: 1, text: "第二句", startMs: 800, endMs: 1200 }); // merges on display
  await s.endSegment();
  expect(
    finishCalled === 1 &&
      resetCalled === 0 &&
      events.length === 4 &&
      events[0].isFinal === 0 && events[0].text === "草稿" &&
      events[1].isFinal === 1 && events[1].text === "第一句" &&
      events[2].isFinal === 1 && events[2].text === "第二句" &&
      events[3].isFinal === 1 && events[3].text === "收尾完整句" &&
      s.fullTranscript === "第一句\n第二句\n收尾完整句",
    "T07/D07: onTranscript receives interim, non-deduped finals, and asrFinish final event",
  );
}

if (failures !== 0) {
  console.log(`${failures} failure(s)`);
  process.exit(1);
}
console.log("all session assertions passed");
