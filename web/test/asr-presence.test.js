import { spawn } from "node:child_process";
import { mkdirSync, readdirSync, writeFileSync, rmSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { createSession, probeAsrFile, presenceFromProbes, requiredAsrFiles } from "../src/index.js";

const CANNED = "你好，这是已经成稿的一句。";
const demoRoot = fileURLToPath(new URL("../demo/", import.meta.url));
const asrDir = join(demoRoot, "asr-model");
const port = 41821;
const base = `http://127.0.0.1:${port}/asr-model`;
const names = ["encoder.onnx", "decoder.onnx", "joiner.onnx", "tokens.txt"];

let failures = 0;

function expect(ok, name) {
  if (ok) console.log(`PASS ${name}`);
  else {
    console.log(`FAIL ${name}`);
    failures += 1;
  }
}

const presentFlags = { directory: false, encoder: true, decoder: true, joiner: true, tokens: true };
expect(presenceFromProbes(presentFlags) === "present", "file probes are present when the directory probe is false");
for (const missing of ["encoder", "decoder", "joiner", "tokens"]) {
  const flags = { ...presentFlags, [missing]: false };
  expect(presenceFromProbes(flags) === "absent", `missing ${missing} is absent`);
}

const files = requiredAsrFiles(base);
expect(
  files.encoder.endsWith("/encoder.onnx") &&
    files.decoder.endsWith("/decoder.onnx") &&
    files.joiner.endsWith("/joiner.onnx") &&
    files.tokens.endsWith("/tokens.txt") &&
    !Object.values(files).includes(base),
  "requiredAsrFiles names the four model files and not the directory",
);

mkdirSync(asrDir, { recursive: true });
for (const name of names) writeFileSync(join(asrDir, name), "not-a-model");

const child = spawn(process.execPath, ["server.js"], {
  cwd: demoRoot,
  env: { ...process.env, PORT: String(port) },
  stdio: ["ignore", "pipe", "pipe"],
});

function stopServer() {
  if (!child.killed) child.kill("SIGTERM");
}

try {
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("demo server did not listen")), 5000);
    let text = "";
    const onData = (chunk) => {
      text += chunk;
      if (text.includes("web demo")) {
        clearTimeout(timer);
        resolve();
      }
    };
    child.stdout.on("data", onData);
    child.stderr.on("data", onData);
    child.on("exit", (code) => {
      clearTimeout(timer);
      reject(new Error(`demo server exited ${code}: ${text}`));
    });
  });

  async function headStatus(url) {
    const response = await fetch(url, { method: "HEAD", headers: { "X-Apexo-Probe": "1" } });
    return response.status;
  }

  for (const [name, url] of Object.entries(files)) {
    const status = await headStatus(url);
    const probed = await probeAsrFile(url);
    expect(status === 200 && probed === true, `HEAD ${name} is 200 and probeAsrFile is present`);
  }
  const directoryStatus = await headStatus(base);
  const directoryProbe = await probeAsrFile(base);
  expect(directoryStatus === 204 && directoryProbe === false, "HEAD asr-model directory is 204 and probeAsrFile is absent");

  const present = createSession({
    summaryModelPath: "gemma-4-E2B-it-web.litertlm",
    asrModelPath: base,
    webSummary: true,
  });
  let loadError = null;
  try {
    await present.loadRecognizer();
  } catch (error) {
    loadError = error;
  }
  const pushed = present.pushPcm(new Float32Array(1600), 16000);
  expect(loadError != null, "present files still fail the installed recognizer");
  expect(pushed === -5 && !present.displayText.includes(CANNED), "present model does not paint canned text when the installed recognizer fails");

  rmSync(join(asrDir, "tokens.txt"), { force: true });
  const absent = createSession({
    summaryModelPath: "gemma-4-E2B-it-web.litertlm",
    asrModelPath: base,
    webSummary: true,
  });
  let absentError = null;
  try {
    await absent.loadRecognizer();
  } catch (error) {
    absentError = error;
  }
  const absentPush = absent.pushPcm(new Float32Array(1600), 16000);
  expect(absentError == null && absentPush === -3 && absent.displayText === "", "missing tokens.txt returns -3 and paints nothing");
  expect(absent.recognizerState === "missing", "missing tokens.txt leaves the recognizer in the missing state");
} finally {
  stopServer();
  let entries = [];
  try {
    entries = readdirSync(asrDir);
  } catch {
    entries = [];
  }
  if (entries.every((name) => names.includes(name))) rmSync(asrDir, { recursive: true, force: true });
}

if (failures !== 0) {
  console.log(`${failures} failure(s)`);
  process.exit(1);
}
console.log("all asr presence assertions passed");
