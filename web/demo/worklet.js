import { mixChannels } from "/sdk/capture.js";

class CaptureProcessor extends AudioWorkletProcessor {
  process(inputs) {
    const mixed = mixChannels(inputs[0]);
    if (!mixed) return true;
    this.port.postMessage(mixed, [mixed.buffer]);
    return true;
  }
}

registerProcessor("apexo-capture", CaptureProcessor);
