export const SAMPLE_RATE = 16000;
export const MERGE_GAP_MS = 2000;
export const SUMMARY_CHUNK_CHARS = 1500;
export const DISPLAY_LINES = 4;
export const DISPLAY_WORDS = 200;
export const SUMMARY_INSTRUCTION = "只根据下面的转写写摘要。转写里没有的内容不要补。\n转写：\n";

function utf8Units(text) {
  const units = [];
  for (const char of text) units.push(char);
  return units;
}

export function chunkText(text, maxChars) {
  const limit = maxChars > 0 ? maxChars : 1;
  const units = utf8Units(text);
  const parts = [];
  for (let i = 0; i < units.length; i += limit) {
    parts.push(units.slice(i, i + limit).join(""));
  }
  return parts;
}

export function isWebGemmaFile(path) {
  return String(path).includes("-web");
}

function isCjk(char) {
  const cp = char.codePointAt(0);
  return (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0x3400 && cp <= 0x4dbf);
}

function tokenize(text) {
  const tokens = [];
  let latin = "";
  const flush = () => {
    if (!latin) return;
    tokens.push({ text: latin, word: true });
    latin = "";
  };
  for (const char of text) {
    if (/\s/.test(char)) {
      flush();
      tokens.push({ text: char, word: false });
    } else if (isCjk(char)) {
      flush();
      tokens.push({ text: char, word: true });
    } else {
      latin += char;
    }
  }
  flush();
  return tokens;
}

function trimWords(text, maxWords) {
  const tokens = tokenize(text);
  const words = tokens.filter((token) => token.word).length;
  if (words <= maxWords) return text;
  let drop = words - maxWords;
  let skipping = true;
  let kept = "";
  for (const token of tokens) {
    if (skipping) {
      if (token.word) {
        drop -= 1;
        if (drop === 0) skipping = false;
      }
      continue;
    }
    kept += token.text;
  }
  return kept;
}

export function createSession(config) {
  const mergeGapMs = config.mergeGapMs > 0 ? config.mergeGapMs : MERGE_GAP_MS;
  const summaryChunkChars = config.summaryChunkChars > 0 ? config.summaryChunkChars : SUMMARY_CHUNK_CHARS;
  const displayLines = config.displayLines > 0 ? config.displayLines : DISPLAY_LINES;
  const displayWords = DISPLAY_WORDS;
  const modelPath = config.summaryModelPath || "";
  const webSummary = Boolean(config.webSummary);
  const engines = config.engines || {};
  const segments = [];
  let interim = "";
  let interimStart = 0;
  let interimEnd = 0;
  let full = "";
  let display = "";
  let error = "";
  let gemmaResident = false;

  function refresh() {
    const start = displayLines > 0 && segments.length > displayLines ? segments.length - displayLines : 0;
    const lines = segments.slice(start).map((segment) => segment.text);
    if (interim) lines.push(interim);
    display = trimWords(lines.join("\n"), displayWords);
  }

  function accept(event) {
    if (!event || event.isFinal < 0) return;
    if (event.isFinal === 0) {
      interim = event.text || "";
      interimStart = event.startMs || 0;
      interimEnd = event.endMs || interimStart;
      refresh();
      return;
    }
    interim = "";
    interimStart = 0;
    interimEnd = 0;
    const last = segments[segments.length - 1];
    if (last && event.text && last.text.endsWith(event.text)) {
      refresh();
      return;
    }
    full = full ? `${full}\n${event.text}` : event.text;
    const current = segments[segments.length - 1];
    if (current && event.startMs - current.endMs < mergeGapMs) {
      last.text += event.text;
      last.endMs = event.endMs;
      refresh();
      return;
    }
    segments.push({ startMs: event.startMs, endMs: event.endMs, text: event.text });
    refresh();
  }

  function modelPathOk() {
    const webFile = isWebGemmaFile(modelPath);
    if (webSummary) {
      if (!modelPath || !webFile) {
        error = "web summary requires a web Gemma file such as gemma-4-E2B-it-web.litertlm";
        return false;
      }
    } else if (webFile) {
      error = "native summary requires a non-web Gemma 4 E2B .litertlm";
      return false;
    }
    return true;
  }

  return {
    get gemmaResident() {
      return gemmaResident;
    },
    get displayText() {
      return display;
    },
    get fullTranscript() {
      return full;
    },
    get lastError() {
      return error;
    },
    pushPcm(pcm, sampleRate) {
      if (!pcm || sampleRate !== SAMPLE_RATE) {
        error = "pcm must be 16000 Hz mono float32";
        return -2;
      }
      if (gemmaResident) {
        error = "gemma is resident; live transcription does not run beside it";
        return -5;
      }
      if (typeof engines.asrPush !== "function") {
        error = "recognizer is not attached";
        return -3;
      }
      const event = engines.asrPush(pcm, sampleRate) || { isFinal: -1 };
      accept(event);
      return 0;
    },
    pushAsrEvent(event) {
      if (gemmaResident) return -5;
      accept(event);
      return 0;
    },
    endSegment() {
      if (interim) {
        const text = interim;
        const startMs = interimStart;
        const endMs = interimEnd;
        interim = "";
        accept({ isFinal: 1, text, startMs, endMs });
      }
      interim = "";
      refresh();
      if (typeof engines.asrReset === "function") engines.asrReset();
      return 0;
    },
    summarize() {
      if (!modelPathOk()) return -4;
      if (typeof engines.sumLoad !== "function" || typeof engines.sumGenerate !== "function") {
        error = "summarizer is not attached";
        return -3;
      }
      const unload = () => {
        if (typeof engines.sumUnload === "function") engines.sumUnload();
        gemmaResident = false;
      };
      const generateAll = () => {
        gemmaResident = true;
        const chunks = chunkText(full, summaryChunkChars);
        const work = chunks.length ? chunks : [""];
        const pieces = [];
        const take = (index) => {
          if (index >= work.length) {
            unload();
            return pieces.join("\n");
          }
          let piece;
          try {
            piece = engines.sumGenerate(SUMMARY_INSTRUCTION + work[index]);
          } catch {
            unload();
            error = "gemma generation failed";
            return -5;
          }
          if (piece != null && typeof piece.then === "function") {
            return piece.then((text) => {
              if (typeof text !== "string") {
                unload();
                error = "gemma generation failed";
                return -5;
              }
              pieces.push(text);
              return take(index + 1);
            }).catch(() => {
              unload();
              error = "gemma generation failed";
              return -5;
            });
          }
          if (typeof piece !== "string") {
            unload();
            error = "gemma generation failed";
            return -5;
          }
          pieces.push(piece);
          return take(index + 1);
        };
        return take(0);
      };
      let loaded;
      try {
        loaded = engines.sumLoad(modelPath);
      } catch {
        error = "gemma failed to load";
        return -5;
      }
      if (loaded != null && typeof loaded.then === "function") {
        return loaded.then((rc) => {
          if (rc !== 0) {
            error = "gemma failed to load";
            return -5;
          }
          return generateAll();
        }).catch(() => {
          error = "gemma failed to load";
          return -5;
        });
      }
      if (loaded !== 0) {
        error = "gemma failed to load";
        return -5;
      }
      return generateAll();
    },
    release() {
      if (gemmaResident && typeof engines.sumUnload === "function") engines.sumUnload();
      gemmaResident = false;
      if (typeof engines.asrReset === "function") engines.asrReset();
      segments.splice(0, segments.length);
      interim = "";
      full = "";
      display = "";
    },
  };
}
