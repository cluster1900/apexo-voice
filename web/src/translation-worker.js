import { createMarianTranslator } from "./marian-pipeline.js";

let translator = null;
let generation = 0;
let queue = Promise.resolve();

self.onmessage = (message) => {
  const data = message.data || {};
  if (data.type === "init") {
    queue = queue.then(async () => {
      try {
        const base = new URL(data.ortUrl, self.location.href).href;
        const ortMod = await import(new URL("ort.wasm.min.mjs", base).href);
        const ort = ortMod.default ?? ortMod;
        ort.env.wasm.wasmPaths = base;
        ort.env.wasm.numThreads = 1;
        translator = await createMarianTranslator(ort, data.hops);
        self.postMessage({ type: "ready" });
      } catch (err) {
        self.postMessage({
          type: "init-error",
          message: String(err?.message || err || "translation worker init failed"),
          stack: err?.stack ? String(err.stack) : "",
        });
      }
    });
    return;
  }
  if (data.type === "reset") {
    generation = data.gen;
    return;
  }
  if (data.type === "translate") {
    const { id, gen, text } = data;
    queue = queue.then(async () => {
      if (gen !== generation) {
        self.postMessage({ type: "translated", id, gen, stale: true, text: "" });
        return;
      }
      if (!translator) {
        self.postMessage({ type: "translate-error", id, gen, code: -3, message: "translator is not loaded" });
        return;
      }
      try {
        const out = await translator.translate(text);
        self.postMessage({ type: "translated", id, gen, stale: gen !== generation, text: out });
      } catch (err) {
        self.postMessage({
          type: "translate-error",
          id,
          gen,
          code: -5,
          message: String(err?.message || err || "translation failed"),
        });
      }
    });
    return;
  }
  if (data.type === "sync") {
    const { id } = data;
    queue = queue.then(() => {
      self.postMessage({ type: "synced", id });
    });
  }
};
