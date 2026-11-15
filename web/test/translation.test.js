import { execFileSync, spawn } from "node:child_process";
import { createServer } from "node:http";
import { createReadStream, existsSync, mkdirSync, mkdtempSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { createSession as createCore, stripLabel } from "../src/session.js";
import {
  createSession,
  SUPPORTED_LANGUAGES,
  normalizeLang,
  getLanguagePackPlan,
  buildSummaryPrompts,
} from "../src/index.js";

const playwright = await import("playwright");
const { chromium } = playwright.default ?? playwright;

let failures = 0;

function expect(ok, name) {
  if (ok) console.log(`PASS ${name}`);
  else {
    console.log(`FAIL ${name}`);
    failures += 1;
  }
}

async function expectRejectCode(promise, expectedCode, name) {
  try {
    await promise;
    expect(false, name);
  } catch (err) {
    expect(err && err.code === expectedCode, name);
  }
}

async function waitUntil(predicate) {
  const deadline = Date.now() + 3000;
  while (!predicate()) {
    if (Date.now() > deadline) throw new Error("timed out waiting for translation");
    await new Promise((resolveWait) => setTimeout(resolveWait, 5));
  }
}

// 1. Language catalog, normalization, pack resolution (1-hop, 2-hop pivot, passthrough), and localized prompts.
{
  expect(SUPPORTED_LANGUAGES.length >= 8, "catalog exposes 8+ out-of-the-box languages");
  expect(
    normalizeLang("zh-CN") === "zh" &&
      normalizeLang("en_US") === "en" &&
      normalizeLang("ja-JP") === "ja" &&
      normalizeLang("ko-KR") === "ko" &&
      normalizeLang("fr-FR") === "fr" &&
      normalizeLang("de-DE") === "de" &&
      normalizeLang("es-ES") === "es" &&
      normalizeLang("ru-RU") === "ru",
    "normalizeLang normalizes BCP-47 and underscore locale tags",
  );

  const direct = getLanguagePackPlan("zh", "en");
  expect(
    direct.asrPack?.id === "asr-zh" &&
      direct.translationMode === "direct" &&
      direct.translationPacks.length === 1 &&
      direct.translationPacks[0].id === "trans-zh-en",
    "pack plan resolves zh -> en as 1-hop direct translation",
  );

  const pivot = getLanguagePackPlan("zh", "ja");
  expect(
    pivot.asrPack?.id === "asr-zh" &&
      pivot.translationMode === "pivot" &&
      pivot.translationPacks.map((p) => p.id).join(",") === "trans-zh-en,trans-en-ja",
    "pack plan resolves zh -> ja via 2-hop English pivot [trans-zh-en, trans-en-ja]",
  );

  const same = getLanguagePackPlan("fr", "fr");
  expect(
    same.asrPack?.id === "asr-fr" && same.translationMode === "passthrough" && same.translationPacks.length === 0,
    "pack plan resolves fr -> fr as passthrough with 0 translation packs",
  );

  const enPrompts = buildSummaryPrompts("en");
  const jaPrompts = buildSummaryPrompts("ja");
  expect(
    enPrompts.chunkInstruction.includes("Write strictly in English") &&
      jaPrompts.synthesisInstruction.includes("日本語") &&
      stripLabel("Summary: Ship on Wednesday.") === "Ship on Wednesday." &&
      stripLabel("要約：水曜日にリリース。") === "水曜日にリリース。" &&
      stripLabel("Zusammenfassung: Mittwoch Release.") === "Mittwoch Release.",
    "localized summary prompts and multilingual heading stripping work cleanly",
  );
}

// 2. Core session async real-time translation ordering, stage/full summary in B, and error contracts.
{
  const prompts = [];
  const transEvents = [];
  const session = createCore({
    summaryModelPath: "gemma-4-E2B-it-gpu.litertlm",
    webSummary: true,
    sourceLang: "zh",
    targetLang: "en",
    onTranslation: (ev) => transEvents.push(ev),
    engines: {
      async transTranslate(text) {
        // First utterance deliberately takes longer than second to verify FIFO ordering!
        const delay = text.includes("第一句") ? 40 : 5;
        await new Promise((r) => setTimeout(r, delay));
        return `EN(${text})`;
      },
      sumLoad() {
        return 0;
      },
      sumGenerate(prompt) {
        prompts.push(prompt);
        return "Summary: Final English summary.";
      },
    },
  });

  session.pushAsrEvent({ isFinal: 0, text: "草稿", startMs: 0, endMs: 200 });
  expect(transEvents.length === 0, "interim ASR event waits for the preview interval before translating");

  session.pushAsrEvent({ isFinal: 1, text: "第一句", startMs: 0, endMs: 600 });
  session.pushAsrEvent({ isFinal: 1, text: "第二句", startMs: 900, endMs: 1500 });
  await session.flushTranslations();

  expect(
    transEvents.length === 2 &&
      transEvents[0].text === "EN(第一句)" &&
      transEvents[1].text === "EN(第二句)" &&
      session.translatedDisplayText === "EN(第一句) EN(第二句)" &&
      session.fullTranslation === "EN(第一句)\nEN(第二句)",
    "async utterance translations resolve in strict spoken order and merge on translatedDisplayText",
  );

  const stage = await session.summarizeNew();
  expect(
    stage === "Final English summary." &&
      prompts.length === 1 &&
      prompts[0].includes("Write strictly in English") &&
      prompts[0].includes("EN(第一句)\nEN(第二句)"),
    "summarizeNew summarizes translated B-language utterances with B-language prompt",
  );

  session.pushAsrEvent({ isFinal: 0, text: "收尾句", startMs: 4000, endMs: 4500 });
  await session.endSegment(); // promotes interim fallback & awaits tail translation
  expect(
    session.fullTranslation === "EN(第一句)\nEN(第二句)\nEN(收尾句)",
    "endSegment awaits translation of the flushed tail utterance",
  );

  prompts.length = 0;
  const fullSum = await session.summarize();
  expect(
    fullSum === "Final English summary." &&
      prompts[0].includes("Material:\nEN(第一句)\nEN(第二句)\nEN(收尾句)"),
    "summarize synthesizes full B-language translation in target language B",
  );

  const standalone = await session.translate("独立测试");
  expect(
    standalone === "EN(独立测试)" && session.fullTranslation === "EN(第一句)\nEN(第二句)\nEN(收尾句)",
    "translate(text) translates standalone text without mutating session state",
  );

  session.reset();
  expect(
    session.translatedDisplayText === "" && session.fullTranslation === "",
    "reset clears translatedDisplayText and fullTranslation",
  );

  const noTrans = createCore({
    summaryModelPath: "gemma-4-E2B-it-gpu.litertlm",
    webSummary: true,
    sourceLang: "en",
    targetLang: "de",
    engines: {},
  });
  await expectRejectCode(noTrans.translate("hello"), -3, "missing translator rejects translate() with -3");
}

// Interval previews use the latest draft, stay outside the transcript/summary, and are replaced by the final.
{
  const calls = [];
  const events = [];
  const prompts = [];
  const session = createSession({
    sourceLang: "zh", targetLang: "en", translationIntervalMs: 25,
    onTranslation: (event) => events.push(event),
    engines: {
      async transTranslate(text) {
        calls.push(text);
        await new Promise((resolveWait) => setTimeout(resolveWait, 5));
        return `EN(${text})`;
      },
      sumLoad: () => 0,
      sumGenerate(prompt) { prompts.push(prompt); return "Summary: confirmed"; },
    },
  });
  session.pushAsrEvent({ isFinal: 0, text: "初始草稿", startMs: 0, endMs: 100 });
  session.pushAsrEvent({ isFinal: 0, text: "修正草稿", startMs: 0, endMs: 200 });
  expect(calls.length === 0, "preview interval throttles rapidly changing interim text");
  await waitUntil(() => session.translationPreviewText === "EN(修正草稿)" && !session.translationPending);
  expect(
    calls.join(",") === "修正草稿" && session.translatedDisplayText === "EN(修正草稿)" &&
      session.fullTranslation === "" && session.fullTranscript === "" && events[0]?.isFinal === 0,
    "preview translates the latest draft and emits isFinal=0 without committing either transcript",
  );
  session.pushAsrEvent({ isFinal: 0, text: "继续说话的草稿", startMs: 0, endMs: 300 });
  await waitUntil(() => session.translationPreviewText === "EN(继续说话的草稿)" && !session.translationPending);
  await new Promise((resolveWait) => setTimeout(resolveWait, 60));
  expect(
    calls.length === 2 && session.translatedDisplayText === "EN(继续说话的草稿)",
    "new previews replace the draft line and unchanged drafts are not translated repeatedly",
  );
  await session.summarizeNew();
  expect(prompts.length === 0, "stage summary ignores translated previews and does not promote unfinished speech");
  session.pushAsrEvent({ isFinal: 1, text: "完整定稿句", startMs: 0, endMs: 500 });
  await session.drain();
  await session.summarizeNew();
  expect(
    session.fullTranslation === "EN(完整定稿句)" && session.translationPreviewText === "" &&
      session.translatedDisplayText === "EN(完整定稿句)" && events.at(-1)?.isFinal === 1 &&
      prompts[0]?.includes("EN(完整定稿句)") && !prompts[0]?.includes("草稿"),
    "final replaces the preview exactly once and alone supplies the target-language summary",
  );
  await session.dispose();
}

// A slow preview never accumulates draft jobs, blocks ASR input, or overtakes queued finals.
{
  const calls = [];
  const releases = [];
  const events = [];
  const session = createCore({
    sourceLang: "zh", targetLang: "en", translationIntervalMs: 10,
    onTranslation: (event) => events.push(event),
    engines: {
      transTranslate(text) {
        calls.push(text);
        return new Promise((resolveTranslation) => releases.push(resolveTranslation));
      },
    },
  });
  session.pushAsrEvent({ isFinal: 0, text: "早期草稿", startMs: 0, endMs: 100 });
  await waitUntil(() => calls.length === 1);
  for (const text of ["更新草稿一", "更新草稿二", "最新草稿"]) {
    session.pushAsrEvent({ isFinal: 0, text, startMs: 0, endMs: 200 });
  }
  await new Promise((resolveWait) => setTimeout(resolveWait, 30));
  expect(
    calls.length === 1 && session.displayText === "最新草稿" && session.translationPending,
    "slow translation keeps only one active preview while ASR continues accepting updates",
  );
  session.pushAsrEvent({ isFinal: 1, text: "第一句定稿", startMs: 0, endMs: 300 });
  session.pushAsrEvent({ isFinal: 1, text: "第二句定稿", startMs: 400, endMs: 600 });
  releases[0]("STALE PREVIEW");
  await waitUntil(() => calls.length === 2);
  expect(events.length === 0 && session.translationPreviewText === "", "late preview is discarded as soon as the utterance is final");
  releases[1]("FIRST FINAL");
  await waitUntil(() => calls.length === 3);
  releases[2]("SECOND FINAL");
  await session.flushTranslations();
  expect(
    calls.join(",") === "早期草稿,第一句定稿,第二句定稿" &&
      session.fullTranslation === "FIRST FINAL\nSECOND FINAL" && events.every((event) => event.isFinal === 1),
    "finals have priority over draft updates and remain in spoken order behind an in-flight preview",
  );
  await session.dispose();
}

// Reset/language changes invalidate in-flight results; disposal cancels pending preview timers.
for (const operation of ["reset", "language"]) {
  const calls = [];
  const releases = [];
  const session = createCore({
    sourceLang: "zh", targetLang: "en", translationIntervalMs: 10,
    engines: {
      transTranslate(text) {
        calls.push(text);
        return new Promise((resolveTranslation) => releases.push(resolveTranslation));
      },
    },
  });
  session.pushAsrEvent({ isFinal: 1, text: "旧内容", startMs: 0, endMs: 100 });
  if (operation === "reset") session.reset();
  else session.setLanguages("zh", "de");
  session.pushAsrEvent({ isFinal: 1, text: "新内容", startMs: 200, endMs: 300 });
  releases[0]("OLD LANGUAGE OR RECORDING");
  await waitUntil(() => calls.length === 2);
  releases[1]("NEW TRANSLATION");
  await session.flushTranslations();
  expect(session.fullTranslation === "NEW TRANSLATION", `${operation} drops stale final translations from the previous configuration`);
  session.pushAsrEvent({ isFinal: 0, text: "未发送草稿", startMs: 400, endMs: 500 });
  await session.dispose();
  await new Promise((resolveWait) => setTimeout(resolveWait, 30));
  expect(calls.length === 2 && session.translationPreviewText === "", `${operation}: dispose cancels unsent previews`);
}

// Explicit finish commits the flushed tail, rather than archiving its earlier preview.
{
  const events = [];
  let finishes = 0;
  const session = createCore({
    sourceLang: "zh", targetLang: "en", translationIntervalMs: 10, displayLines: 1,
    onTranslation: (event) => events.push(event),
    engines: {
      transTranslate: (text) => `EN(${text})`,
      asrFinish: () => finishes++ === 0
        ? { isFinal: 1, text: "完整尾句", startMs: 200, endMs: 400 }
        : { isFinal: -1 },
    },
  });
  session.pushAsrEvent({ isFinal: 1, text: "前句", startMs: 0, endMs: 100 });
  session.pushAsrEvent({ isFinal: 0, text: "尾句草稿", startMs: 200, endMs: 300 });
  await waitUntil(() => session.translationPreviewText !== "");
  expect(session.translatedDisplayText === "EN(尾句草稿)", "preview respects a one-line translation display limit");
  await session.endSegment();
  await session.endSegment();
  expect(
    session.fullTranslation === "EN(前句)\nEN(完整尾句)" && session.translationPreviewText === "" &&
      events.filter((event) => event.isFinal === 1).length === 2,
    "endSegment replaces the preview with the flushed tail once and awaits its translation",
  );
  await session.dispose();
}

{
  let calls = 0;
  let updates = 0;
  const session = createSession({
    sourceLang: "zh", targetLang: "en", translationIntervalMs: 0,
    onChange: () => { updates += 1; },
    engines: { transTranslate() { calls += 1; return -3; } },
  });
  session.pushAsrEvent({ isFinal: 0, text: "草稿", startMs: 0, endMs: 100 });
  await new Promise((resolveWait) => setTimeout(resolveWait, 30));
  expect(calls === 0, "translationIntervalMs=0 retains final-only translation mode");
  session.pushAsrEvent({ isFinal: 1, text: "定稿", startMs: 0, endMs: 200 });
  session.pushAsrEvent({ isFinal: 0, text: "后续草稿", startMs: 300, endMs: 400 });
  expect(
    session.translationError.includes("missing") && updates > 0 && session.fullTranslation === "",
    "translation failure remains visible after subsequent ASR events and notifies the UI",
  );
  await session.dispose();
}

// 3. Real offline MarianMT ONNX translation in Node.js + Playwright Browser E2E flow.
const repoRoot = fileURLToPath(new URL("../../", import.meta.url));
const realTransDir = resolve(repoRoot, "native/tests/build/real-assets/trans-zh-en");
const hasRealTrans = existsSync(join(realTransDir, "encoder_model.onnx"));

if (hasRealTrans) {
  const sdkSession = createSession({
    sourceLang: "zh",
    targetLang: "en",
    translationModelPaths: [realTransDir],
  });
  await sdkSession.loadTranslator();
  expect(sdkSession.translatorState === "live", "Node SDK loads real MarianMT ONNX model via onnxruntime-web");
  const enText = await sdkSession.translate("今天下午三点开会，决定下周三发布新版本。");
  const lower = enText.toLowerCase();
  expect(
    enText.length > 0 && (lower.includes("meet") || lower.includes("wednesday") || lower.includes("release")),
    `Node SDK real zh -> en ONNX translation succeeds ("${enText}")`,
  );
  await sdkSession.dispose();
}

// 4. Playwright Browser E2E: Select languages -> Download packs -> Record A -> Real-time translate B in Worker!
if (hasRealTrans) {
  const workTmp = mkdtempSync(join(tmpdir(), "apexo-trans-e2e-"));
  const upstreamDir = join(workTmp, "upstream");
  const dataDir = join(workTmp, "demo-data");
  mkdirSync(upstreamDir, { recursive: true });
  mkdirSync(dataDir, { recursive: true });

  // Build a tiny valid .tar.bz2 ASR archive for asr-zh so /api/packs/download?kind=asr&id=asr-zh exercises full tar extraction!
  const fakeAsrSrc = join(workTmp, "fake-asr-archive/sherpa-zh");
  mkdirSync(fakeAsrSrc, { recursive: true });
  writeFileSync(join(fakeAsrSrc, "tokens.txt"), "a 1\n");
  writeFileSync(join(fakeAsrSrc, "encoder-epoch-99.int8.onnx"), "fake-enc");
  writeFileSync(join(fakeAsrSrc, "decoder-epoch-99.onnx"), "fake-dec");
  writeFileSync(join(fakeAsrSrc, "joiner-epoch-99.int8.onnx"), "fake-join");
  execFileSync("tar", ["-cjf", join(upstreamDir, "asr-zh.tar.bz2"), "-C", join(workTmp, "fake-asr-archive"), "sherpa-zh"]);

  const upstreamPort = 41951;
  const demoPort = 41952;
  const upstreamServer = createServer((req, res) => {
    const pathname = new URL(req.url || "/", "http://127.0.0.1").pathname;
    let filePath = null;
    if (pathname === "/asr-zh.tar.bz2") {
      filePath = join(upstreamDir, "asr-zh.tar.bz2");
    } else if (pathname.startsWith("/trans-zh-en/")) {
      const name = pathname.slice("/trans-zh-en/".length);
      filePath = join(realTransDir, name);
    }
    if (!filePath || !existsSync(filePath)) {
      res.writeHead(404);
      res.end("not found");
      return;
    }
    const st = statSync(filePath);
    res.writeHead(200, { "Content-Length": st.size, "Content-Type": "application/octet-stream" });
    createReadStream(filePath).pipe(res);
  });

  await new Promise((resolveListen) => upstreamServer.listen(upstreamPort, "127.0.0.1", resolveListen));

  const demoRoot = fileURLToPath(new URL("../demo/", import.meta.url));
  const demoChild = spawn(process.execPath, ["server.js"], {
    cwd: demoRoot,
    env: {
      ...process.env,
      PORT: String(demoPort),
      APEXO_DEMO_DATA_DIR: dataDir,
      APEXO_PACK_UPSTREAM_BASE: `http://127.0.0.1:${upstreamPort}`,
    },
    stdio: ["ignore", "pipe", "pipe"],
  });

  try {
    await new Promise((resolveReady, rejectReady) => {
      let out = "";
      const timer = setTimeout(() => rejectReady(new Error(`demo server timeout\n${out}`)), 6000);
      const onChunk = (c) => {
        out += c;
        if (out.includes("web demo")) {
          clearTimeout(timer);
          resolveReady();
        }
      };
      demoChild.stdout.on("data", onChunk);
      demoChild.stderr.on("data", onChunk);
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
    const pageErrors = [];
    const externalRequests = [];
    page.on("pageerror", (e) => pageErrors.push(e.message));
    page.on("request", (req) => {
      const u = req.url();
      if (!u.startsWith(`http://127.0.0.1:${demoPort}/`) && !u.startsWith("blob:") && !u.startsWith("data:")) {
        externalRequests.push(u);
      }
    });
    await page.addInitScript(() => {
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

    await page.goto(`http://127.0.0.1:${demoPort}/`, { waitUntil: "load" });

    // Step A: Select cross-language pair zh -> ja and verify 2-hop pivot pack display!
    await page.selectOption("#target-lang", "ja");
    await page.waitForFunction(() =>
      (document.querySelector("#pack-status")?.innerText || "").includes("English → 日本語"),
    );
    expect(
      (await page.locator("#download-packs").innerText()).includes("(3)"),
      "UI shows 3 packs needed for zh -> ja (ASR + 2-hop pivot translation)",
    );

    // Step B: Select zh -> en and click Download Packs!
    await page.selectOption("#target-lang", "en");
    await page.waitForFunction(() =>
      (document.querySelector("#download-packs")?.innerText || "").includes("(2)"),
    );
    await page.click("#download-packs");
    await page.waitForFunction(
      () => (document.querySelector("#download-packs")?.innerText || "") === "所需语言包已就绪",
      null,
      { timeout: 45000 },
    );
    expect(
      (await page.locator("#engine").innerText()).includes("离线翻译引擎已就绪 (zh → en)"),
      "downloading packs extracts ASR + MarianMT ONNX files and boots translation Worker in Chromium",
    );

    // Step C: Record Chinese speech (via opt-in SpeechRecognition since fake ASR onnx is intentionally dummy)
    // and verify real-time offline ONNX translation into #translation!
    await page.check("#use-browser-asr");
    await page.click("#mic");
    await page.waitForFunction(() => document.querySelector("#status")?.innerText === "录音中");
    expect(
      await page.evaluate(() => window.__speech?.lang === "zh-CN"),
      "SpeechRecognition language syncs with selected source language",
    );

    await page.evaluate(() => {
      const row = [{ transcript: "今天下午三点开会。" }];
      row.isFinal = false;
      window.__speech.onresult({ results: [row] });
    });
    await page.waitForFunction(
      () => (document.querySelector("#translation-status")?.innerText || "").includes("末行为翻译预览"),
      null,
      { timeout: 15000 },
    );
    expect(
      (await page.locator("#translation").innerText()).toLowerCase().includes("meet") &&
        (await page.locator("#status").innerText()) === "录音中",
      "continuous unfinished speech gets a real offline Worker translation preview before any final or summary",
    );

    await page.evaluate(() => {
      const row = [{ transcript: "今天下午三点开会，决定下周三发布新版本。" }];
      row.isFinal = true;
      window.__speech.onresult({ results: [row] });
    });

    await page.waitForFunction(
      () => (document.querySelector("#translation")?.innerText || "").toLowerCase().includes("wednesday") &&
        !(document.querySelector("#translation-status")?.innerText || "").includes("末行为翻译预览"),
      null,
      { timeout: 20000 },
    );
    const liveSrc = (await page.locator("#transcript").innerText()).trim();
    const liveTrans = (await page.locator("#translation").innerText()).trim();
    expect(
      liveSrc === "今天下午三点开会，决定下周三发布新版本。" &&
        (liveTrans.toLowerCase().includes("meet") ||
          liveTrans.toLowerCase().includes("wednesday") ||
          liveTrans.toLowerCase().includes("release")),
      `Browser Worker translates A -> B in real time ("${liveTrans}")`,
    );

    await page.click("#stop");
    await page.waitForFunction(() => document.querySelector("#status")?.innerText === "这一段已结束");

    expect(externalRequests.length === 0, "browser never fetches external CDN or remote URLs");
    expect(pageErrors.length === 0, `browser E2E stays free of page errors${pageErrors.length ? `: ${pageErrors.join(" | ")}` : ""}`);
    await browser.close();
  } finally {
    if (!demoChild.killed) demoChild.kill("SIGTERM");
    await new Promise((r) => upstreamServer.close(r));
    rmSync(workTmp, { recursive: true, force: true });
  }
}

if (failures !== 0) {
  console.log(`${failures} failure(s)`);
  process.exit(1);
}
console.log("all translation assertions passed");
