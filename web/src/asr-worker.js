// Runs sherpa-onnx in a module worker so the page never blocks on the model, and so the wasm can be compiled
// synchronously (browsers refuse that on the main thread).
//
// The npm package `sherpa-onnx` ships CommonJS files built for Node. They are fetched as text and evaluated here
// with small stand-ins for `require`, `process` and Node's `fs` (see node-shim.js). Model files are fetched and
// kept in the stand-in's memory.
//
// Messages in:  { type: "init", sherpaUrl, files: { encoder, decoder, joiner, tokens } }
//               { type: "pcm", gen, pcm: Float32Array }   16 kHz mono
//               { type: "end", gen }   finish the current segment: reset the stream, then answer { type: "end" }
//               { type: "reset", gen }
//               { type: "sync", id }   answered with { type: "synced", id } once everything sent before it is decoded
// Messages out: { type: "ready" } | { type: "error", message } | { type: "event", gen, event }
//               { type: "end", gen } | { type: "synced", id }

import { createAsrPipeline, onlineRecognizerConfig } from "./asr-pipeline.js";
import { createNodeShim } from "./node-shim.js";

let pipeline = null;

async function fetchChecked(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`${url} answered ${response.status}`);
  return response;
}

async function fetchText(url) {
  return (await fetchChecked(url)).text();
}

async function fetchBytes(url) {
  return new Uint8Array(await (await fetchChecked(url)).arrayBuffer());
}

function evaluateCommonJs(source, shim) {
  const module = { exports: {} };
  new Function("module", "exports", "require", "__dirname", "__filename", "Buffer", source)(
    module,
    module.exports,
    shim.require,
    "/sherpa-onnx",
    "/sherpa-onnx/sherpa-onnx-wasm-nodejs.js",
    { alloc: (size) => new Uint8Array(size) },
  );
  return module.exports;
}

async function init({ sherpaUrl, files }) {
  const base = sherpaUrl.endsWith("/") ? sherpaUrl : `${sherpaUrl}/`;
  const [glue, asrSource, wasmBinary] = await Promise.all([
    fetchText(`${base}sherpa-onnx-wasm-nodejs.js`),
    fetchText(`${base}sherpa-onnx-asr.js`),
    fetchBytes(`${base}sherpa-onnx-wasm-nodejs.wasm`),
  ]);
  const shim = createNodeShim(new Map());
  // The glue decides it runs under Node by looking at globalThis.process.
  globalThis.process = shim.process;
  const factory = evaluateCommonJs(glue, shim);
  const wasm = { wasmBinary, locateFile: (name) => `${base}${name}`, print() {}, printErr() {} };
  await Promise.resolve(factory(wasm));
  const asr = evaluateCommonJs(asrSource, shim);
  const names = { encoder: "encoder.onnx", decoder: "decoder.onnx", joiner: "joiner.onnx", tokens: "tokens.txt" };
  const written = [];
  for (const [key, name] of Object.entries(names)) {
    shim.addFile(`/asr/${name}`, await fetchBytes(files[key]));
    written.push(`/asr/${name}`);
  }
  const recognizer = asr.createOnlineRecognizer(wasm, onlineRecognizerConfig("/asr"));
  if (!recognizer || typeof recognizer.createStream !== "function" || recognizer.handle === 0) {
    throw new Error("sherpa-onnx did not create an online recognizer");
  }
  // The weights now live inside the recognizer; the copies in memory only cost space.
  for (const path of written) shim.removeFile(path);
  pipeline = createAsrPipeline(recognizer);
}

self.onmessage = async (message) => {
  const data = message.data;
  try {
    if (data.type === "init") {
      await init(data);
      self.postMessage({ type: "ready" });
    } else if (data.type === "pcm" && pipeline) {
      const event = pipeline.push(data.pcm);
      if (event.isFinal >= 0) self.postMessage({ type: "event", gen: data.gen, event });
    } else if (data.type === "end" && pipeline) {
      pipeline.reset();
      self.postMessage({ type: "end", gen: data.gen });
    } else if (data.type === "reset" && pipeline) {
      pipeline.reset();
    } else if (data.type === "sync") {
      self.postMessage({ type: "synced", id: data.id });
    }
  } catch (error) {
    self.postMessage({ type: "error", message: String(error?.message || error), stack: String(error?.stack || "") });
  }
};
