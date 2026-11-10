import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const playwright = await import("playwright");
const { chromium } = playwright.default ?? playwright;

const demoRoot = fileURLToPath(new URL("../demo/", import.meta.url));
const port = 41931;
const origin = `http://127.0.0.1:${port}/`;

let failures = 0;

function expect(ok, name) {
  if (ok) console.log(`PASS ${name}`);
  else {
    console.log(`FAIL ${name}`);
    failures += 1;
  }
}

const child = spawn(process.execPath, ["server.js"], {
  cwd: demoRoot,
  env: { ...process.env, PORT: String(port) },
  stdio: ["ignore", "pipe", "pipe"],
});
let serverLog = "";
child.stdout.on("data", (chunk) => {
  serverLog += chunk;
});
child.stderr.on("data", (chunk) => {
  serverLog += chunk;
});

try {
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`demo server did not listen\n${serverLog}`)), 5000);
    const wait = () => {
      if (serverLog.includes("web demo")) {
        clearTimeout(timer);
        resolve();
      }
    };
    child.stdout.on("data", wait);
    child.stderr.on("data", wait);
    child.on("exit", (code) => {
      clearTimeout(timer);
      reject(new Error(`demo server exited ${code}\n${serverLog}`));
    });
    wait();
  });

  const browser = await chromium.launch({
    headless: true,
    args: [
      "--use-fake-ui-for-media-stream",
      "--use-fake-device-for-media-stream",
      "--autoplay-policy=no-user-gesture-required",
    ],
  });
  const page = await browser.newPage({ permissions: ["microphone"] });
  const errors = [];
  const gemmaUrls = [];
  page.on("pageerror", (error) => errors.push(`pageerror: ${error.message}`));
  page.on("console", (message) => {
    if (message.type() === "error") errors.push(`console: ${message.text()}`);
  });
  page.on("request", (request) => {
    if (request.url().includes(".litertlm")) gemmaUrls.push(request.url());
  });
  await page.addInitScript(() => {
    window.__buffers = [];
    const original = AudioContext.prototype.createBuffer;
    AudioContext.prototype.createBuffer = function createBuffer(channels, length, rate) {
      window.__buffers.push({ channels, length, rate });
      return original.call(this, channels, length, rate);
    };
    class FakeSpeech {
      start() {
        window.__speech = this;
      }
      stop() {
        this.onend?.();
      }
    }
    window.SpeechRecognition = FakeSpeech;
    window.webkitSpeechRecognition = FakeSpeech;
  });

  await page.goto(origin, { waitUntil: "load" });
  const response = await page.request.head(origin);
  expect(response.headers()["cross-origin-opener-policy"] === "same-origin" && response.headers()["cross-origin-embedder-policy"] === "require-corp", "demo is served with COOP and COEP");
  const labels = await page.locator("button").allInnerTexts();
  expect(labels.join(",") === "开始录音,结束,总结,回听刚才这一段,下载 Gemma 4 E2B", "start, end, summarize, replay, and the Gemma 4 E2B download are shown");
  expect(await page.locator("#download").isVisible() && (await page.locator("#gemma-file").innerText()).includes("gemma-4-E2B-it-gpu.litertlm"), "the Gemma 4 E2B download and file name are visible");
  expect((await page.locator("#gemma-progress").innerText()).includes("2 GB"), "the download explains that summary needs the weights");
  expect((await page.locator("#transcript").innerText()).trim() === "", "transcript stays empty before any speech");
  expect(!gemmaUrls.length, "summary model is not loaded on open");

  await page.click("#stop");
  expect((await page.locator("#status").innerText()) === "还没有开始录音", "end before recording does not invent a segment");
  await page.click("#listen");
  expect((await page.locator("#status").innerText()) === "还没有可回听的一段", "replay before recording does not play anything");
  await page.click("#summarize");
  expect((await page.locator("#status").innerText()) === "还没有可总结的文字", "summarize before speech does not run");
  expect((await page.locator("#transcript").innerText()).trim() === "" && gemmaUrls.length === 0, "empty page still has no transcript and no Gemma request");

  await page.click("#mic");
  await page.waitForFunction(() => document.querySelector("#status")?.innerText === "录音中");
  expect((await page.locator("#transcript").innerText()).trim() === "", "recording starts with no placeholder text");

  async function say(text, isFinal) {
    await page.evaluate(({ text, isFinal }) => {
      const row = [{ transcript: text }];
      row.isFinal = isFinal;
      if (!window.__speech || typeof window.__speech.onresult !== "function") {
        throw new Error("speech recognition is not receiving results");
      }
      window.__speech.onresult({ results: [row] });
    }, { text, isFinal });
  }

  await say("今天", false);
  const interim = await page.locator("#transcript").innerText();
  const interimStatus = await page.locator("#status").innerText();
  expect(interim === "今天" && interimStatus === "录音中", "interim text appears while recording is still running");
  await say("今天开会", false);
  expect((await page.locator("#transcript").innerText()) === "今天开会" && (await page.locator("#status").innerText()) === "录音中", "a later interim replaces the line before end");
  await page.waitForTimeout(400);
  await say("今天开会", true);
  expect((await page.locator("#transcript").innerText()) === "今天开会" && (await page.locator("#status").innerText()) === "录音中", "final text is on screen before end");

  await page.click("#stop");
  expect((await page.locator("#transcript").innerText()) === "今天开会", "end keeps the spoken line");
  expect((await page.locator("#status").innerText()) === "这一段已结束", "end closes the segment without summarizing");
  expect(gemmaUrls.length === 0, "ending a segment does not load Gemma");

  await page.click("#listen");
  await page.waitForFunction(() => window.__buffers.length >= 1);
  const firstReplay = await page.evaluate(() => window.__buffers[window.__buffers.length - 1]);
  expect((await page.locator("#status").innerText()) === "正在回放刚才这一段", "replay plays the segment that just ended");
  expect(firstReplay.channels === 1 && firstReplay.length > 1000, "replay contains the captured segment, not an empty buffer");
  await page.waitForTimeout(300);
  await page.click("#listen");
  const secondReplay = await page.evaluate(() => window.__buffers[window.__buffers.length - 1]);
  expect(secondReplay.length === firstReplay.length && secondReplay.rate === firstReplay.rate, "the ended segment does not keep growing during replay");

  await page.click("#summarize");
  await page.waitForFunction(() => (document.querySelector("#status")?.innerText || "").includes("网页版 Gemma"));
  expect((await page.locator("#transcript").innerText()) === "今天开会", "summarize keeps the spoken line when Gemma is absent");
  expect(!(await page.locator("#transcript").innerText()).includes("你好，这是已经成稿的一句"), "summarize does not replace speech with the placeholder sentence");
  expect(gemmaUrls.length > 0, "Gemma is requested only after summarize is clicked");

  await page.click("#mic");
  await page.waitForFunction(() => document.querySelector("#status")?.innerText === "录音中");
  expect((await page.locator("#transcript").innerText()).trim() === "", "a new recording clears the previous segment");
  await say("第二段", false);
  expect((await page.locator("#transcript").innerText()) === "第二段" && (await page.locator("#status").innerText()) === "录音中", "the new segment shows its own interim text");
  await page.click("#summarize");
  await page.waitForFunction(() => (document.querySelector("#status")?.innerText || "").includes("网页版 Gemma"));
  expect((await page.locator("#status").innerText()) !== "录音中", "summarize ends the recording");
  expect((await page.locator("#transcript").innerText()) === "第二段", "summarize from the recording button keeps the unfinished line");

  expect(errors.length === 0, `page stays free of errors${errors.length ? `: ${errors.join(" | ")}` : ""}`);
  await browser.close();
} finally {
  if (!child.killed) child.kill("SIGTERM");
}

if (failures !== 0) {
  console.log(`${failures} failure(s)`);
  process.exit(1);
}
console.log("all demo flow assertions passed");
