import { createServer } from "node:http";
import { Readable } from "node:stream";
import { readFile } from "node:fs/promises";
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
  if (url.pathname.startsWith("/vendor/")) {
    let vendorPath;
    try {
      vendorPath = decodeURIComponent(url.pathname.slice("/vendor/".length));
    } catch {
      response.writeHead(400, sharedHeaders);
      response.end("bad request");
      return;
    }
    file = inside(vendorRoot, vendorPath);
  }
  else if (url.pathname.startsWith("/sdk/")) file = inside(sdkRoot, url.pathname.slice("/sdk/".length));
  else if (url.pathname === "/worklet.js") file = join(root, "worklet.js");
  else if (url.pathname !== "/") file = inside(root, url.pathname.slice(1));
  if (!file) {
    response.writeHead(404);
    response.end("not found");
    return;
  }
  const headers = sharedHeaders;
  try {
    const body = await readFile(file);
    response.writeHead(200, { ...headers, "Content-Type": types[extname(file)] || "application/octet-stream" });
    response.end(request.method === "HEAD" ? undefined : body);
  } catch {
    const probe = request.headers["x-apexo-probe"] === "1";
    response.writeHead(probe ? 204 : 404, headers);
    response.end(probe ? undefined : "not found");
  }
});

const port = Number(process.env.PORT || 4173);
server.listen(port, "127.0.0.1", () => {
  console.log(`web demo http://127.0.0.1:${port}`);
});
