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

const BREAK_CHARS = new Set(["\n", "。", "！", "？", ".", "!", "?"]);

// At most maxChars characters per chunk. A full chunk is cut back to its last line or sentence end when that
// keeps at least half of it, so sentences are not cut in half.
export function chunkText(text, maxChars) {
  const limit = maxChars > 0 ? maxChars : 1;
  const units = utf8Units(text);
  const parts = [];
  let start = 0;
  while (start < units.length) {
    let end = Math.min(start + limit, units.length);
    if (end < units.length) {
      for (let i = end - 1; i >= start + Math.ceil(limit / 2) - 1 && i >= start; i -= 1) {
        if (BREAK_CHARS.has(units[i])) {
          end = i + 1;
          break;
        }
      }
    }
    parts.push(units.slice(start, end).join(""));
    start = end;
  }
  return parts;
}

// A summary model is a LiteRT-LM bundle. Which build it is (CPU or WebGPU) is the host's choice and is not
// guessed from the file name.
export function isLiteRtLmFile(path) {
  const name = String(path).split(/[?#]/)[0].split(/[\\/]/).pop();
  return name.toLowerCase().endsWith(".litertlm");
}

// Joins two finals shown on one line. Non-CJK words get a space between them; CJK text is joined directly.
function joinText(line, next) {
  const a = line[line.length - 1];
  const b = next[0];
  const wordy = (c) => c !== undefined && c.charCodeAt(0) < 0x80 && !/\s/.test(c);
  return wordy(a) && wordy(b) ? `${line} ${next}` : line + next;
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

// Gemma often opens with its own heading ("摘要：", "转写内容摘要：", "**总结**:", "Summary:"). The session lays
// summaries out itself, so such a leading label is dropped. A label is at most a few words ending in 摘要/总结/Summary,
// followed by a colon or a line break, so "总结一下……" and ordinary sentences are kept.
const LEADING_LABEL = /^\s*[#* ]*[^\n:：，。,.]{0,8}?(?:摘要|总结|summary)[* ]*(?:[:：]|\n)\s*/i;
export function stripLabel(text) {
  return text.replace(LEADING_LABEL, "").replace(/^\s+/, "");
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
  let summary = "";
  let summarizedLength = 0;
  let epoch = 0;
  let error = "";
  let gemmaResident = false;
  let summarizing = false;

  function refresh() {
    // The unfinished line counts toward the line limit.
    const limit = interim && displayLines > 1 ? displayLines - 1 : displayLines;
    const start = limit > 0 && segments.length > limit ? segments.length - limit : 0;
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
    // A final with no words (for example an endpoint fired by silence) carries nothing to keep.
    if (!event.text) {
      refresh();
      return;
    }
    const last = segments[segments.length - 1];
    // A repeat is only dropped when the audio really overlaps the previous final (the recognizer reported the
    // same stretch of speech twice). Two utterances that merely sit close together are both kept, even when
    // they read the same.
    if (
      last &&
      !last.closed &&
      event.startMs >= last.startMs &&
      event.startMs < last.endMs &&
      last.text.endsWith(event.text)
    ) {
      refresh();
      return;
    }
    full = full ? `${full}\n${event.text}` : event.text;
    if (last && !last.closed && event.startMs >= last.startMs && event.startMs - last.endMs < mergeGapMs) {
      last.text = joinText(last.text, event.text);
      last.endMs = event.endMs;
      refresh();
      return;
    }
    segments.push({ startMs: event.startMs, endMs: event.endMs, text: event.text, closed: false });
    refresh();
  }

  // A line that never got its final is kept: it becomes final before the recognizer is reset.
  function endSegmentNow() {
    if (interim) {
      const text = interim;
      const startMs = interimStart;
      const endMs = interimEnd;
      interim = "";
      accept({ isFinal: 1, text, startMs, endMs });
    }
    if (segments.length) segments[segments.length - 1].closed = true;
    interim = "";
    refresh();
    if (typeof engines.asrReset === "function") engines.asrReset();
  }

  function modelPathOk() {
    if (webSummary && !modelPath) {
      error = "web summary needs a model name ending in .litertlm";
      return false;
    }
    if (modelPath && !isLiteRtLmFile(modelPath)) {
      error = "the summary model must be a .litertlm file";
      return false;
    }
    return true;
  }

  function runChunks(textToSummarize, blockPcm, onDone) {
    summarizing = true;
    if (blockPcm) gemmaResident = true;
    // Unloading can be asynchronous (a browser engine frees its weights in the background). Nothing after it,
    // including accepting PCM again, happens until it is done.
    const unloadThen = (next) => {
      let pending = null;
      try {
        if (typeof engines.sumUnload === "function") pending = engines.sumUnload();
      } catch {
        // Already gone.
      }
      const done = () => {
        if (blockPcm) gemmaResident = false;
        return next();
      };
      if (pending != null && typeof pending.then === "function") return pending.then(done, done);
      return done();
    };
    const fail = (message) =>
      unloadThen(() => {
        error = message;
        return -5;
      });
    const generateAll = () => {
      const work = chunkText(textToSummarize, summaryChunkChars);
      const pieces = [];
      const take = (index) => {
        if (index >= work.length) {
          return unloadThen(() => onDone(pieces.join("\n")));
        }
        let piece;
        try {
          piece = engines.sumGenerate(SUMMARY_INSTRUCTION + work[index]);
        } catch {
          return fail("gemma generation failed");
        }
        if (piece != null && typeof piece.then === "function") {
          return piece.then((text) => {
            if (typeof text !== "string") return fail("gemma generation failed");
            pieces.push(stripLabel(text));
            return take(index + 1);
          }).catch(() => fail("gemma generation failed"));
        }
        if (typeof piece !== "string") return fail("gemma generation failed");
        pieces.push(stripLabel(piece));
        return take(index + 1);
      };
      return take(0);
    };
    const loadFailed = (rc) => {
      if (rc === -3) {
        return unloadThen(() => {
          error = "summary model file was not found";
          return -3;
        });
      }
      return fail("gemma failed to load");
    };
    const run = () => {
      let loaded;
      try {
        loaded = engines.sumLoad(modelPath);
      } catch {
        return fail("gemma failed to load");
      }
      if (loaded != null && typeof loaded.then === "function") {
        return loaded.then((rc) => {
          if (rc !== 0) return loadFailed(rc);
          return generateAll();
        }).catch(() => fail("gemma failed to load"));
      }
      if (loaded !== 0) return loadFailed(loaded);
      return generateAll();
    };
    const result = run();
    if (result != null && typeof result.then === "function") {
      return result.then((value) => {
        summarizing = false;
        return value;
      });
    }
    summarizing = false;
    return result;
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
    get summaryText() {
      return summary;
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
      endSegmentNow();
      return 0;
    },
    summarize() {
      if (!modelPathOk()) return -4;
      if (typeof engines.sumLoad !== "function" || typeof engines.sumGenerate !== "function") {
        error = "summarizer is not attached";
        return -3;
      }
      if (summarizing) {
        error = "a summary is already running";
        return -5;
      }
      if (interim) {
        const text = interim;
        const startMs = interimStart;
        const endMs = interimEnd;
        interim = "";
        accept({ isFinal: 1, text, startMs, endMs });
      }
      summary = "";
      summarizedLength = 0;
      // Nothing was said: do not load the model just to have it write a summary of nothing.
      if (!full) return "";
      const targetLength = full.length;
      const startEpoch = epoch;
      return runChunks(full, true, (generated) => {
        if (epoch === startEpoch) {
          summary = generated;
          summarizedLength = targetLength;
        }
        return summary;
      });
    },
    summarizeNew() {
      if (!modelPathOk()) return -4;
      if (typeof engines.sumLoad !== "function" || typeof engines.sumGenerate !== "function") {
        error = "summarizer is not attached";
        return -3;
      }
      if (summarizing) {
        error = "a summary is already running";
        return -5;
      }
      // Speech still being recognized is included: it is closed as a line here and the recognizer starts over, so
      // its later results do not write the same words again.
      if (interim) endSegmentNow();
      const delta = full.slice(summarizedLength).replace(/^\n+/, "");
      const targetLength = full.length;
      const startEpoch = epoch;
      if (!delta) {
        summarizedLength = targetLength;
        return summary;
      }
      return runChunks(delta, false, (piece) => {
        if (epoch === startEpoch && full.length >= targetLength) {
          summary = summary && piece ? `${summary}\n${piece}` : summary + piece;
          summarizedLength = targetLength;
        }
        return summary;
      });
    },
    release() {
      // A running summary still uses the engine: it unloads it (and clears gemmaResident) when it finishes, and
      // the epoch check below keeps its result out of the cleared session.
      if (!summarizing) {
        if (gemmaResident && typeof engines.sumUnload === "function") engines.sumUnload();
        gemmaResident = false;
      }
      if (typeof engines.asrReset === "function") engines.asrReset();
      epoch += 1;
      segments.splice(0, segments.length);
      interim = "";
      full = "";
      display = "";
      summary = "";
      summarizedLength = 0;
    },
  };
}
