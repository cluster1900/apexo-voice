import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { createOnlineRecognizer } from "sherpa-onnx";
import { createSession } from "../src/index.js";

let failures = 0;

function expect(ok, name) {
  if (ok) console.log(`PASS ${name}`);
  else {
    console.log(`FAIL ${name}`);
    failures += 1;
  }
}

let bareThrew = false;
try {
  createOnlineRecognizer({ tokens: "tokens.txt", sileroVad: { model: "silero_vad.onnx" } });
} catch (error) {
  bareThrew = error instanceof TypeError && String(error.message).includes("transducer");
}
expect(bareThrew, "installed sherpa-onnx rejects a recognizer config without modelConfig.transducer");

const dir = mkdtempSync(join(tmpdir(), "apexo-real-sherpa-"));
const asrDir = join(dir, "asr");
mkdirSync(asrDir);
for (const name of ["encoder.onnx", "decoder.onnx", "joiner.onnx", "silero_vad.onnx"]) {
  writeFileSync(join(asrDir, name), "not-a-model");
}
writeFileSync(join(asrDir, "tokens.txt"), "a 1\n");
const gemma = join(dir, "gemma-4-E2B-it-web.litertlm");
writeFileSync(gemma, "gemma");

const session = createSession({
  summaryModelPath: gemma,
  asrModelPath: asrDir,
  webSummary: true,
});
let loadError = null;
try {
  await session.loadRecognizer();
} catch (error) {
  loadError = error;
}
const pushed = session.pushPcm(new Float32Array(1600), 16000);
const canned = session.displayText.includes("你好，这是已经成稿的一句。");
if (loadError == null) {
  expect(pushed === 0 && !canned, "installed sherpa-onnx accepted the directory config and pushPcm");
} else {
  const inOperator = loadError instanceof TypeError && String(loadError.message).includes("transducer");
  expect(!inOperator, "directory config passes sherpa-onnx modelConfig.transducer");
  expect(pushed === -5 && !canned, "present model does not paint canned text when the installed recognizer fails");
}

rmSync(dir, { recursive: true, force: true });
if (failures !== 0) {
  console.log(`${failures} failure(s)`);
  process.exit(1);
}
console.log("all real sherpa config assertions passed");
