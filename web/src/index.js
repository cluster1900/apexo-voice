import { createSession as createCore, isWebGemmaFile, SUMMARY_INSTRUCTION } from "./session.js";
import { createStreamingResampler, resampleTo16k } from "./resample.js";

export { createStreamingResampler, resampleTo16k, isWebGemmaFile, SUMMARY_INSTRUCTION };

const CANNED_SUMMARY = "（网页版 Gemma 权重由宿主提供，这里只提交转写文本）";

export function requiredAsrFiles(dir) {
  return {
    encoder: joinPath(dir, "encoder.onnx"),
    decoder: joinPath(dir, "decoder.onnx"),
    joiner: joinPath(dir, "joiner.onnx"),
    tokens: joinPath(dir, "tokens.txt"),
  };
}

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
    const response = await fetch(path, { method: "HEAD", headers: { "X-Apexo-Probe": "1" } });
    return response.status === 200;
  } catch {
    return false;
  }
}

function cannedEvent(calls) {
  if (calls === 1) {
    return { isFinal: 1, text: "你好，这是已经成稿的一句。", startMs: 0, endMs: 800 };
  }
  return { isFinal: 0, text: "下一句还在识别", startMs: 4000, endMs: 4400 };
}

function joinPath(dir, name) {
  if (!dir) return name;
  if (dir.endsWith("/") || dir.endsWith("\\")) return dir + name;
  return `${dir}/${name}`;
}

function asrDirectory(asrModelPath, modelConfig) {
  const hinted = modelConfig && typeof modelConfig.tokens === "string" ? modelConfig.tokens : "";
  const raw = asrModelPath || hinted;
  if (!raw) return "";
  if (raw.endsWith("/tokens.txt") || raw.endsWith("\\tokens.txt")) return raw.slice(0, -"/tokens.txt".length);
  return raw;
}

function onlineRecognizerConfig(dir) {
  const files = requiredAsrFiles(dir);
  return {
    featConfig: { sampleRate: 16000, featureDim: 80 },
    modelConfig: {
      transducer: {
        encoder: files.encoder,
        decoder: files.decoder,
        joiner: files.joiner,
      },
      tokens: files.tokens,
      numThreads: 1,
      provider: "cpu",
      debug: 0,
      modelingUnit: "cjkchar",
    },
    decodingMethod: "greedy_search",
    maxActivePaths: 4,
    enableEndpoint: 1,
    rule1MinTrailingSilence: 2.4,
    rule2MinTrailingSilence: 1.2,
    rule3MinUtteranceLength: 20,
  };
}

function vadConfig(silero) {
  return {
    sileroVad: {
      model: silero,
      threshold: 0.5,
      minSilenceDuration: 0.5,
      minSpeechDuration: 0.25,
      maxSpeechDuration: 20,
      windowSize: 512,
    },
    sampleRate: 16000,
    numThreads: 1,
    provider: "cpu",
    debug: 0,
    bufferSizeInSeconds: 30,
  };
}

function readGeneratedText(response) {
  const content = response?.content;
  if (typeof content === "string") return content;
  if (Array.isArray(content) && content[0] && typeof content[0].text === "string") return content[0].text;
  return "";
}

export function createSession(options = {}) {
  const summaryModelPath = options.summaryModelPath || "gemma-4-E2B-it-web.litertlm";
  const custom = options.engines;
  let sherpaModule = null;
  let recognizer = null;
  let stream = null;
  let vad = null;
  let asrMode = "unknown";
  let cannedCalls = 0;
  let fed = 0;
  let modelExists = false;
  let pendingLitert = null;
  let downloadedSummary = null;
  let liveEngine = null;
  let liveConversation = null;
  const resampleStream = createStreamingResampler();

  const defaultEngines = {
    asrPush(pcm) {
      if (asrMode === "canned") {
        cannedCalls += 1;
        return cannedEvent(cannedCalls);
      }
      if (asrMode !== "live" || !recognizer || !stream) {
        return { isFinal: -1, text: "", startMs: 0, endMs: 0 };
      }
      if (vad && typeof vad.acceptWaveform === "function") vad.acceptWaveform(pcm);
      stream.acceptWaveform(16000, pcm);
      let steps = 0;
      while (recognizer.isReady(stream) && steps < 8) {
        recognizer.decode(stream);
        steps += 1;
      }
      const result = recognizer.getResult(stream) || {};
      const text = typeof result.text === "string" ? result.text : "";
      const endpoint = Boolean(recognizer.isEndpoint(stream));
      if (endpoint && typeof recognizer.reset === "function") recognizer.reset(stream);
      fed += pcm.length;
      const endMs = Math.round((fed / 16000) * 1000);
      const startMs = Math.round(((fed - pcm.length) / 16000) * 1000);
      if (!text && !endpoint) return { isFinal: -1, text: "", startMs, endMs };
      return { isFinal: endpoint ? 1 : 0, text, startMs, endMs };
    },
    asrReset() {
      if (recognizer && stream && typeof recognizer.reset === "function") recognizer.reset(stream);
    },
    sumLoad(path) {
      if (!modelExists) return 0;
      const create = pendingLitert?.Engine?.create;
      if (typeof create !== "function") return -5;
      const model = downloadedSummary ?? path;
      return prepareWebGemmaRuntime(pendingLitert).then(() => create.call(pendingLitert.Engine, { model })).then(async (engine) => {
        liveEngine = engine;
        liveConversation = await engine.createConversation();
        return 0;
      });
    },
    sumUnload() {
      if (liveEngine && typeof liveEngine.delete === "function") liveEngine.delete();
      liveEngine = null;
      liveConversation = null;
    },
    sumGenerate(prompt) {
      if (!modelExists) return CANNED_SUMMARY;
      if (!liveConversation || typeof liveConversation.sendMessage !== "function") return -5;
      return liveConversation.sendMessage(prompt).then((response) => readGeneratedText(response));
    },
  };

  const core = createCore({
    mergeGapMs: options.mergeGapMs,
    summaryChunkChars: options.summaryChunkChars,
    displayLines: options.displayLines,
    summaryModelPath,
    webSummary: options.webSummary !== false,
    engines: custom || defaultEngines,
  });

  async function loadRecognizer(modelConfig = {}) {
    const dir = asrDirectory(options.asrModelPath, modelConfig);
    const files = requiredAsrFiles(dir);
    const flags = {
      encoder: await probeAsrFile(files.encoder),
      decoder: await probeAsrFile(files.decoder),
      joiner: await probeAsrFile(files.joiner),
      tokens: await probeAsrFile(files.tokens),
    };
    const asrPresent = presenceFromProbes(flags) === "present";
    const silero = joinPath(dir, "silero_vad.onnx");
    try {
      sherpaModule = await import("sherpa-onnx");
    } catch (error) {
      asrMode = asrPresent ? "failed" : "canned";
      if (asrPresent) throw error;
      return null;
    }
    if (typeof options.onRecognizer === "function") options.onRecognizer(sherpaModule, modelConfig);
    if (!asrPresent) {
      asrMode = "canned";
      return sherpaModule;
    }
    try {
      if (typeof sherpaModule.createOnlineRecognizer !== "function") {
        throw new Error("sherpa-onnx did not export createOnlineRecognizer");
      }
      const recognizerConfig = onlineRecognizerConfig(dir);
      recognizer = sherpaModule.createOnlineRecognizer(recognizerConfig);
      if (!recognizer || typeof recognizer.createStream !== "function" || recognizer.handle === 0) {
        throw new Error("sherpa-onnx did not create an online recognizer");
      }
      stream = recognizer.createStream();
      if (await probeAsrFile(silero)) {
        if (typeof sherpaModule.createVad !== "function") {
          throw new Error("sherpa-onnx did not export createVad");
        }
        vad = sherpaModule.createVad(vadConfig(silero));
      }
    } catch (error) {
      asrMode = "failed";
      recognizer = null;
      stream = null;
      vad = null;
      throw error;
    }
    asrMode = "live";
    return sherpaModule;
  }

  async function summarizeWithGemma(model) {
    const path = model || summaryModelPath;
    if (!isWebGemmaFile(path)) {
      throw new Error("web summary requires a web Gemma file such as gemma-4-E2B-it-web.litertlm");
    }
    let litert = null;
    try {
      litert = await import("@litert-lm/core");
    } catch (error) {
      if (downloadedSummary != null || await probeAsrFile(path)) throw error;
    }
    if (!custom) {
      modelExists = downloadedSummary != null || await probeAsrFile(path);
      pendingLitert = litert;
    }
    const summary = await Promise.resolve(core.summarize());
    return { litert, summary };
  }

  return {
    get gemmaResident() {
      return core.gemmaResident;
    },
    get displayText() {
      return core.displayText;
    },
    get fullTranscript() {
      return core.fullTranscript;
    },
    get lastError() {
      return core.lastError;
    },
    get recognizerState() {
      return asrMode;
    },
    pushPcm(samples, sampleRate) {
      const pcm = resampleStream(samples, sampleRate);
      if (!pcm.length) return 0;
      if (!custom && (asrMode === "failed" || asrMode === "unknown")) return asrMode === "failed" ? -5 : -3;
      return core.pushPcm(pcm, 16000);
    },
    pushAsrEvent(event) {
      return core.pushAsrEvent(event);
    },
    endSegment() {
      return core.endSegment();
    },
    summarize() {
      if (custom) return core.summarize();
      return summarizeWithGemma().then((result) => result.summary);
    },
    release() {
      core.release();
    },
    loadRecognizer,
    summarizeWithGemma,
    setSummaryModel(model) {
      downloadedSummary = model ?? null;
    },
  };
}

function prepareWebGemmaRuntime(litert) {
  if (!litert || typeof litert.getOrLoadGlobalLiteRtLm !== "function") return Promise.resolve();
  if (typeof document === "undefined" || typeof location === "undefined" || !location.href) return Promise.resolve();
  return litert.getOrLoadGlobalLiteRtLm(new URL("/vendor/@litert-lm/core/wasm/", location.href).href);
}
