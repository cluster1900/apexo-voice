import { execFile } from "node:child_process";
import { createServer } from "node:http";
import { Readable } from "node:stream";
import {
  copyFileSync,
  createReadStream,
  createWriteStream,
  existsSync,
  mkdirSync,
  readdirSync,
  renameSync,
  rmSync,
  statSync,
} from "node:fs";
import { basename, extname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

import { ASR_PACKAGES, TRANSLATION_PACKAGES, getLanguagePackPlan } from "../src/languages.js";

const execFileAsync = promisify(execFile);

const root = fileURLToPath(new URL(".", import.meta.url));
const dataRoot = resolve(process.env.APEXO_DEMO_DATA_DIR || root);
const modelsRoot = join(dataRoot, "models");
const sdkRoot = fileURLToPath(new URL("../src/", import.meta.url));
const vendorRoot = fileURLToPath(new URL("../node_modules/", import.meta.url));
const gemmaWebFile = "gemma-4-E2B-it-gpu.litertlm";
const upstreamOverride = (process.env.APEXO_PACK_UPSTREAM_BASE || "").replace(/\/+$/, "");
const gemmaWebUrl = upstreamOverride
  ? `${upstreamOverride}/${gemmaWebFile}`
  : `https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/${gemmaWebFile}`;

const types = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".mjs": "text/javascript; charset=utf-8",
  ".wasm": "application/wasm",
  ".map": "application/json",
  ".json": "application/json",
  ".txt": "text/plain; charset=utf-8",
  ".wav": "audio/wav",
  ".spm": "application/octet-stream",
  ".onnx": "application/octet-stream",
  ".litertlm": "application/octet-stream",
};

function inside(base, relativePath) {
  const rootPath = resolve(base);
  const file = resolve(rootPath, relativePath);
  if (file !== rootPath && !file.startsWith(rootPath + sep)) return null;
  return file;
}

function isNonEmptyFile(filePath) {
  try {
    if (!existsSync(filePath)) return false;
    const st = statSync(filePath);
    return st.isFile() && st.size > 0;
  } catch {
    return false;
  }
}

function isAsrDirReady(dir) {
  return ["encoder.onnx", "decoder.onnx", "joiner.onnx", "tokens.txt"].every((name) =>
    isNonEmptyFile(join(dir, name)),
  );
}

function isTransDirReady(dir) {
  return ["config.json", "vocab.json", "source.spm", "encoder_model.onnx", "decoder_model.onnx"].every((name) =>
    isNonEmptyFile(join(dir, name)),
  );
}

const sharedHeaders = {
  "Cross-Origin-Opener-Policy": "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
  "Cross-Origin-Resource-Policy": "same-origin",
};

function sendJson(response, status, payload) {
  const body = JSON.stringify(payload);
  response.writeHead(status, {
    ...sharedHeaders,
    "Content-Type": "application/json; charset=utf-8",
    "Cache-Control": "no-store",
    "Content-Length": Buffer.byteLength(body),
  });
  response.end(body);
}

function buildPacksStatus(source, target) {
  const plan = getLanguagePackPlan(source, target);
  let asr = null;
  if (plan.asrPack) {
    const primaryDir = join(modelsRoot, plan.asrPack.id);
    const legacyZhDir = plan.asrPack.id === "asr-zh" ? join(dataRoot, "asr-model") : "";
    const inPrimary = isAsrDirReady(primaryDir);
    const inLegacy = !inPrimary && Boolean(legacyZhDir) && isAsrDirReady(legacyZhDir);
    asr = {
      id: plan.asrPack.id,
      lang: plan.asrPack.lang,
      title: plan.asrPack.title,
      sizeMB: plan.asrPack.sizeMB,
      ready: inPrimary || inLegacy,
      url: inLegacy ? "asr-model" : `/models/${plan.asrPack.id}`,
      sampleWavUrl: isNonEmptyFile(join(primaryDir, "test.wav"))
        ? `/models/${plan.asrPack.id}/test.wav`
        : plan.asrPack.id === "asr-zh" && isNonEmptyFile(join(dataRoot, "test.wav"))
          ? "/test.wav"
          : "",
    };
  }
  const translation = plan.translationPacks.map((pack) => {
    const dir = join(modelsRoot, pack.id);
    return {
      id: pack.id,
      from: pack.from,
      to: pack.to,
      title: pack.title,
      sizeMB: pack.sizeMB,
      ready: isTransDirReady(dir),
      url: `/models/${pack.id}`,
    };
  });
  return {
    sourceLang: plan.sourceLang,
    targetLang: plan.targetLang,
    asr,
    translationMode: plan.translationMode,
    translation,
    summary: {
      file: gemmaWebFile,
      ready: isNonEmptyFile(join(dataRoot, gemmaWebFile)),
    },
  };
}

function walkFiles(dir, out = []) {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    if (entry.name.startsWith(".")) continue;
    const full = join(dir, entry.name);
    if (entry.isDirectory()) walkFiles(full, out);
    else if (entry.isFile()) out.push(full);
  }
  return out;
}

async function streamUrlToFile(url, destPath, onProgress) {
  const upstream = await fetch(url);
  if (!upstream.ok || !upstream.body) {
    throw new Error(`HTTP ${upstream.status} fetching ${url}`);
  }
  const total = Number(upstream.headers.get("content-length")) || 0;
  let received = 0;
  const reader = upstream.body.getReader();
  const out = createWriteStream(destPath);
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      received += value.byteLength;
      if (!out.write(value)) {
        await new Promise((resolveDrain) => out.once("drain", resolveDrain));
      }
      if (onProgress) onProgress(received, total);
    }
    await new Promise((resolveFinish, rejectFinish) => {
      out.end(() => resolveFinish());
      out.once("error", rejectFinish);
    });
  } catch (err) {
    out.destroy();
    throw err;
  }
}

const activeDownloads = new Map();

async function downloadPackOnDisk(kind, id, emit) {
  mkdirSync(modelsRoot, { recursive: true });
  const targetDir = join(modelsRoot, id);
  const stagingDir = join(modelsRoot, `.staging-${id}-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`);
  mkdirSync(stagingDir, { recursive: true });

  try {
    if (kind === "trans") {
      const pack = TRANSLATION_PACKAGES[id];
      if (!pack) throw new Error(`unknown translation package: ${id}`);
      if (isTransDirReady(targetDir)) {
        emit({ stage: "done", id, done: true, percent: 100 });
        return;
      }
      const entries = Object.entries(pack.files);
      for (let idx = 0; idx < entries.length; idx += 1) {
        const [localName, remoteUrl] = entries[idx];
        const effectiveUrl = upstreamOverride ? `${upstreamOverride}/${id}/${localName}` : remoteUrl;
        let lastReport = 0;
        emit({
          stage: "downloading",
          id,
          file: localName,
          fileIndex: idx + 1,
          fileCount: entries.length,
          received: 0,
          total: 0,
        });
        await streamUrlToFile(effectiveUrl, join(stagingDir, localName), (received, total) => {
          const now = Date.now();
          if (now - lastReport >= 120 || (total > 0 && received >= total)) {
            lastReport = now;
            const fileFraction = total > 0 ? received / total : 0.5;
            const percent = Math.min(99, Math.floor(((idx + fileFraction) / entries.length) * 100));
            emit({
              stage: "downloading",
              id,
              file: localName,
              fileIndex: idx + 1,
              fileCount: entries.length,
              received,
              total,
              percent,
            });
          }
        });
      }
      if (!isTransDirReady(stagingDir)) {
        throw new Error(`downloaded translation pack ${id} is incomplete`);
      }
      rmSync(targetDir, { recursive: true, force: true });
      renameSync(stagingDir, targetDir);
      emit({ stage: "done", id, done: true, percent: 100 });
      return;
    }

    if (kind === "asr") {
      const pack = ASR_PACKAGES[id];
      if (!pack) throw new Error(`unknown ASR package: ${id}`);
      if (isAsrDirReady(targetDir)) {
        emit({ stage: "done", id, done: true, percent: 100 });
        return;
      }
      const archiveUrl = upstreamOverride ? `${upstreamOverride}/${id}.tar.bz2` : pack.archiveUrl;
      const archivePath = join(stagingDir, "archive.tar.bz2");
      const extractDir = join(stagingDir, "extract");
      mkdirSync(extractDir, { recursive: true });

      let lastReport = 0;
      emit({ stage: "downloading", id, file: `${id}.tar.bz2`, received: 0, total: 0, percent: 0 });
      await streamUrlToFile(archivePath.startsWith("/") ? archiveUrl : archiveUrl, archivePath, (received, total) => {
        const now = Date.now();
        if (now - lastReport >= 120 || (total > 0 && received >= total)) {
          lastReport = now;
          const percent = total > 0 ? Math.min(90, Math.floor((received / total) * 90)) : 0;
          emit({ stage: "downloading", id, file: `${id}.tar.bz2`, received, total, percent });
        }
      });

      emit({ stage: "extracting", id, percent: 92 });
      await execFileAsync("tar", ["-xjf", archivePath, "-C", extractDir]);
      const allFiles = walkFiles(extractDir);
      const byName = (pred) => allFiles.find((f) => pred(basename(f).toLowerCase(), f));

      const tokensSrc = byName((n) => n === "tokens.txt");
      const encoderSrc =
        byName((n) => n.includes("encoder") && n.endsWith(".int8.onnx")) ||
        byName((n) => n.includes("encoder") && n.endsWith(".onnx"));
      const decoderSrc =
        byName((n) => n.includes("decoder") && n.endsWith(".onnx") && !n.endsWith(".int8.onnx")) ||
        byName((n) => n.includes("decoder") && n.endsWith(".int8.onnx"));
      const joinerSrc =
        byName((n) => n.includes("joiner") && n.endsWith(".int8.onnx")) ||
        byName((n) => n.includes("joiner") && n.endsWith(".onnx"));
      const sampleWavSrc =
        allFiles.find((f) => f.endsWith("/test_wavs/0.wav") || f.endsWith("\\test_wavs\\0.wav")) ||
        byName((n) => n.endsWith(".wav"));

      if (!tokensSrc || !encoderSrc || !decoderSrc || !joinerSrc) {
        throw new Error(`archive for ${id} did not contain encoder/decoder/joiner/tokens`);
      }

      copyFileSync(tokensSrc, join(stagingDir, "tokens.txt"));
      copyFileSync(encoderSrc, join(stagingDir, "encoder.onnx"));
      copyFileSync(decoderSrc, join(stagingDir, "decoder.onnx"));
      copyFileSync(joinerSrc, join(stagingDir, "joiner.onnx"));
      if (sampleWavSrc) {
        copyFileSync(sampleWavSrc, join(stagingDir, "test.wav"));
      }
      rmSync(extractDir, { recursive: true, force: true });
      rmSync(archivePath, { force: true });

      if (!isAsrDirReady(stagingDir)) {
        throw new Error(`extracted ASR pack ${id} is incomplete`);
      }
      rmSync(targetDir, { recursive: true, force: true });
      renameSync(stagingDir, targetDir);
      emit({ stage: "done", id, done: true, percent: 100 });
      return;
    }

    throw new Error(`unsupported package kind: ${kind}`);
  } finally {
    rmSync(stagingDir, { recursive: true, force: true });
  }
}

async function handlePackDownload(url, response) {
  const kind = url.searchParams.get("kind") || "";
  const id = url.searchParams.get("id") || "";
  if ((kind !== "asr" && kind !== "trans") || !/^[a-z0-9-]+$/.test(id)) {
    sendJson(response, 400, { error: "invalid kind or id" });
    return;
  }
  response.writeHead(200, {
    ...sharedHeaders,
    "Content-Type": "application/x-ndjson; charset=utf-8",
    "Cache-Control": "no-cache",
  });
  const writeEvent = (obj) => {
    if (!response.writableEnded && !response.destroyed) {
      response.write(`${JSON.stringify(obj)}\n`);
    }
  };
  const key = `${kind}:${id}`;
  try {
    if (activeDownloads.has(key)) {
      await activeDownloads.get(key);
      writeEvent({ stage: "done", id, done: true, percent: 100 });
    } else {
      const listeners = new Set([writeEvent]);
      const broadcast = (ev) => {
        for (const fn of listeners) fn(ev);
      };
      const task = downloadPackOnDisk(kind, id, broadcast).finally(() => {
        activeDownloads.delete(key);
      });
      activeDownloads.set(key, task);
      await task;
    }
  } catch (err) {
    writeEvent({ stage: "error", id, error: String(err?.message || err || "download failed") });
  } finally {
    if (!response.writableEnded && !response.destroyed) {
      response.end();
    }
  }
}

async function sendGemmaDownload(response) {
  const localFile = join(dataRoot, gemmaWebFile);
  if (isNonEmptyFile(localFile)) {
    const size = statSync(localFile).size;
    response.writeHead(200, {
      ...sharedHeaders,
      "Content-Type": "application/octet-stream",
      "Content-Disposition": `attachment; filename="${gemmaWebFile}"`,
      "Content-Length": size,
    });
    const stream = createReadStream(localFile);
    stream.on("error", () => response.destroy());
    response.on("close", () => stream.destroy());
    stream.pipe(response);
    return;
  }
  const upstream = await fetch(gemmaWebUrl);
  if (!upstream.ok || !upstream.body) {
    response.writeHead(upstream.status || 502, sharedHeaders);
    response.end("Gemma 4 E2B download failed");
    return;
  }
  const length = upstream.headers.get("content-length");
  response.writeHead(200, {
    ...sharedHeaders,
    "Content-Type": "application/octet-stream",
    "Content-Disposition": `attachment; filename="${gemmaWebFile}"`,
    ...(length ? { "Content-Length": length } : {}),
  });
  const stream = Readable.fromWeb(upstream.body);
  stream.on("error", () => response.destroy());
  response.on("close", () => stream.destroy());
  stream.pipe(response);
}

const server = createServer(async (request, response) => {
  const url = new URL(request.url || "/", "http://127.0.0.1");
  if (url.pathname === "/api/packs/status") {
    const source = url.searchParams.get("source") || "zh";
    const target = url.searchParams.get("target") || "en";
    sendJson(response, 200, buildPacksStatus(source, target));
    return;
  }
  if (url.pathname === "/api/packs/download") {
    await handlePackDownload(url, response);
    return;
  }
  if (url.pathname === `/download/${gemmaWebFile}`) {
    try {
      await sendGemmaDownload(response);
    } catch {
      if (!response.headersSent) {
        response.writeHead(502, sharedHeaders);
        response.end("Gemma 4 E2B download failed");
      } else {
        response.destroy();
      }
    }
    return;
  }

  let file = join(root, "index.html");
  if (url.pathname.startsWith("/assets/apexo/sherpa/")) {
    const rel = decodeURIComponent(url.pathname.slice("/assets/apexo/sherpa/".length));
    file = inside(join(vendorRoot, "sherpa-onnx"), rel);
  } else if (url.pathname.startsWith("/assets/apexo/ort/")) {
    const rel = decodeURIComponent(url.pathname.slice("/assets/apexo/ort/".length));
    file = inside(join(vendorRoot, "onnxruntime-web/dist"), rel);
  } else if (url.pathname.startsWith("/assets/apexo/litert/dist/")) {
    const rel = decodeURIComponent(url.pathname.slice("/assets/apexo/litert/dist/".length));
    file = inside(join(vendorRoot, "@litert-lm/core/dist"), rel);
  } else if (url.pathname.startsWith("/assets/apexo/litert/")) {
    const rel = decodeURIComponent(url.pathname.slice("/assets/apexo/litert/".length));
    file = inside(join(vendorRoot, "@litert-lm/core/wasm"), rel);
  } else if (url.pathname.startsWith("/assets/apexo/wasm-utils/dist/")) {
    const rel = decodeURIComponent(url.pathname.slice("/assets/apexo/wasm-utils/dist/".length));
    file = inside(join(vendorRoot, "@litertjs/wasm-utils/dist"), rel);
  } else if (url.pathname.startsWith("/models/")) {
    const rel = decodeURIComponent(url.pathname.slice("/models/".length));
    file = inside(modelsRoot, rel);
    if ((!file || !existsSync(file)) && rel.startsWith("asr-zh/")) {
      file = inside(join(dataRoot, "asr-model"), rel.slice("asr-zh/".length));
    }
  } else if (url.pathname.startsWith("/vendor/")) {
    let vendorPath;
    try {
      vendorPath = decodeURIComponent(url.pathname.slice("/vendor/".length));
    } catch {
      response.writeHead(400, sharedHeaders);
      response.end("bad request");
      return;
    }
    file = inside(vendorRoot, vendorPath);
  } else if (url.pathname.startsWith("/sdk/")) {
    file = inside(sdkRoot, url.pathname.slice("/sdk/".length));
  } else if (url.pathname === "/worklet.js") {
    file = join(root, "worklet.js");
  } else if (url.pathname !== "/") {
    const rel = decodeURIComponent(url.pathname.slice(1));
    const candidateInData = inside(dataRoot, rel);
    if (candidateInData && existsSync(candidateInData)) {
      file = candidateInData;
    } else if (dataRoot === root) {
      file = inside(root, rel);
    } else {
      // When APEXO_DEMO_DATA_DIR is explicitly set, model probes stay isolated inside dataRoot.
      file = candidateInData;
    }
  }

  if (!file || !existsSync(file)) {
    response.writeHead(404, sharedHeaders);
    response.end("not found");
    return;
  }

  let stats;
  try {
    stats = statSync(file);
    if (stats.isDirectory()) {
      response.writeHead(404, sharedHeaders);
      response.end("not found");
      return;
    }
  } catch {
    response.writeHead(404, sharedHeaders);
    response.end("not found");
    return;
  }

  const size = stats.size;
  const mime = types[extname(file)] || "application/octet-stream";
  const rangeHeader = request.headers.range;
  const rangeMatch = rangeHeader ? rangeHeader.match(/^bytes=(\d+)-(\d*)$/) : null;

  if (rangeMatch) {
    const start = Number(rangeMatch[1]);
    const end = rangeMatch[2] ? Math.min(size - 1, Number(rangeMatch[2])) : size - 1;
    if (start >= size || end < start) {
      response.writeHead(416, { ...sharedHeaders, "Content-Range": `bytes */${size}` });
      response.end();
      return;
    }
    response.writeHead(206, {
      ...sharedHeaders,
      "Content-Type": mime,
      "Content-Length": end - start + 1,
      "Content-Range": `bytes ${start}-${end}/${size}`,
      "Accept-Ranges": "bytes",
    });
    if (request.method === "HEAD") {
      response.end();
    } else {
      const stream = createReadStream(file, { start, end });
      stream.on("error", () => response.destroy());
      stream.on("close", () => stream.destroy());
      stream.pipe(response);
    }
  } else {
    response.writeHead(200, {
      ...sharedHeaders,
      "Content-Type": mime,
      "Content-Length": size,
      "Accept-Ranges": "bytes",
    });
    if (request.method === "HEAD") {
      response.end();
    } else {
      const stream = createReadStream(file);
      stream.on("error", () => response.destroy());
      response.on("close", () => stream.destroy());
      stream.pipe(response);
    }
  }
});

const port = Number(process.env.PORT || 4173);
server.listen(port, "127.0.0.1", () => {
  console.log(`web demo http://127.0.0.1:${port}`);
});
