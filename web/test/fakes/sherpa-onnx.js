function bag() {
  if (!globalThis.__apexoFake) {
    globalThis.__apexoFake = {
      accepts: [],
      vad: [],
      models: [],
      prompts: [],
      wasmUrls: [],
      deleted: 0,
      creates: 0,
      resets: 0,
      inputFinishedCount: 0,
      freedStreams: 0,
      noEndpoint: false,
      finishText: "尾词收尾完整",
    };
  }
  return globalThis.__apexoFake;
}

export function createOnlineRecognizer(config) {
  const model = config && config.modelConfig;
  if (!model || !("transducer" in model)) {
    throw new TypeError("Cannot use 'in' operator to search for 'transducer' in undefined");
  }
  const transducer = model.transducer;
  if (!transducer || !transducer.encoder || !transducer.decoder || !transducer.joiner || !model.tokens) {
    throw new TypeError("online recognizer config is missing transducer encoder, decoder, joiner, or tokens");
  }
  if (bag().throwOnCreate) throw new Error("sherpa-onnx failed to create the online recognizer");
  return new OnlineRecognizer(config);
}

export function createVad(config) {
  return new VoiceActivityDetector(config);
}

class OnlineRecognizer {
  constructor(config) {
    bag().recognizerConfig = config;
    bag().creates += 1;
  }

  createStream() {
    const stream = {
      ready: 0,
      finished: false,
      acceptWaveform(sampleRate, samples) {
        bag().accepts.push({ sampleRate, length: samples.length });
        stream.ready = 1;
      },
      inputFinished() {
        bag().inputFinishedCount = (bag().inputFinishedCount || 0) + 1;
        stream.finished = true;
        stream.ready = 1;
      },
      free() {
        bag().freedStreams = (bag().freedStreams || 0) + 1;
      },
    };
    return stream;
  }

  isReady(stream) {
    return stream.ready === 1;
  }

  decode(stream) {
    stream.ready = 0;
  }

  getResult(stream) {
    if (bag().noEndpoint) {
      return { text: stream && stream.finished ? bag().finishText || "尾词收尾完整" : "适配器草稿" };
    }
    return { text: "适配器转写" };
  }

  isEndpoint() {
    if (bag().noEndpoint) return false;
    return true;
  }

  reset() {
    bag().resets += 1;
  }
}

class VoiceActivityDetector {
  constructor(config) {
    bag().vadConfig = config;
  }

  acceptWaveform(samples) {
    bag().vad.push(samples.length);
  }
}
