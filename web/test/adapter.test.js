import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { createSession, SUMMARY_SYNTHESIS_INSTRUCTION } from "../src/index.js";
import { createOnlineRecognizer } from "sherpa-onnx";

const fake = globalThis.__apexoFake || (globalThis.__apexoFake = {
  accepts: [],
  vad: [],
  models: [],
  prompts: [],
  deleted: 0,
  creates: 0,
  resets: 0,
});

let failures = 0;

function expect(ok, name) {
  if (ok) console.log(`PASS ${name}`);
  else {
    console.log(`FAIL ${name}`);
    failures += 1;
  }
}

const dir = mkdtempSync(join(tmpdir(), "apexo-web-adapter-"));
const asrDir = join(dir, "asr");
mkdirSync(asrDir);
const tokens = join(asrDir, "tokens.txt");
const encoder = join(asrDir, "encoder.onnx");
const decoder = join(asrDir, "decoder.onnx");
const joiner = join(asrDir, "joiner.onnx");
const silero = join(asrDir, "silero_vad.onnx");
const gemma = join(dir, "gemma-4-E2B-it-web.litertlm");
writeFileSync(tokens, "token\n");
writeFileSync(encoder, "encoder");
writeFileSync(decoder, "decoder");
writeFileSync(joiner, "joiner");
writeFileSync(silero, "vad");
writeFileSync(gemma, "gemma");

let bareConfigThrew = false;
try {
  createOnlineRecognizer({ tokens });
} catch (error) {
  bareConfigThrew = error instanceof TypeError && String(error.message).includes("transducer");
}
expect(bareConfigThrew, "sherpa-onnx rejects a recognizer config without modelConfig.transducer");

const session = createSession({
  summaryModelPath: gemma,
  asrModelPath: asrDir,
  litertWasmUrl: "https://example.test/meeting/assets/apexo/litert/",
  webSummary: true,
});
await session.loadRecognizer();
const pcm = new Float32Array(1600);
pcm[0] = 0.25;
const pushed = session.pushPcm(pcm, 16000);
const model = fake.recognizerConfig && fake.recognizerConfig.modelConfig;
expect(pushed === 0, "pushPcm accepts 16 kHz PCM");
expect(fake.creates === 1 && model && model.tokens === tokens, "sherpa-onnx createOnlineRecognizer");
expect(model && model.transducer.encoder === encoder && model.transducer.decoder === decoder && model.transducer.joiner === joiner, "recognizer config is built from the model directory");
expect(fake.recognizerConfig.featConfig.sampleRate === 16000, "recognizer feature rate is 16 kHz");
expect(fake.accepts.length === 1 && fake.accepts[0].sampleRate === 16000 && fake.accepts[0].length === 1600, "sherpa-onnx acceptWaveform 16000");
expect(fake.vad.length === 0, "no unused vad is created or fed");
expect(session.fullTranscript.includes("适配器转写"), "sherpa-onnx result enters the transcript");
expect(session.gemmaResident === false, "Gemma-not-resident-during-transcription");

const wide = new Float32Array(4800);
const widePush = session.pushPcm(wide, 48000);
expect(widePush === 0 && fake.accepts[1].sampleRate === 16000 && fake.accepts[1].length <= 1600 && fake.accepts[1].length >= 1584, "pushPcm resamples to 16 kHz before sherpa-onnx");

// Test explicit endSegment() with noEndpoint -> flushes tail via inputFinished
fake.noEndpoint = true;
fake.finishText = "尾词收尾完整";
session.pushPcm(pcm, 16000);
expect(!session.fullTranscript.includes("尾词收尾完整"), "unfinished speech is interim before endSegment");
await session.endSegment();
expect(session.fullTranscript.includes("尾词收尾完整") && fake.inputFinishedCount === 1,
       "endSegment flushes tail frames via stream.inputFinished()");
fake.noEndpoint = false;

const result = await session.summarizeWithGemma(gemma);
expect(fake.models.length === 1 && fake.models[0] === gemma, "summarize loads the web .litertlm");
expect(fake.wasmUrls.includes("https://example.test/meeting/assets/apexo/litert/"),
       "custom litertWasmUrl is passed to getOrLoadGlobalLiteRtLm");
expect(typeof result.summary === "string" && result.summary.includes("适配器摘要"), "LiteRT-LM generation is returned");
expect(fake.prompts.length >= 1 && fake.prompts[0].startsWith(SUMMARY_SYNTHESIS_INSTRUCTION), "generates from the transcript-only prompt");
expect(fake.prompts[0].includes("适配器转写") && !fake.prompts[0].includes("pcm"), "text-only-summary");
expect(fake.deleted === 1, "unloads");
expect(session.gemmaResident === false, "Gemma unloads after summarize");

const missing = join(dir, "missing-gemma-4-E2B-it-web.litertlm");
const modelsBefore = fake.models.length;
const deletedBefore = fake.deleted;
const absent = createSession({
  summaryModelPath: missing,
  asrModelPath: join(dir, "no-asr-model"),
  webSummary: true,
});
await absent.loadRecognizer({ tokens: join(dir, "no-asr-model", "tokens.txt") });
expect(absent.pushPcm(new Float32Array(1600), 16000) === -3 && absent.displayText === "", "missing asr model returns -3 and paints nothing");
expect(absent.lastError.includes("missing"), "missing asr model says why");
absent.pushAsrEvent({ isFinal: 1, text: "今天开会", startMs: 0, endMs: 800 });
let missingCode = 0;
try {
  await absent.summarize();
} catch (err) {
  missingCode = err.code;
}
expect(missingCode === -3 && absent.lastError.includes("not found"), "missing summary model rejects with code === -3, not a made-up summary");
expect(fake.models.length === modelsBefore, "missing web model does not call Engine.create");
expect(fake.deleted === deletedBefore, "missing web model does not unload an engine it did not load");

// Summary-only scenario: external text summary works without ASR model files
const textOnlySession = createSession({
  summaryModelPath: gemma,
  webSummary: true,
});
const extSummary = await textOnlySession.summarize("这是一段不需要ASR模型的外部会议纪要文本。");
expect(extSummary.includes("适配器摘要") && textOnlySession.fullTranscript === "" && textOnlySession.summaryText === "",
       "summarize(text) summarizes external text without loading ASR or mutating session state");

const weights = new Blob(["gemma-4-e2b"]);
const fromDownload = createSession({
  summaryModelPath: missing,
  asrModelPath: join(dir, "no-asr-model"),
  webSummary: true,
});
fromDownload.setSummaryModel(weights);
fromDownload.pushAsrEvent({ isFinal: 1, text: "今天开会", startMs: 0, endMs: 800 });
const downloaded = await fromDownload.summarizeWithGemma(missing);
expect(fake.models.at(-1) === weights, "a downloaded Gemma 4 E2B file is what the summarizer loads");
expect(typeof downloaded.summary === "string" && !downloaded.summary.includes("网页版 Gemma"), "a downloaded model is summarized instead of the missing-file note");

const overridden = createSession({
  summaryModelPath: missing,
  asrModelPath: join(dir, "no-asr-model"),
  webSummary: true,
});
overridden.pushAsrEvent({ isFinal: 1, text: "今天开会", startMs: 0, endMs: 800 });
const withOverride = await overridden.summarizeWithGemma(gemma);
expect(fake.models.at(-1) === gemma, "a model passed to summarizeWithGemma is the one that gets loaded");
expect(typeof withOverride.summary === "string" && withOverride.summary.includes("适配器摘要"),
       "a model passed to summarizeWithGemma replaces a missing configured path");

fake.throwOnCreate = true;
const broken = createSession({
  summaryModelPath: gemma,
  asrModelPath: asrDir,
  webSummary: true,
});
let createThrew = false;
try {
  await broken.loadRecognizer();
} catch {
  createThrew = true;
}
const brokenPush = broken.pushPcm(new Float32Array(1600), 16000);
expect(createThrew && brokenPush === -5, "present asr model does not fall back when sherpa-onnx fails to load");
expect(!broken.displayText.includes("你好，这是已经成稿的一句。"), "present model does not paint canned text when recognizer load fails");
fake.throwOnCreate = false;

rmSync(dir, { recursive: true, force: true });
if (failures !== 0) {
  console.log(`${failures} failure(s)`);
  process.exit(1);
}
console.log("all adapter assertions passed");
