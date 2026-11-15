import {
  createSession as createCore,
  isLiteRtLmFile,
  SUMMARY_INSTRUCTION,
  SUMMARY_CHUNK_INSTRUCTION,
  SUMMARY_SYNTHESIS_INSTRUCTION,
  ERR_BUSY,
  createApexoError,
} from "./session.js";
import { createStreamingResampler, resampleTo16k } from "./resample.js";
import { createAsrPipeline, onlineRecognizerConfig, requiredAsrFiles } from "./asr-pipeline.js";
import {
  SUPPORTED_LANGUAGES,
  ASR_PACKAGES,
  TRANSLATION_PACKAGES,
  normalizeLang,
  speechRecognitionLangFor,
  resolveTranslationPlan,
  getLanguagePackPlan,
  buildSummaryPrompts,
} from "./languages.js";
import {
  requiredTranslationFiles,
  translationPresenceFromProbes,
  parseSentencePieceSpm,
  createMarianHop,
  createMarianTranslator,
} from "./marian-pipeline.js";

export {
  createStreamingResampler,
  resampleTo16k,
  isLiteRtLmFile,
  requiredAsrFiles,
  requiredTranslationFiles,
  translationPresenceFromProbes,
  parseSentencePieceSpm,
  createMarianHop,
  createMarianTranslator,
  SUPPORTED_LANGUAGES,
  ASR_PACKAGES,
  TRANSLATION_PACKAGES,
  normalizeLang,
  speechRecognitionLangFor,
  resolveTranslationPlan,
  getLanguagePackPlan,
  buildSummaryPrompts,
  SUMMARY_INSTRUCTION,
  SUMMARY_CHUNK_INSTRUCTION,
  SUMMARY_SYNTHESIS_INSTRUCTION,
  ERR_BUSY,
  createApexoError,
};

export function presenceFromProbes(flags) {
  const files = ["encoder", "decoder", "joiner", "tokens"];
  return files.every((name) => flags?.[name] === true) ? "present" : "absent";
}

export async function probeAsrFile(path) {
  if (!path) return false;
  const nodeLocal = globalThis.process?.versions?.node && !/^https?:\/\//i.test(path);
  if (nodeLocal) {
    const fs = await import("node:fs");
    if (!fs.existsSync(path)) return false;
    return fs.statSync(path).isFile();
  }
  try {
    const response = await fetch(path, { method: "HEAD" });
    if (response.status === 405 || response.status === 501) return true;
    if (response.status !== 200) return false;
    // Many servers answer a missing file with their index page and status 200. A model file is never HTML.
    return !(response.headers.get("content-type") || "").toLowerCase().startsWith("text/html");
  } catch {
    return false;
  }
}

function asrDirectory(asrModelPath, modelConfig) {
  const hinted = modelConfig && typeof modelConfig.tokens === "string" ? modelConfig.tokens : "";
  const raw = asrModelPath || hinted;
  if (!raw) return "";
  if (raw.endsWith("/tokens.txt") || raw.endsWith("\\tokens.txt")) return raw.slice(0, -"/tokens.txt".length);
  return raw;
}

const isNode = () => Boolean(globalThis.process?.versions?.node);

// Runs sherpa-onnx in a module worker. Decoded events come back asynchronously; `gen` drops events that belong
// to audio from before the last reset/release.
function startWorkerRecognizer({ files, sherpaUrl, onEvent, onFail }) {
  const worker = new Worker(new URL("./asr-worker.js", import.meta.url), { type: "module" });
  let gen = 0;
  let syncId = 0;
  let endId = 0;
  const syncs = new Map();
  const ends = new Map();
  const settleSyncs = () => {
    for (const resolve of syncs.values()) resolve();
    syncs.clear();
  };
  const settleEnds = (err) => {
    for (const entry of ends.values()) {
      if (err) entry.reject(err);
      else entry.resolve();
    }
    ends.clear();
  };
  const ready = new Promise((resolve, reject) => {
    worker.onmessage = (message) => {
      const data = message.data;
      if (data.type === "ready") resolve();
      else if (data.type === "error") {
        const error = createApexoError(-5, data.message || "the recognizer worker failed");
        if (data.stack) error.workerStack = data.stack;
        reject(error);
        onFail(error);
        settleSyncs();
        settleEnds(error);
      } else if (data.type === "event" && data.gen === gen) onEvent(data.event);
      else if (data.type === "end") {
        const pending = ends.get(data.id);
        ends.delete(data.id);
        pending?.resolve();
      } else if (data.type === "synced") {
        syncs.get(data.id)?.();
        syncs.delete(data.id);
      }
    };
    worker.onerror = (event) => {
      const error = createApexoError(-5, event.message || "the recognizer worker failed");
      settleSyncs();
      settleEnds(error);
      reject(error);
      onFail(error);
    };
  });
  worker.postMessage({ type: "init", files, sherpaUrl });
  return {
    ready,
    push(pcm) {
      worker.postMessage({ type: "pcm", gen, pcm }, [pcm.buffer]);
      return null;
    },
    end() {
      return new Promise((resolve, reject) => {
        const id = (endId += 1);
        ends.set(id, { resolve, reject });
        worker.postMessage({ type: "end", gen, id });
      });
    },
    reset() {
      gen += 1;
      settleEnds();
      worker.postMessage({ type: "reset", gen });
    },
    // Resolves after the worker has decoded all audio sent so far and its results have reached this thread.
    sync() {
      return new Promise((resolve) => {
        const id = (syncId += 1);
        syncs.set(id, resolve);
        worker.postMessage({ type: "sync", id });
      });
    },
    free() {
      worker.terminate();
      settleSyncs();
      settleEnds();
    },
  };
}

// Runs offline MarianMT ONNX translation in a dedicated browser module worker.
function startWorkerTranslator({ hops, ortUrl, onFail }) {
  const worker = new Worker(new URL("./translation-worker.js", import.meta.url), { type: "module" });
  let gen = 0;
  let reqId = 0;
  let syncId = 0;
  const pending = new Map();
  const syncs = new Map();
  const rejectPending = (err) => {
    for (const entry of pending.values()) entry.reject(err);
    pending.clear();
    for (const resolve of syncs.values()) resolve();
    syncs.clear();
  };
  const ready = new Promise((resolve, reject) => {
    worker.onmessage = (message) => {
      const data = message.data || {};
      if (data.type === "ready") {
        resolve();
      } else if (data.type === "init-error") {
        const err = createApexoError(-5, data.message || "translation worker failed to initialize");
        if (data.stack) err.workerStack = data.stack;
        reject(err);
        onFail(err);
        rejectPending(err);
      } else if (data.type === "translated") {
        const entry = pending.get(data.id);
        pending.delete(data.id);
        entry?.resolve(data.stale ? "" : data.text);
      } else if (data.type === "translate-error") {
        const entry = pending.get(data.id);
        pending.delete(data.id);
        entry?.reject(createApexoError(data.code || -5, data.message || "translation failed"));
      } else if (data.type === "synced") {
        syncs.get(data.id)?.();
        syncs.delete(data.id);
      }
    };
    worker.onerror = (event) => {
      const err = createApexoError(-5, event.message || "translation worker failed");
      reject(err);
      onFail(err);
      rejectPending(err);
    };
  });
  worker.postMessage({ type: "init", hops, ortUrl });
  return {
    ready,
    translate(text) {
      return new Promise((resolve, reject) => {
        const id = (reqId += 1);
        pending.set(id, { resolve, reject });
        worker.postMessage({ type: "translate", id, gen, text });
      });
    },
    reset() {
      gen += 1;
      worker.postMessage({ type: "reset", gen });
    },
    sync() {
      return new Promise((resolve) => {
        const id = (syncId += 1);
        syncs.set(id, resolve);
        worker.postMessage({ type: "sync", id });
      });
    },
    free() {
      worker.terminate();
      rejectPending(createApexoError(-1, "translator closed"));
    },
  };
}

function streamFromChunks(chunks) {
  let index = 0;
  return new ReadableStream({
    pull(controller) {
      if (index >= chunks.length) {
        controller.close();
        return;
      }
      const chunk = chunks[index++];
      controller.enqueue(chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk));
    },
  });
}

async function resolveModelSource(downloaded, path) {
  if (downloaded == null) return path;
  if (Array.isArray(downloaded)) return streamFromChunks(downloaded);
  if (downloaded instanceof Uint8Array || downloaded instanceof ArrayBuffer) return streamFromChunks([downloaded]);
  if (typeof downloaded === "function") return downloaded();
  if (downloaded instanceof Blob && !isNode()) {
    try {
      await downloaded.slice(0, 1).arrayBuffer();
      return downloaded;
    } catch {
      return path;
    }
  }
  return downloaded;
}

function readGeneratedText(response) {
  const content = response?.content;
  if (typeof content === "string") return content;
  if (Array.isArray(content)) {
    for (const part of content) {
      if (part && typeof part.text === "string") return part.text;
    }
  }
  if (typeof response?.text === "string") return response.text;
  return "";
}

function normalizeHopList(raw) {
  if (!raw) return [];
  if (Array.isArray(raw)) return raw.filter(Boolean);
  return [raw];
}

export function createSession(options = {}) {
  const summaryModelPath = options.summaryModelPath ?? "gemma-4-E2B-it-gpu.litertlm";
  const litertWasmUrl = options.litertWasmUrl;
  const custom = options.engines;
  let configuredAsrPath = options.asrModelPath || "";
  let configuredTranslationHops = normalizeHopList(options.translationModelPaths ?? options.translationModelPath);
  let sherpaModule = null;
  let recognizer = null;
  let backend = null; // { push(pcm) -> event | null, finish?(), end?(), reset(), free() }
  let asrMode = "unknown"; // unknown | missing | live | failed
  let transBackend = null; // { translate(text) -> Promise<string>, reset?(), sync?(), free() }
  let transMode = "unknown"; // unknown | missing | live | failed | passthrough
  let modelExists = false;
  let pendingLitert = null;
  let downloadedSummary = null;
  let liveEngine = null;
  let liveConversation = null;
  let callModel = null; // model path passed to summarizeWithGemma for the call that is running
  let localError = "";
  const resampleStream = createStreamingResampler();

  async function drain() {
    if (!custom && asrMode === "live" && backend && typeof backend.sync === "function") {
      await backend.sync();
    }
    await core.flushTranslations();
  }

  const defaultEngines = {
    asrPush(pcm) {
      if (asrMode !== "live" || !backend) return { isFinal: -1, text: "", startMs: 0, endMs: 0 };
      return backend.push(pcm) || { isFinal: -1, text: "", startMs: 0, endMs: 0 };
    },
    async asrFinish() {
      if (asrMode === "failed") {
        throw createApexoError(-5, "the recognizer failed to load");
      }
      if (asrMode !== "live" || !backend) {
        return { fallback: true };
      }
      if (typeof backend.finish === "function") {
        return backend.finish();
      }
      if (typeof backend.end === "function") {
        await backend.end();
        return { isFinal: -1, text: "", startMs: 0, endMs: 0 };
      }
      return { fallback: true };
    },
    asrReset() {
      if (!backend) return;
      if (typeof backend.reset === "function") backend.reset();
    },
    transReset() {
      if (transBackend && typeof transBackend.reset === "function") transBackend.reset();
    },
    transTranslate(text) {
      if (transMode === "failed") return -5;
      if (transMode !== "live" || !transBackend) return -3;
      return transBackend.translate(text);
    },
    async beforeSnapshot() {
      await drain();
    },
    async beforeSummarize(chosenPath) {
      const path = chosenPath || summaryModelPath;
      let litert = null;
      let importErr = null;
      try {
        litert = await import("@litert-lm/core");
      } catch (err) {
        importErr = err;
      }
      const override = chosenPath && chosenPath !== summaryModelPath ? chosenPath : null;
      modelExists = override ? await probeAsrFile(path) : downloadedSummary != null || (await probeAsrFile(path));
      pendingLitert = litert;
      callModel = override;
      localError = "";
      if (importErr && modelExists) {
        throw createApexoError(-5, importErr.message || "gemma failed to load");
      }
    },
    // 0: loaded. -3: the model file is not there. -5: it is there but could not be loaded.
    sumLoad(path) {
      if (!modelExists) return -3;
      const create = pendingLitert?.Engine?.create;
      if (typeof create !== "function") return -5;
      const chosen = callModel; // read now: another call cannot enter while summarizing is true
      return prepareWebGemmaRuntime(pendingLitert, litertWasmUrl)
        .then(() => (chosen ? chosen : resolveModelSource(downloadedSummary, path)))
        .then((model) => create.call(pendingLitert.Engine, { model }))
        .then(async (engine) => {
          liveEngine = engine;
          liveConversation = await engine.createConversation();
          return 0;
        })
        .finally(() => {
          callModel = null;
        });
    },
    // The conversation must be destroyed before its engine.
    async sumUnload() {
      const conversation = liveConversation;
      const engine = liveEngine;
      liveConversation = null;
      liveEngine = null;
      try {
        if (conversation && typeof conversation.delete === "function") await conversation.delete();
      } catch {
        // Already gone.
      }
      try {
        if (engine && typeof engine.delete === "function") await engine.delete();
      } catch {
        // Already gone.
      }
    },
    // Every chunk gets a fresh conversation, so chunks of one transcript do not see each other as chat history.
    async sumGenerate(prompt) {
      if (!liveEngine || typeof liveEngine.createConversation !== "function") return -5;
      if (liveConversation && liveConversation.used) {
        const old = liveConversation;
        liveConversation = await liveEngine.createConversation();
        try {
          if (typeof old.delete === "function") await old.delete();
        } catch {
          // Already gone.
        }
      }
      if (!liveConversation || typeof liveConversation.sendMessage !== "function") return -5;
      const response = await liveConversation.sendMessage(prompt);
      liveConversation.used = true;
      return readGeneratedText(response);
    },
  };

  const core = createCore({
    mergeGapMs: options.mergeGapMs,
    summaryChunkChars: options.summaryChunkChars,
    displayLines: options.displayLines,
    translationIntervalMs: options.translationIntervalMs,
    summaryModelPath,
    webSummary: options.webSummary !== false,
    sourceLang: options.sourceLang,
    targetLang: options.targetLang,
    onTranscript: options.onTranscript,
    onTranslation: options.onTranslation,
    onTranslationUpdate: () => {
      if (typeof options.onChange === "function") options.onChange();
    },
    engines: custom || defaultEngines,
  });

  function freeRecognizer() {
    for (const handle of [backend, recognizer]) {
      try {
        if (handle && typeof handle.free === "function") handle.free();
      } catch {
        // Already freed.
      }
    }
    backend = null;
    recognizer = null;
  }

  async function freeTranslator() {
    const prev = transBackend;
    transBackend = null;
    if (!prev) return;
    try {
      if (typeof prev.free === "function") await prev.free();
    } catch {
      // Already freed.
    }
  }

  function onWorkerEvent(event) {
    core.pushAsrEvent(event);
    if (typeof options.onChange === "function") options.onChange();
  }

  function onWorkerFail() {
    asrMode = "failed";
  }

  function onTransWorkerFail() {
    transMode = "failed";
  }

  // Node loads the sherpa-onnx package in this thread. A browser starts a worker that loads the package's wasm
  // from `options.sherpaUrl` (default /vendor/sherpa-onnx/) and the model files from the model directory.
  async function loadRecognizer(modelConfig = {}) {
    if (asrMode === "live" && backend) return sherpaModule || backend;
    const dir = asrDirectory(configuredAsrPath, modelConfig);
    const files = requiredAsrFiles(dir);
    const flags = {
      encoder: await probeAsrFile(files.encoder),
      decoder: await probeAsrFile(files.decoder),
      joiner: await probeAsrFile(files.joiner),
      tokens: await probeAsrFile(files.tokens),
    };
    if (presenceFromProbes(flags) !== "present") {
      asrMode = "missing";
      return null;
    }
    try {
      if (isNode()) {
        sherpaModule = await import("sherpa-onnx");
        if (typeof options.onRecognizer === "function") options.onRecognizer(sherpaModule, modelConfig);
        if (typeof sherpaModule.createOnlineRecognizer !== "function") {
          throw new Error("sherpa-onnx did not export createOnlineRecognizer");
        }
        recognizer = sherpaModule.createOnlineRecognizer(onlineRecognizerConfig(dir, modelConfig));
        if (!recognizer || typeof recognizer.createStream !== "function" || recognizer.handle === 0) {
          throw new Error("sherpa-onnx did not create an online recognizer");
        }
        const pipeline = createAsrPipeline(recognizer);
        backend = {
          push: (pcm) => pipeline.push(pcm),
          finish: () => pipeline.finish(),
          reset: () => pipeline.reset(),
          free: () => pipeline.free(),
        };
      } else {
        if (typeof Worker === "undefined") throw new Error("this browser has no Worker");
        const sherpaUrl = options.sherpaUrl
          ? new URL(options.sherpaUrl, globalThis.location.href).href
          : new URL("/vendor/sherpa-onnx/", globalThis.location.href).href;
        backend = startWorkerRecognizer({
          files: Object.fromEntries(
            Object.entries(files).map(([key, path]) => [key, new URL(path, globalThis.location.href).href]),
          ),
          sherpaUrl,
          onEvent: onWorkerEvent,
          onFail: onWorkerFail,
        });
        await backend.ready;
        sherpaModule = null;
      }
    } catch (error) {
      asrMode = "failed";
      freeRecognizer();
      throw error;
    }
    asrMode = "live";
    return sherpaModule || backend;
  }

  // Loads 1-hop or 2-hop MarianMT ONNX translation models in Node (via onnxruntime-web) or a Browser Worker.
  async function loadTranslator(overrideHops) {
    if (overrideHops !== undefined) {
      await freeTranslator();
      configuredTranslationHops = normalizeHopList(overrideHops);
      transMode = "unknown";
    }
    if (core.sourceLang && core.sourceLang === core.targetLang) {
      await freeTranslator();
      transMode = "passthrough";
      return { mode: "passthrough" };
    }
    if (!core.targetLang) {
      await freeTranslator();
      transMode = "passthrough";
      return { mode: "passthrough" };
    }
    if (transMode === "live" && transBackend) return transBackend;
    if (configuredTranslationHops.length === 0) {
      transMode = "missing";
      return null;
    }

    const resolvedHops = [];
    for (const hop of configuredTranslationHops) {
      if (typeof hop === "string") {
        const files = requiredTranslationFiles(hop);
        const flags = {
          config: await probeAsrFile(files.config),
          vocab: await probeAsrFile(files.vocab),
          sourceSpm: await probeAsrFile(files.sourceSpm),
          encoder: await probeAsrFile(files.encoder),
          decoder: await probeAsrFile(files.decoder),
        };
        if (translationPresenceFromProbes(flags) !== "present") {
          transMode = "missing";
          return null;
        }
        resolvedHops.push(files);
      } else if (hop && typeof hop === "object") {
        resolvedHops.push(hop);
      }
    }

    try {
      if (isNode()) {
        const ortMod = await import("onnxruntime-web");
        const ort = ortMod.default ?? ortMod;
        ort.env.wasm.numThreads = 1;
        const pipeline = await createMarianTranslator(ort, resolvedHops);
        transBackend = {
          translate: (text) => pipeline.translate(text),
          free: () => pipeline.free(),
        };
      } else {
        if (typeof Worker === "undefined") throw new Error("this browser has no Worker");
        const ortUrl = options.ortUrl
          ? new URL(options.ortUrl, globalThis.location.href).href
          : new URL("/assets/apexo/ort/", globalThis.location.href).href;
        const workerHops = resolvedHops.map((files) =>
          Object.fromEntries(
            Object.entries(files).map(([k, v]) => [
              k,
              typeof v === "string" ? new URL(v, globalThis.location.href).href : v,
            ]),
          ),
        );
        transBackend = startWorkerTranslator({
          hops: workerHops,
          ortUrl,
          onFail: onTransWorkerFail,
        });
        await transBackend.ready;
      }
    } catch (error) {
      transMode = "failed";
      await freeTranslator();
      throw error;
    }
    transMode = "live";
    return transBackend;
  }

  async function summarizeWithGemma(model, { incremental = false, text } = {}) {
    const summary = await core.summarizeInternal({
      text,
      incremental,
      overrideModel: model || undefined,
    });
    return { litert: pendingLitert, summary };
  }

  const api = {
    get gemmaResident() {
      return core.gemmaResident;
    },
    get displayText() {
      return core.displayText;
    },
    get fullTranscript() {
      return core.fullTranscript;
    },
    get translatedDisplayText() {
      return core.translatedDisplayText;
    },
    get fullTranslation() {
      return core.fullTranslation;
    },
    get translationPreviewText() {
      return core.translationPreviewText;
    },
    get translationPending() {
      return core.translationPending;
    },
    get translationError() {
      return core.translationError;
    },
    get summaryText() {
      return core.summaryText;
    },
    get lastError() {
      return localError || core.lastError;
    },
    get recognizerState() {
      return asrMode;
    },
    get translatorState() {
      if (core.sourceLang && core.sourceLang === core.targetLang) return "passthrough";
      if (!core.targetLang) return "passthrough";
      return transMode;
    },
    get sourceLang() {
      return core.sourceLang;
    },
    get targetLang() {
      return core.targetLang;
    },
    setLanguages(sourceLang, targetLang) {
      core.setLanguages(sourceLang, targetLang);
    },
    setAsrModelPath(nextAsrPath) {
      const next = nextAsrPath || "";
      if (next === configuredAsrPath) return;
      freeRecognizer();
      configuredAsrPath = next;
      asrMode = "unknown";
    },
    async setTranslationModelPaths(nextHops) {
      await freeTranslator();
      configuredTranslationHops = normalizeHopList(nextHops);
      transMode = "unknown";
    },
    setTranscriptListener(listener) {
      core.setTranscriptListener(listener);
    },
    setTranslationListener(listener) {
      core.setTranslationListener(listener);
    },
    // -3: no recognizer (model files missing or not loaded yet). -5: the recognizer failed to load.
    pushPcm(samples, sampleRate) {
      if (!samples || typeof sampleRate !== "number" || sampleRate < 16000) {
        return core.pushPcm(samples, sampleRate);
      }
      localError = "";
      if (!custom && asrMode !== "live") {
        if (asrMode === "failed") {
          localError = "the recognizer failed to load";
          return -5;
        }
        localError = asrMode === "missing" ? "recognizer model files are missing" : "recognizer is not loaded";
        return -3;
      }
      const pcm = resampleStream(samples, sampleRate);
      if (!pcm.length) return 0;
      return core.pushPcm(pcm, 16000);
    },
    pushAsrEvent(event) {
      return core.pushAsrEvent(event);
    },
    async endSegment() {
      const tail = resampleStream.flush();
      if (tail.length && (custom || asrMode === "live")) {
        core.pushPcm(tail, 16000);
      }
      await core.endSegment();
      if (typeof options.onChange === "function") options.onChange();
    },
    translate(text) {
      return core.translate(text);
    },
    summarize(text) {
      return core.summarize(text);
    },
    summarizeNew() {
      return core.summarizeNew();
    },
    // Clears the transcript, summary, cursor and recognizer state while keeping any loaded recognizer ready.
    reset() {
      resampleStream.flush();
      localError = "";
      core.reset();
    },
    // Compatibility alias for reset().
    release() {
      api.reset();
    },
    // Frees the recognizer, translator, and waits for any in-flight summary to finish unloading its Gemma engine.
    async dispose() {
      resampleStream.flush();
      localError = "";
      await core.dispose();
      freeRecognizer();
      await freeTranslator();
      if (asrMode === "live") asrMode = "unknown";
      if (transMode === "live") transMode = "unknown";
    },
    loadRecognizer,
    loadTranslator,
    drain,
    summarizeWithGemma,
    setSummaryModel(model) {
      downloadedSummary = model ?? null;
    },
  };
  return api;
}

function prepareWebGemmaRuntime(litert, litertWasmUrl) {
  if (!litert || typeof litert.getOrLoadGlobalLiteRtLm !== "function") return Promise.resolve();
  const rawUrl = litertWasmUrl || "/vendor/@litert-lm/core/wasm/";
  if (typeof location !== "undefined" && location.href) {
    return litert.getOrLoadGlobalLiteRtLm(new URL(rawUrl, location.href).href);
  }
  if (litertWasmUrl) {
    return litert.getOrLoadGlobalLiteRtLm(rawUrl);
  }
  return Promise.resolve();
}
