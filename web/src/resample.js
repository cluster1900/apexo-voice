export function createStreamingResampler() {
  let fromRate = 0;
  let pending = new Float32Array(0);
  let pendingStart = 0;
  let nextOut = 0;

  return function push(samples, rate) {
    if (!(samples instanceof Float32Array)) samples = Float32Array.from(samples ?? []);
    if (!samples.length) return new Float32Array(0);
    if (rate === 16000) return new Float32Array(samples);
    if (!rate || rate < 16000) {
      throw new Error("resample source rate must be at least 16000");
    }
    if (fromRate !== rate) {
      fromRate = rate;
      pending = new Float32Array(0);
      pendingStart = 0;
      nextOut = 0;
    }
    const ratio = rate / 16000;
    const merged = new Float32Array(pending.length + samples.length);
    merged.set(pending, 0);
    merged.set(samples, pending.length);
    pending = merged;
    const lastIndex = pendingStart + pending.length - 1;
    const out = [];
    while (nextOut * ratio + 1 <= lastIndex) {
      const srcPos = nextOut * ratio;
      const left = Math.floor(srcPos);
      const mix = srcPos - left;
      const base = left - pendingStart;
      out.push(pending[base] * (1 - mix) + pending[base + 1] * mix);
      nextOut += 1;
    }
    const keepFrom = Math.min(lastIndex, Math.floor(nextOut * ratio));
    const drop = keepFrom - pendingStart;
    if (drop > 0) {
      pending = new Float32Array(pending.subarray(drop));
      pendingStart += drop;
    }
    return Float32Array.from(out);
  };
}

export function resampleTo16k(samples, fromRate) {
  if (!(samples instanceof Float32Array)) {
    samples = Float32Array.from(samples);
  }
  if (fromRate === 16000) return samples;
  if (!fromRate || fromRate < 16000) {
    throw new Error("resample source rate must be at least 16000");
  }
  const ratio = fromRate / 16000;
  const outLength = Math.floor(samples.length / ratio);
  const out = new Float32Array(outLength);
  for (let i = 0; i < outLength; i += 1) {
    const position = i * ratio;
    const left = Math.floor(position);
    const right = Math.min(left + 1, samples.length - 1);
    const mix = position - left;
    out[i] = samples[left] * (1 - mix) + samples[right] * mix;
  }
  return out;
}
