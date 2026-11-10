import { createServer } from "node:http";
import { Readable } from "node:stream";
import { createReadStream, existsSync, statSync } from "node:fs";
import { extname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL(".", import.meta.url));
const sdkRoot = fileURLToPath(new URL("../src/", import.meta.url));
const vendorRoot = fileURLToPath(new URL("../node_modules/", import.meta.url));
const gemmaWebFile = "gemma-4-E2B-it-gpu.litertlm";
const gemmaWebUrl = `https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/${gemmaWebFile}`;
const types = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".wasm": "application/wasm",
  ".map": "application/json",
  ".json": "application/json",
  ".wav": "audio/wav",
  ".onnx": "application/octet-stream",
  ".litertlm": "application/octet-stream",
};

function inside(base, relativePath) {
  const rootPath = resolve(base);
  const file = resolve(rootPath, relativePath);
  if (file !== rootPath && !file.startsWith(rootPath + sep)) return null;
  return file;
}

const sharedHeaders = {
  "Cross-Origin-Opener-Policy": "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
  "Cross-Origin-Resource-Policy": "same-origin",
};

async function sendGemmaDownload(response) {
  const localFile = join(root, gemmaWebFile);
  if (existsSync(localFile) && statSync(localFile).isFile()) {
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
  } else if (url.pathname.startsWith("/assets/apexo/litert/dist/")) {
    const rel = decodeURIComponent(url.pathname.slice("/assets/apexo/litert/dist/".length));
    file = inside(join(vendorRoot, "@litert-lm/core/dist"), rel);
  } else if (url.pathname.startsWith("/assets/apexo/litert/")) {
    const rel = decodeURIComponent(url.pathname.slice("/assets/apexo/litert/".length));
    file = inside(join(vendorRoot, "@litert-lm/core/wasm"), rel);
  } else if (url.pathname.startsWith("/assets/apexo/wasm-utils/dist/")) {
    const rel = decodeURIComponent(url.pathname.slice("/assets/apexo/wasm-utils/dist/".length));
    file = inside(join(vendorRoot, "@litertjs/wasm-utils/dist"), rel);
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
    file = inside(root, url.pathname.slice(1));
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
      response.on("close", () => stream.destroy());
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
