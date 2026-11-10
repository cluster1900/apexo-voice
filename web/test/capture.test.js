import { createSession, createStreamingResampler, resampleTo16k } from "../src/index.js";
import { mixChannels } from "../src/capture.js";

let failures = 0;

function expect(ok, name) {
  if (ok) console.log(`PASS ${name}`);
  else {
    console.log(`FAIL ${name}`);
    failures += 1;
  }
}

const length = 128;
const quiet = new Float32Array(length);
const voice = new Float32Array(length);
for (let i = 0; i < length; i += 1) voice[i] = Math.sin((2 * Math.PI * 4 * i) / length);
const mono = mixChannels([voice]);
const stereo = mixChannels([quiet, voice]);
let monoErr = 0;
let stereoErr = 0;
for (let i = 0; i < length; i += 1) {
  monoErr = Math.max(monoErr, Math.abs(mono[i] - voice[i]));
  stereoErr = Math.max(stereoErr, Math.abs(stereo[i] - voice[i] / 2));
}
expect(mono && monoErr === 0, "mono mic samples are copied unchanged");
expect(stereo && stereoErr < 1e-6, "voice on the second channel is mixed into the mono buffer");
expect(mixChannels([]) == null && mixChannels([new Float32Array(0)]) == null, "empty mic inputs are not a buffer of leftover samples");

const rate = 48000;
const total = rate;
const chunk = 128;
const input = new Float32Array(total);
for (let i = 0; i < total; i += 1) input[i] = Math.sin((2 * Math.PI * 440 * i) / rate);
const once = resampleTo16k(input, rate);
const wholeStream = createStreamingResampler();
const wholeHead = wholeStream(input, rate);
const wholeTail = wholeStream.flush();
const whole = new Float32Array(wholeHead.length + wholeTail.length);
whole.set(wholeHead, 0);
whole.set(wholeTail, wholeHead.length);
const parts = [];
const chunked = createStreamingResampler();
for (let offset = 0; offset < total; offset += chunk) {
  parts.push(chunked(input.subarray(offset, Math.min(total, offset + chunk)), rate));
}
parts.push(chunked.flush());
let count = 0;
for (const part of parts) count += part.length;
const merged = new Float32Array(count);
let cursor = 0;
for (const part of parts) {
  merged.set(part, cursor);
  cursor += part.length;
}
let boundaryErr = 0;
const shared = Math.min(once.length, merged.length, whole.length);
for (let i = 0; i < shared; i += 1) {
  boundaryErr = Math.max(boundaryErr, Math.abs(merged[i] - whole[i]), Math.abs(whole[i] - once[i]));
}
expect(merged.length === whole.length && merged.length === once.length, "worklet-sized chunks keep the full 16 kHz voice");
expect(boundaryErr === 0, "chunked resample matches one continuous 48 kHz voice buffer");

const seen = [];
const session = createSession({
  webSummary: true,
  summaryModelPath: "gemma-4-E2B-it-web.litertlm",
  engines: {
    asrPush(pcm) {
      seen.push(new Float32Array(pcm));
      return { isFinal: -1 };
    },
  },
});
for (let offset = 0; offset < total; offset += chunk) {
  session.pushPcm(input.subarray(offset, Math.min(total, offset + chunk)), rate);
}
let fed = 0;
for (const pcm of seen) fed += pcm.length;
expect(fed <= once.length && fed >= once.length - 16, "pushPcm feeds the recognizer the whole resampled voice (all but the last half millisecond)");

// Down-sampling must not fold audio above 8 kHz into the speech band, and must keep the speech band.
function rms(samples) {
  let sum = 0;
  for (const v of samples) sum += v * v;
  return Math.sqrt(sum / samples.length);
}
function tone(freq, seconds) {
  const out = new Float32Array(rate * seconds);
  for (let i = 0; i < out.length; i += 1) out[i] = Math.sin((2 * Math.PI * freq * i) / rate);
  return out;
}
const inBand = resampleTo16k(tone(1000, 1), rate);
const aboveNyquist = resampleTo16k(tone(12000, 1), rate); // a naive resampler folds this down to 4 kHz
const inBandLevel = rms(inBand.subarray(400, inBand.length - 400));
const aliasLevel = rms(aboveNyquist.subarray(400, aboveNyquist.length - 400));
expect(Math.abs(inBandLevel - Math.SQRT1_2) < 0.01, "a 1 kHz tone keeps its level after down-sampling");
expect(aliasLevel < 0.02, "a 12 kHz tone does not alias into the 16 kHz output");
const odd = resampleTo16k(Float32Array.from({ length: 44100 }, (_, i) => Math.sin((2 * Math.PI * 1000 * i) / 44100)), 44100);
expect(Math.abs(odd.length - 16000) <= 1, "a non-integer ratio (44.1 kHz) gives the right number of samples");

if (failures !== 0) {
  console.log(`${failures} failure(s)`);
  process.exit(1);
}
console.log("all capture assertions passed");
