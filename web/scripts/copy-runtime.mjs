#!/usr/bin/env node
// Copies the WebAssembly runtime assets required by @apexo/sdk in the browser from the
// currently installed sherpa-onnx and @litert-lm/core packages into a target directory:
//
//   node ./scripts/copy-runtime.mjs ./public/assets/apexo
//
// Produces:
//   <target>/sherpa/sherpa-onnx-wasm-nodejs.js
//   <target>/sherpa/sherpa-onnx-asr.js
//   <target>/sherpa/sherpa-onnx-wasm-nodejs.wasm
//   <target>/litert/<@litert-lm/core/wasm contents>

import { cpSync, existsSync, mkdirSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname, join, resolve } from "node:path";
import { pathToFileURL } from "node:url";

const SHERPA_FILES = [
  "sherpa-onnx-wasm-nodejs.js",
  "sherpa-onnx-asr.js",
  "sherpa-onnx-wasm-nodejs.wasm",
];

export function copyApexoRuntime(targetDir, { fromDir = process.cwd() } = {}) {
  if (!targetDir) {
    throw new Error("usage: copy-runtime.mjs <target-directory>");
  }
  const req = createRequire(join(resolve(fromDir), "package.json"));
  let sherpaPkgPath;
  let litertPkgPath;
  try {
    sherpaPkgPath = req.resolve("sherpa-onnx/package.json");
    litertPkgPath = req.resolve("@litert-lm/core/package.json");
  } catch {
    const localReq = createRequire(import.meta.url);
    sherpaPkgPath = localReq.resolve("sherpa-onnx/package.json");
    litertPkgPath = localReq.resolve("@litert-lm/core/package.json");
  }
  const sherpaRoot = dirname(sherpaPkgPath);
  const litertRoot = dirname(litertPkgPath);
  const litertWasmRoot = join(litertRoot, "wasm");
  if (!existsSync(litertWasmRoot)) {
    throw new Error(`@litert-lm/core wasm directory not found at ${litertWasmRoot}`);
  }

  const outRoot = resolve(targetDir);
  const sherpaOut = join(outRoot, "sherpa");
  const litertOut = join(outRoot, "litert");
  mkdirSync(sherpaOut, { recursive: true });
  mkdirSync(litertOut, { recursive: true });

  for (const name of SHERPA_FILES) {
    const src = join(sherpaRoot, name);
    if (!existsSync(src)) {
      throw new Error(`sherpa-onnx runtime file missing: ${src}`);
    }
    cpSync(src, join(sherpaOut, name));
  }
  cpSync(litertWasmRoot, litertOut, { recursive: true });
  const litertDist = join(litertRoot, "dist");
  if (existsSync(litertDist)) {
    cpSync(litertDist, join(litertOut, "dist"), { recursive: true });
  }
  try {
    const litertReq = createRequire(litertPkgPath);
    const wasmUtilsPkg = litertReq.resolve("@litertjs/wasm-utils/package.json");
    const wasmUtilsDist = join(dirname(wasmUtilsPkg), "dist");
    if (existsSync(wasmUtilsDist)) {
      const wasmUtilsOut = join(outRoot, "wasm-utils", "dist");
      mkdirSync(wasmUtilsOut, { recursive: true });
      cpSync(wasmUtilsDist, wasmUtilsOut, { recursive: true });
    }
  } catch {
    // Optional when a bundler resolves @litertjs/wasm-utils directly.
  }
  return { sherpaDir: sherpaOut, litertDir: litertOut };
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const target = process.argv[2];
  if (!target) {
    console.error("usage: copy-runtime.mjs <target-directory>");
    process.exit(2);
  }
  const { sherpaDir, litertDir } = copyApexoRuntime(target);
  console.log(`Copied sherpa-onnx runtime -> ${sherpaDir}`);
  console.log(`Copied @litert-lm/core wasm -> ${litertDir}`);
}
