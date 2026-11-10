import { createAsrPipeline, onlineRecognizerConfig, requiredAsrFiles } from "../src/asr-pipeline.js";

let failures = 0;
function expect(ok, name) {
  if (ok) console.log(`PASS ${name}`);
  else {
    console.log(`FAIL ${name}`);
    failures += 1;
  }
}

// A recognizer that replays a script: each accepted chunk sets what getResult and isEndpoint report.
function scripted(steps) {
  let index = -1;
  let freed = 0;
  let resets = 0;
  let ready = false;
  const recognizer = {
    createStream() {
      return {
        acceptWaveform() {
          index += 1;
          ready = true;
        },
        free() {
          freed += 1;
        },
      };
    },
    isReady() {
      const was = ready;
      ready = false;
      return was;
    },
    decode() {},
    getResult() {
      return { text: steps[index].text };
    },
    isEndpoint() {
      return steps[index].endpoint === true;
    },
    reset() {
      resets += 1;
    },
  };
  return { recognizer, freed: () => freed, resets: () => resets };
}

const chunk = new Float32Array(1600); // 100 ms
const script = scripted([
  { text: "" },
  { text: "你" },
  { text: "你好" },
  { text: "你好", endpoint: true },
  { text: "", endpoint: true },
  { text: "世界", endpoint: true },
]);
const pipeline = createAsrPipeline(script.recognizer);
const events = [];
for (let i = 0; i < 6; i += 1) events.push(pipeline.push(chunk));

expect(events[0].isFinal === -1, "silence before any words is not an event");
expect(events[1].isFinal === 0 && events[1].text === "你" && events[1].startMs === 100, "the first words are an interim that starts at their chunk");
expect(events[2].isFinal === 0 && events[2].startMs === 100 && events[2].endMs === 300, "a growing interim keeps the start of the utterance");
expect(events[3].isFinal === 1 && events[3].text === "你好" && events[3].startMs === 100 && events[3].endMs === 400, "an endpoint with words is a final spanning the utterance");
expect(events[4].isFinal === -1, "an endpoint with no words (silence) is not an event");
expect(script.resets() === 2 + 1, "every endpoint resets the stream, including silent ones");
expect(events[5].isFinal === 1 && events[5].startMs === 500, "the next utterance starts at its own first chunk");

pipeline.reset();
expect(script.resets() === 4, "reset resets the stream");
pipeline.free();
expect(script.freed() === 1, "free releases the stream");

const config = onlineRecognizerConfig("/asr");
const files = requiredAsrFiles("/asr");
expect(config.modelConfig.transducer.encoder === files.encoder && config.modelConfig.tokens === "/asr/tokens.txt", "recognizer config is built from the model directory");

if (failures !== 0) {
  console.log(`${failures} failure(s)`);
  process.exit(1);
}
console.log("all asr pipeline assertions passed");
