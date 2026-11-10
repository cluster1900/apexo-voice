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
const whole = createStreamingResampler()(input, rate);
const parts = [];
const chunked = createStreamingResampler();
for (let offset = 0; offset < total; offset += chunk) {
  parts.push(chunked(input.subarray(offset, Math.min(total, offset + chunk)), rate));
}
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
expect(fed === once.length, "pushPcm feeds the recognizer the whole resampled voice");

if (failures !== 0) {
  console.log(`${failures} failure(s)`);
  process.exit(1);
}
console.log("all capture assertions passed");
