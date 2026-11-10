export const SAMPLE_RATE = 16000;
export const MERGE_GAP_MS = 2000;
export const SUMMARY_CHUNK_CHARS = 1500;
export const DISPLAY_LINES = 4;
export const DISPLAY_WORDS = 200;
export const ERR_BUSY = -6;
export const SUMMARY_CHUNK_INSTRUCTION =
  "仅依据以下转写提取简洁事实摘要。\n" +
  "保留决定、待办、明确的负责人和时间，以及对先前决定的修改或撤销。\n" +
  "没有明确给出的信息不要补充；尚未确定的事项保持未确定。\n" +
  "按原文发生顺序概括，保留“取消”“改为”“最终决定”等关系。\n" +
  "转写：\n";
export const SUMMARY_SYNTHESIS_INSTRUCTION =
  "以下内容按会议发生顺序排列。请归纳成一份简洁的会议总结。\n" +
  "合并重复事项；对于明确被后续内容修改或撤销的决定，以最后明确决定为准。\n" +
  "保留仍未确定或互相矛盾且没有结论的事项，不自行猜测。\n" +
  "只写材料中有依据的要点、最终决定和待办，不补负责人或日期。\n" +
  "材料：\n";
export const SUMMARY_INSTRUCTION = SUMMARY_CHUNK_INSTRUCTION;

export function createApexoError(code, message) {
  const err = new Error(message);
  err.code = code;
  return err;
}

export function isBlankText(text) {
  return !text || /^\s*$/.test(text);
}

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

export function createSession(config = {}) {
  const mergeGapMs = config.mergeGapMs > 0 ? config.mergeGapMs : MERGE_GAP_MS;
  const summaryChunkChars = config.summaryChunkChars > 0 ? config.summaryChunkChars : SUMMARY_CHUNK_CHARS;
  const displayLines = config.displayLines > 0 ? config.displayLines : DISPLAY_LINES;
  const displayWords = DISPLAY_WORDS;
  const modelPath = config.summaryModelPath || "";
  const webSummary = Boolean(config.webSummary);
  const engines = config.engines || {};
  let transcriptListener = typeof config.onTranscript === "function" ? config.onTranscript : null;
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
  let closed = false;
  let activeSummaryTask = null;

  function refresh() {
    // The unfinished line counts toward the line limit.
    const limit = interim && displayLines > 1 ? displayLines - 1 : displayLines;
    const start = limit > 0 && segments.length > limit ? segments.length - limit : 0;
    const lines = segments.slice(start).map((segment) => segment.text);
    if (interim) lines.push(interim);
    display = trimWords(lines.join("\n"), displayWords);
  }

  function notifyTranscript(event) {
    if (typeof transcriptListener !== "function") return;
    transcriptListener({
      text: String(event.text || ""),
      isFinal: event.isFinal === 1 ? 1 : 0,
      startMs: event.startMs || 0,
      endMs: event.endMs || 0,
    });
  }

  function accept(event) {
    if (!event || event.isFinal < 0) return false;
    if (event.isFinal === 0) {
      interim = event.text || "";
      interimStart = event.startMs || 0;
      interimEnd = event.endMs || interimStart;
      refresh();
      notifyTranscript({ text: interim, isFinal: 0, startMs: interimStart, endMs: interimEnd });
      return true;
    }
    interim = "";
    interimStart = 0;
    interimEnd = 0;
    // A final with no words (for example an endpoint fired by silence) carries nothing to keep.
    if (!event.text) {
      refresh();
      return false;
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
      return false;
    }
    full = full ? `${full}\n${event.text}` : event.text;
    if (last && !last.closed && event.startMs >= last.startMs && event.startMs - last.endMs < mergeGapMs) {
      last.text = joinText(last.text, event.text);
      last.endMs = event.endMs;
      refresh();
      notifyTranscript({ text: event.text, isFinal: 1, startMs: event.startMs || 0, endMs: event.endMs || 0 });
      return true;
    }
    segments.push({ startMs: event.startMs, endMs: event.endMs, text: event.text, closed: false });
    refresh();
    notifyTranscript({ text: event.text, isFinal: 1, startMs: event.startMs || 0, endMs: event.endMs || 0 });
    return true;
  }

  function modelPathOk(pathToCheck = modelPath) {
    if (webSummary && !pathToCheck) {
      error = "web summary needs a model name ending in .litertlm";
      return false;
    }
    if (pathToCheck && !isLiteRtLmFile(pathToCheck)) {
      error = "the summary model must be a .litertlm file";
      return false;
    }
    return true;
  }

  async function unloadSummarizer() {
    try {
      if (typeof engines.sumUnload === "function") await engines.sumUnload();
    } catch {
      // Already gone.
    }
    gemmaResident = false;
  }

  async function generateOne(prompt) {
    let raw;
    try {
      raw = await engines.sumGenerate(prompt);
    } catch {
      await unloadSummarizer();
      error = "gemma generation failed";
      throw createApexoError(-5, error);
    }
    if (typeof raw !== "string") {
      await unloadSummarizer();
      const code = typeof raw === "number" && raw < 0 ? raw : -5;
      error = "gemma generation failed";
      throw createApexoError(code, error);
    }
    return stripLabel(raw);
  }

  async function runSummaryGeneration(textToSummarize, synthesize, effectiveModelPath = modelPath) {
    gemmaResident = true;
    let loadRc;
    try {
      loadRc = await engines.sumLoad(effectiveModelPath);
    } catch (err) {
      await unloadSummarizer();
      if (err && typeof err.code === "number") {
        error = err.message || (err.code === -3 ? "summary model file was not found" : "gemma failed to load");
        throw createApexoError(err.code, error);
      }
      error = "gemma failed to load";
      throw createApexoError(-5, error);
    }
    if (loadRc !== 0) {
      await unloadSummarizer();
      if (loadRc === -3) {
        error = "summary model file was not found";
        throw createApexoError(-3, error);
      }
      error = "gemma failed to load";
      throw createApexoError(-5, error);
    }

    const work = chunkText(textToSummarize, summaryChunkChars);
    if (!synthesize) {
      const pieces = [];
      for (const chunk of work) {
        const piece = await generateOne(SUMMARY_CHUNK_INSTRUCTION + chunk);
        if (piece) pieces.push(piece);
      }
      await unloadSummarizer();
      return pieces.join("\n");
    }

    if (work.length === 1) {
      const out = await generateOne(SUMMARY_SYNTHESIS_INSTRUCTION + work[0]);
      await unloadSummarizer();
      return out;
    }

    const chunkNotes = [];
    for (const chunk of work) {
      const note = await generateOne(SUMMARY_CHUNK_INSTRUCTION + chunk);
      if (note) chunkNotes.push(note);
    }
    let orderedNotes = chunkNotes.join("\n");

    while (chunkText(orderedNotes, summaryChunkChars).length > 1) {
      const prevChars = utf8Units(orderedNotes).length;
      const groups = chunkText(orderedNotes, summaryChunkChars);
      const reducedParts = [];
      for (const group of groups) {
        const note = await generateOne(SUMMARY_SYNTHESIS_INSTRUCTION + group);
        if (note) reducedParts.push(note);
      }
      const reduced = reducedParts.join("\n");
      const nextChars = utf8Units(reduced).length;
      if (nextChars >= prevChars) {
        await unloadSummarizer();
        error = "gemma summary reduction did not converge";
        throw createApexoError(-5, error);
      }
      orderedNotes = reduced;
    }

    const finalOut = await generateOne(SUMMARY_SYNTHESIS_INSTRUCTION + orderedNotes);
    await unloadSummarizer();
    return finalOut;
  }

  function executeWithBusyGuard(fn) {
    if (closed) {
      error = "session is closed";
      return Promise.reject(createApexoError(-1, error));
    }
    if (summarizing) {
      error = "a summary is already running";
      return Promise.reject(createApexoError(ERR_BUSY, error));
    }
    summarizing = true;
    const task = (async () => {
      try {
        return await fn();
      } finally {
        summarizing = false;
        if (activeSummaryTask === task) activeSummaryTask = null;
      }
    })();
    activeSummaryTask = task;
    return task;
  }

  function resetInternal() {
    if (!summarizing) {
      if (gemmaResident && typeof engines.sumUnload === "function") {
        try {
          engines.sumUnload();
        } catch {
          // Already gone.
        }
      }
      gemmaResident = false;
    }
    if (typeof engines.asrReset === "function") engines.asrReset();
    epoch += 1;
    segments.splice(0, segments.length);
    interim = "";
    interimStart = 0;
    interimEnd = 0;
    full = "";
    display = "";
    summary = "";
    summarizedLength = 0;
    error = "";
  }

  async function runSummarizeInternal({ text, incremental = false, overrideModel } = {}) {
    const chosenPath = overrideModel || modelPath;
    if (!modelPathOk(chosenPath)) {
      throw createApexoError(-4, error);
    }
    if (typeof engines.sumLoad !== "function" || typeof engines.sumGenerate !== "function") {
      error = "summarizer is not attached";
      throw createApexoError(-3, error);
    }
    if (text !== undefined) {
      if (typeof text !== "string") {
        error = "invalid summarize_text arguments";
        throw createApexoError(-1, error);
      }
      const inputCopy = String(text);
      const startEpoch = epoch;
      if (isBlankText(inputCopy)) {
        error = "";
        return "";
      }
      if (typeof engines.beforeSummarize === "function") {
        await engines.beforeSummarize(chosenPath);
      }
      const generated = await runSummaryGeneration(inputCopy, true, chosenPath);
      if (epoch !== startEpoch || closed) {
        error = "";
        return "";
      }
      error = "";
      return generated;
    }

    if (typeof engines.beforeSnapshot === "function") {
      await engines.beforeSnapshot();
    }

    if (incremental) {
      const currentFull = full;
      const delta = currentFull.slice(summarizedLength).replace(/^\n+/, "");
      const targetLength = currentFull.length;
      const startEpoch = epoch;
      if (isBlankText(delta)) {
        summarizedLength = targetLength;
        error = "";
        return summary;
      }
      if (typeof engines.beforeSummarize === "function") {
        await engines.beforeSummarize(chosenPath);
      }
      const piece = await runSummaryGeneration(delta, false, chosenPath);
      if (epoch === startEpoch && !closed && full.length >= targetLength) {
        summary = summary && piece ? `${summary}\n${piece}` : summary + piece;
        summarizedLength = targetLength;
        error = "";
        return summary;
      }
      error = "";
      return "";
    }

    const transcript = full;
    const targetLength = transcript.length;
    const startEpoch = epoch;
    // Nothing was said: do not load the model just to have it write a summary of nothing.
    if (isBlankText(transcript)) {
      summary = "";
      summarizedLength = 0;
      error = "";
      return "";
    }
    if (typeof engines.beforeSummarize === "function") {
      await engines.beforeSummarize(chosenPath);
    }
    const generated = await runSummaryGeneration(transcript, true, chosenPath);
    if (epoch === startEpoch && !closed) {
      summary = generated;
      summarizedLength = targetLength;
      error = "";
      return summary;
    }
    error = "";
    return "";
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
    setTranscriptListener(listener) {
      transcriptListener = typeof listener === "function" ? listener : null;
    },
    pushPcm(pcm, sampleRate) {
      if (closed) {
        error = "session is closed";
        return -1;
      }
      if (!pcm || sampleRate !== SAMPLE_RATE) {
        error = "pcm must be 16000 Hz mono float32";
        return -2;
      }
      if (typeof engines.asrPush !== "function") {
        error = "recognizer is not attached";
        return -3;
      }
      const event = engines.asrPush(pcm, sampleRate) || { isFinal: -1 };
      accept(event);
      error = "";
      return 0;
    },
    pushAsrEvent(event) {
      if (closed) {
        error = "session is closed";
        return -1;
      }
      accept(event);
      error = "";
      return 0;
    },
    async endSegment() {
      if (closed) {
        error = "session is closed";
        throw createApexoError(-1, error);
      }
      if (typeof engines.asrFinish === "function") {
        let res;
        try {
          res = await engines.asrFinish();
        } catch (err) {
          const code = typeof err?.code === "number" ? err.code : -5;
          error = err?.message || (code === -3 ? "recognizer model files are missing" : "recognizer failed to finish segment");
          throw createApexoError(code, error);
        }
        if (typeof res === "number" && res < 0) {
          error = res === -3 ? "recognizer model files are missing" : "recognizer failed to finish segment";
          throw createApexoError(res, error);
        }
        if (!res || !res.fallback) {
          if (res && typeof res === "object" && res.isFinal === 1 && res.text) {
            accept({ isFinal: 1, text: res.text, startMs: res.startMs || 0, endMs: res.endMs || 0 });
          } else {
            interim = "";
            interimStart = 0;
            interimEnd = 0;
            refresh();
          }
          if (segments.length) segments[segments.length - 1].closed = true;
          refresh();
          error = "";
          return;
        }
      }
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
      error = "";
    },
    summarize(text) {
      return executeWithBusyGuard(() => runSummarizeInternal({ text, incremental: false }));
    },
    summarizeNew() {
      return executeWithBusyGuard(() => runSummarizeInternal({ incremental: true }));
    },
    summarizeInternal(opts) {
      return executeWithBusyGuard(() => runSummarizeInternal(opts));
    },
    reset() {
      resetInternal();
    },
    release() {
      resetInternal();
    },
    async dispose() {
      closed = true;
      const running = activeSummaryTask;
      resetInternal();
      if (running) {
        try {
          await running;
        } catch {
          // Summary failed or was cancelled; it already unloaded its own engine.
        }
      }
    },
  };
}
