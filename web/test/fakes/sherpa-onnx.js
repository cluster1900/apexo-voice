function bag() {
  if (!globalThis.__apexoFake) {
    globalThis.__apexoFake = { accepts: [], vad: [], models: [], prompts: [], deleted: 0, creates: 0, resets: 0 };
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
      acceptWaveform(sampleRate, samples) {
        bag().accepts.push({ sampleRate, length: samples.length });
        stream.ready = 1;
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

  getResult() {
    return { text: "适配器转写" };
  }

  isEndpoint() {
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
