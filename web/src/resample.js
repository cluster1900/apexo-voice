// Down-sampling to 16 kHz with a low-pass filter, so audio above 8 kHz does not fold back into the speech band.
//
// Each output sample is a windowed-sinc weighted sum of the input around it. The sinc cuts off just under the
// 8 kHz Nyquist of the output; the window is Blackman. The filter reaches a few input samples ahead, so the
// streaming resampler answers about 0.5 ms (at 48 kHz) late, and `flush()` pushes out what is still waiting.

const TARGET_RATE = 16000;
const ZERO_CROSSINGS = 8; // on each side, measured at the output rate
const CUTOFF = 0.46; // of the input rate divided by the ratio; 0.5 would be exactly the output's Nyquist

function kernelFor(ratio) {
  return { halfWidth: Math.ceil(ZERO_CROSSINGS * ratio), cutoff: CUTOFF / ratio };
}

// Weighted sum of the input around source position `position`. `at(index)` returns the sample or 0.
function filteredSample(at, position, kernel) {
  const centre = Math.floor(position);
  const { halfWidth, cutoff } = kernel;
  let sum = 0;
  let norm = 0;
  for (let index = centre - halfWidth + 1; index <= centre + halfWidth; index += 1) {
    const t = index - position;
    const x = t / halfWidth;
    const window = 0.42 + 0.5 * Math.cos(Math.PI * x) + 0.08 * Math.cos(2 * Math.PI * x);
    const sinc = Math.abs(t) < 1e-9 ? 2 * cutoff : Math.sin(2 * Math.PI * cutoff * t) / (Math.PI * t);
    const weight = sinc * window;
    sum += weight * at(index);
    norm += weight;
  }
  return norm === 0 ? 0 : sum / norm;
}

// Returns push(samples, rate) -> Float32Array of 16 kHz samples. `push.flush()` returns the samples still waiting
// for input that will not come, and ends the stream. A rate of 16000 is passed through; a lower rate is refused.
export function createStreamingResampler() {
  let fromRate = 0;
  let kernel = null;
  let ratio = 1;
  let pending = new Float32Array(0);
  let pendingStart = 0; // input index of pending[0]
  let nextOut = 0;
  let received = 0; // real input samples seen, not counting flush padding

  function reset(rate) {
    fromRate = rate;
    ratio = rate / TARGET_RATE;
    kernel = kernelFor(ratio);
    pending = new Float32Array(0);
    pendingStart = 0;
    nextOut = 0;
    received = 0;
  }

  function append(samples) {
    const merged = new Float32Array(pending.length + samples.length);
    merged.set(pending, 0);
    merged.set(samples, pending.length);
    pending = merged;
  }

  // Emits every output whose filter support is inside the data (or, with `limit`, whose position is below it).
  function emit(limit) {
    const lastIndex = pendingStart + pending.length - 1;
    const at = (index) => {
      const local = index - pendingStart;
      return local >= 0 && local < pending.length ? pending[local] : 0;
    };
    const out = [];
    for (;;) {
      const position = nextOut * ratio;
      if (limit !== undefined ? position >= limit : Math.floor(position) + kernel.halfWidth > lastIndex) break;
      out.push(filteredSample(at, position, kernel));
      nextOut += 1;
    }
    // Keep only what later outputs can still reach.
    const keepFrom = Math.max(pendingStart, Math.floor(nextOut * ratio) - kernel.halfWidth + 1);
    const drop = Math.min(keepFrom - pendingStart, pending.length);
    if (drop > 0) {
      pending = pending.slice(drop);
      pendingStart += drop;
    }
    return Float32Array.from(out);
  }

  function push(samples, rate) {
    if (!(samples instanceof Float32Array)) samples = Float32Array.from(samples ?? []);
    if (!samples.length) return new Float32Array(0);
    if (rate === TARGET_RATE) return new Float32Array(samples);
    if (!rate || rate < TARGET_RATE) {
      throw new Error("resample source rate must be at least 16000");
    }
    if (fromRate !== rate) reset(rate);
    append(samples);
    received += samples.length;
    return emit();
  }

  push.flush = () => {
    if (!kernel || !received) return new Float32Array(0);
    append(new Float32Array(kernel.halfWidth + Math.ceil(ratio) + 1));
    const tail = emit(received);
    fromRate = 0;
    return tail;
  };

  return push;
}

export function resampleTo16k(samples, fromRate) {
  if (!(samples instanceof Float32Array)) {
    samples = Float32Array.from(samples);
  }
  if (fromRate === TARGET_RATE) return samples;
  if (!fromRate || fromRate < TARGET_RATE) {
    throw new Error("resample source rate must be at least 16000");
  }
  const resampler = createStreamingResampler();
  const head = resampler(samples, fromRate);
  const tail = resampler.flush();
  const out = new Float32Array(head.length + tail.length);
  out.set(head, 0);
  out.set(tail, head.length);
  return out;
}
