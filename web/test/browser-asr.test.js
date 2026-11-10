// Runs the recognizer worker in Chromium with a real streaming model. Opt-in: the model is not in the repo.
//
//   APEXO_TEST_ASR_DIR=/path/to/dir node ./test/browser-asr.test.js
//
// The directory holds encoder.onnx, decoder.onnx, joiner.onnx and tokens.txt (a streaming zipformer
// transducer), plus a 16 kHz mono 16-bit wav at either wav/0.wav or test_wavs/0.wav. The sentence in the wav
// must contain the text in APEXO_TEST_ASR_EXPECT (default: 研究感兴趣, for the zh-14M test_wavs/0.wav).
import { spawn } from "node:child_process";
import { existsSync, readFileSync, unlinkSync, symlinkSync, lstatSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const modelDir = process.env.APEXO_TEST_ASR_DIR;
if (!modelDir) {
  console.log("SKIP browser-asr: set APEXO_TEST_ASR_DIR to run it with a real model");
  process.exit(0);
}
const wavPath = [join(modelDir, "wav/0.wav"), join(modelDir, "test_wavs/0.wav")].find(existsSync);
for (const name of ["encoder.onnx", "decoder.onnx", "joiner.onnx", "tokens.txt"]) {
  if (!existsSync(join(modelDir, name))) throw new Error(`${name} is missing in ${modelDir}`);
}
if (!wavPath) throw new Error("no wav/0.wav or test_wavs/0.wav in the model directory");
const expected = process.env.APEXO_TEST_ASR_EXPECT || "研究感兴趣";

const { chromium } = await import("playwright");
const demoRoot = fileURLToPath(new URL("../demo/", import.meta.url));
const link = join(demoRoot, "asr-model");
const port = 41823;
let failures = 0;
function expect(ok, name) {
  if (ok) console.log(`PASS ${name}`);
  else {
    console.log(`FAIL ${name}`);
    failures += 1;
  }
}

let linked = false;
try {
  lstatSync(link);
  throw new Error(`${link} already exists; remove it and run again`);
} catch (error) {
  if (error.code !== "ENOENT") throw error;
}
symlinkSync(modelDir, link);
linked = true;
const server = spawn(process.execPath, ["server.js"], { cwd: demoRoot, env: { ...process.env, PORT: String(port) }, stdio: "ignore" });
try {
  await new Promise((resolve) => setTimeout(resolve, 1000));
  const wav = readFileSync(wavPath);
  const browser = await chromium.launch();
  const page = await browser.newPage();
  const pageErrors = [];
  page.on("pageerror", (error) => pageErrors.push(error.message));
  await page.goto(`http://127.0.0.1:${port}/`);
  const result = await page.evaluate(async ({ base64, expected }) => {
    const bytes = Uint8Array.from(atob(base64), (c) => c.charCodeAt(0));
    const pcm16 = new Int16Array(bytes.buffer.slice(44, 44 + ((bytes.length - 44) & ~1)));
    const samples = Float32Array.from(pcm16, (v) => v / 32768);
    const { createSession } = await import("/sdk/index.js");
    let changes = 0;
    const session = createSession({ asrModelPath: "asr-model", webSummary: true, onChange: () => { changes += 1; } });
    await session.loadRecognizer();
    const out = { state: session.recognizerState };
    for (let i = 0; i < samples.length; i += 1600) session.pushPcm(samples.slice(i, i + 1600), 16000);
    for (let i = 0; i < 48; i += 1) session.pushPcm(new Float32Array(1600), 16000);
    const deadline = performance.now() + 20000;
    while (performance.now() < deadline && !session.fullTranscript.includes(expected)) await new Promise((r) => setTimeout(r, 100));
    out.transcript = session.fullTranscript;
    out.changes = changes;
    out.blankLines = /\n\s*\n/.test(session.fullTranscript);
    session.release();
    await new Promise((r) => setTimeout(r, 300));
    out.afterRelease = session.fullTranscript;
    // Ending mid-utterance with 48 kHz input (what the AudioWorklet delivers): the resampler tail and the audio
    // still queued in the worker must finish this line, not start a second copy of it.
    const part = samples.slice(0, Math.floor(samples.length * 0.6));
    const wide = new Float32Array(part.length * 3);
    for (let i = 0; i < wide.length; i += 1) wide[i] = part[Math.floor(i / 3)];
    for (let i = 0; i < wide.length; i += 4800) session.pushPcm(wide.slice(i, i + 4800), 48000);
    session.endSegment();
    out.immediate = await session.summarizeWithGemma("absent-for-this-test.litertlm").then(() => session.fullTranscript, () => session.fullTranscript);
    // Rolling summary in the middle of an utterance: the unfinished words are included as a line, and the rest of
    // the speech continues on a new line instead of repeating them.
    session.release();
    const cut = Math.floor(samples.length * 0.6);
    for (let i = 0; i < cut; i += 1600) session.pushPcm(samples.slice(i, Math.min(i + 1600, cut)), 16000);
    await session.drain();
    await session.summarizeWithGemma("absent-for-this-test.litertlm", { incremental: true }).catch(() => {});
    out.rollingFirst = session.fullTranscript;
    for (let i = cut; i < samples.length; i += 1600) session.pushPcm(samples.slice(i, i + 1600), 16000);
    for (let i = 0; i < 48; i += 1) session.pushPcm(new Float32Array(1600), 16000);
    await session.drain();
    out.rollingAll = session.fullTranscript;
    session.dispose();
    return out;
  }, { base64: wav.toString("base64"), expected });
  await browser.close();
  expect(result.state === "live", "the recognizer worker loads the model and goes live");
  expect(result.transcript.includes(expected), "the worker decodes the wav with the real model");
  expect(result.changes > 0, "onChange fires for events that arrive after pushPcm returned");
  expect(!result.blankLines, "silence after the speech adds no blank lines");
  expect(result.afterRelease === "", "release clears the transcript and drops late events");
  expect(result.immediate.length > 0 && !result.immediate.includes("\n"),
         "ending mid-utterance at 48 kHz keeps one line and summarize waits for the queued speech");
  {
    const lines = result.rollingAll.split("\n");
    expect(result.rollingFirst.length > 0 && lines[0] === result.rollingFirst && lines.length === 2 &&
           !lines[1].startsWith(result.rollingFirst.slice(0, 4)),
           "rolling summary mid-utterance includes the unfinished words once; the rest continues on the next line");
  }
  expect(pageErrors.length === 0, "the page stays free of errors");
} finally {
  server.kill("SIGTERM");
  if (linked) unlinkSync(link);
}
if (failures !== 0) {
  console.log(`${failures} failure(s)`);
  process.exit(1);
}
console.log("all browser asr assertions passed");
