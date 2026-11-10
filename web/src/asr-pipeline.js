// Shared by the in-process recognizer (Node) and the browser worker.

export function joinPath(dir, name) {
  if (!dir) return name;
  if (dir.endsWith("/") || dir.endsWith("\\")) return dir + name;
  return `${dir}/${name}`;
}

export function requiredAsrFiles(dir) {
  return {
    encoder: joinPath(dir, "encoder.onnx"),
    decoder: joinPath(dir, "decoder.onnx"),
    joiner: joinPath(dir, "joiner.onnx"),
    tokens: joinPath(dir, "tokens.txt"),
  };
}

export function onlineRecognizerConfig(dir) {
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

// Feeds 16 kHz PCM to an online recognizer and turns what it decodes into session events.
//
// Times describe the words, not the audio chunk: an utterance starts at the first chunk that had text. An
// endpoint with no words (silence) is not an event.
export function createAsrPipeline(recognizer) {
  const stream = recognizer.createStream();
  let fed = 0;
  let utteranceStart = -1;

  function resetStream() {
    if (typeof recognizer.reset === "function") recognizer.reset(stream);
    utteranceStart = -1;
  }

  return {
    push(pcm) {
      const startMs = Math.round((fed / 16000) * 1000);
      stream.acceptWaveform(16000, pcm);
      fed += pcm.length;
      const endMs = Math.round((fed / 16000) * 1000);
      // Decode everything that is ready, so latency does not pile up behind large pushes. The bound only guards
      // against a runaway loop.
      for (let steps = 0; steps < 1024 && recognizer.isReady(stream); steps += 1) recognizer.decode(stream);
      const result = recognizer.getResult(stream) || {};
      const text = typeof result.text === "string" ? result.text.trim() : "";
      const endpoint = Boolean(recognizer.isEndpoint(stream));
      const hasText = text.length > 0;
      if (hasText && utteranceStart < 0) utteranceStart = startMs;
      const event = { text, startMs: hasText ? utteranceStart : startMs, endMs, isFinal: -1 };
      if (endpoint) {
        event.isFinal = hasText ? 1 : -1;
        resetStream();
      } else {
        event.isFinal = hasText ? 0 : -1;
      }
      return event;
    },
    reset: resetStream,
    free() {
      try {
        if (stream && typeof stream.free === "function") stream.free();
      } catch {
        // Already freed.
      }
    },
  };
}
