// Offline MarianMT (OPUS-MT) ONNX translation pipeline for Node and Browser Workers.
// Parses binary SentencePiece Unigram model (source.spm) + vocab.json + config.json,
// and executes encoder + KV-cached (or non-cached) autoregressive decoder via onnxruntime-web.

import { joinPath } from "./asr-pipeline.js";

export function requiredTranslationFiles(dir) {
  return {
    config: joinPath(dir, "config.json"),
    vocab: joinPath(dir, "vocab.json"),
    sourceSpm: joinPath(dir, "source.spm"),
    encoder: joinPath(dir, "encoder_model.onnx"),
    decoder: joinPath(dir, "decoder_model.onnx"),
  };
}

export function translationPresenceFromProbes(flags) {
  const keys = ["config", "vocab", "sourceSpm", "encoder", "decoder"];
  return keys.every((k) => flags?.[k] === true) ? "present" : "absent";
}

function readVarint(bytes, offset, end) {
  let result = 0n;
  let shift = 0n;
  let pos = offset;
  while (pos < end && shift < 64n) {
    const b = bytes[pos++];
    result |= BigInt(b & 0x7f) << shift;
    if ((b & 0x80) === 0) return { ok: true, val: Number(result), pos };
    shift += 7n;
  }
  return { ok: false, val: 0, pos };
}

function skipWire(bytes, offset, end, wireType) {
  if (wireType === 0) {
    const v = readVarint(bytes, offset, end);
    return v.ok ? v.pos : -1;
  }
  if (wireType === 1) return offset + 8 <= end ? offset + 8 : -1;
  if (wireType === 2) {
    const v = readVarint(bytes, offset, end);
    if (!v.ok || offset + v.val < 0 || v.pos + v.val > end) return -1;
    return v.pos + v.val;
  }
  if (wireType === 5) return offset + 4 <= end ? offset + 4 : -1;
  return -1;
}

// Parses SentencePiece ModelProto field 1 (repeated SentencePiece { string piece=1; float score=2; Type type=3; }).
export function parseSentencePieceSpm(rawBytes) {
  const bytes = rawBytes instanceof Uint8Array ? rawBytes : new Uint8Array(rawBytes);
  const decoder = new TextDecoder("utf-8");
  const pieces = [];
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let pos = 0;
  const end = bytes.length;
  while (pos < end) {
    const tagRes = readVarint(bytes, pos, end);
    if (!tagRes.ok) break;
    pos = tagRes.pos;
    const fieldNum = tagRes.val >>> 3;
    const wireType = tagRes.val & 0x07;
    if (fieldNum === 1 && wireType === 2) {
      const lenRes = readVarint(bytes, pos, end);
      if (!lenRes.ok || lenRes.pos + lenRes.val > end) break;
      pos = lenRes.pos;
      const msgEnd = pos + lenRes.val;
      let sub = pos;
      let piece = "";
      let score = 0;
      let type = 1;
      while (sub < msgEnd) {
        const subTag = readVarint(bytes, sub, msgEnd);
        if (!subTag.ok) break;
        sub = subTag.pos;
        const sf = subTag.val >>> 3;
        const sw = subTag.val & 0x07;
        if (sf === 1 && sw === 2) {
          const sl = readVarint(bytes, sub, msgEnd);
          if (!sl.ok || sl.pos + sl.val > msgEnd) break;
          piece = decoder.decode(bytes.subarray(sl.pos, sl.pos + sl.val));
          sub = sl.pos + sl.val;
        } else if (sf === 2 && sw === 5) {
          if (sub + 4 > msgEnd) break;
          score = view.getFloat32(sub, true);
          sub += 4;
        } else if (sf === 3 && sw === 0) {
          const tv = readVarint(bytes, sub, msgEnd);
          if (!tv.ok) break;
          type = tv.val;
          sub = tv.pos;
        } else {
          const next = skipWire(bytes, sub, msgEnd, sw);
          if (next < 0) break;
          sub = next;
        }
      }
      pieces.push({ piece, score, type });
      pos = msgEnd;
    } else {
      const next = skipWire(bytes, pos, end, wireType);
      if (next < 0) break;
      pos = next;
    }
  }
  return pieces;
}

export function buildMarianTokenizerData(config, vocab, spmBytes, targetPrefix = "") {
  let maxId = 0;
  for (const id of Object.values(vocab)) {
    if (typeof id === "number" && id > maxId) maxId = id;
  }
  const vocabSize = Math.max(Number(config?.vocab_size) || 0, maxId + 1);
  const idToPiece = new Array(vocabSize).fill("");
  for (const [piece, id] of Object.entries(vocab)) {
    if (typeof id === "number" && id >= 0 && id < vocabSize) {
      idToPiece[id] = piece;
    }
  }
  const spmPieces = parseSentencePieceSpm(spmBytes);
  const pieceMap = new Map();
  let maxPieceBytes = 1;
  const encoder = new TextEncoder();
  for (const sp of spmPieces) {
    if (!sp.piece || sp.type === 2 || sp.type === 3 || sp.type === 5) continue;
    const id = vocab[sp.piece];
    if (typeof id !== "number") continue;
    pieceMap.set(sp.piece, { id, score: sp.score });
    const blen = encoder.encode(sp.piece).length;
    if (blen > maxPieceBytes) maxPieceBytes = blen;
  }
  if (pieceMap.size === 0) {
    throw new Error("MarianMT source.spm contained no valid vocabulary pieces");
  }
  let prefixTokenId = -1;
  if (targetPrefix && typeof vocab[targetPrefix] === "number") {
    prefixTokenId = vocab[targetPrefix];
  }
  return {
    eosTokenId: typeof config?.eos_token_id === "number" ? config.eos_token_id : 0,
    padTokenId: typeof config?.pad_token_id === "number" ? config.pad_token_id : 65000,
    decoderStartTokenId:
      typeof config?.decoder_start_token_id === "number"
        ? config.decoder_start_token_id
        : typeof config?.pad_token_id === "number"
          ? config.pad_token_id
          : 65000,
    unkTokenId: typeof vocab["<unk>"] === "number" ? vocab["<unk>"] : 1,
    maxLength: typeof config?.max_length === "number" && config.max_length > 8 ? config.max_length : 512,
    decoderLayers: typeof config?.decoder_layers === "number" ? config.decoder_layers : 6,
    decoderHeads: typeof config?.decoder_attention_heads === "number" ? config.decoder_attention_heads : 8,
    dModel: typeof config?.d_model === "number" ? config.d_model : 512,
    prefixTokenId,
    vocab,
    idToPiece,
    pieceMap,
    maxPieceChars: Math.max(32, maxPieceBytes),
  };
}

export function encodeMarianText(tok, rawText) {
  const collapsed = String(rawText || "")
    .normalize("NFKC")
    .replace(/\s+/g, " ")
    .trim();
  const ids = [];
  if (tok.prefixTokenId >= 0) ids.push(tok.prefixTokenId);
  if (!collapsed) {
    ids.push(tok.eosTokenId);
    return ids;
  }
  const norm = `\u2581${collapsed.replace(/ /g, "\u2581")}`;
  const chars = Array.from(norm);
  const n = chars.length;
  const bestScore = new Float64Array(n + 1).fill(-1e30);
  const bestPrev = new Int32Array(n + 1).fill(-1);
  const bestToken = new Int32Array(n + 1).fill(tok.unkTokenId);
  bestScore[0] = 0;
  const utf8Encoder = new TextEncoder();

  for (let i = 0; i < n; i += 1) {
    if (bestScore[i] <= -1e29) continue;
    const maxEnd = Math.min(n, i + tok.maxPieceChars);
    let matched = false;
    let sub = "";
    for (let j = i; j < maxEnd; j += 1) {
      sub += chars[j];
      const hit = tok.pieceMap.get(sub);
      if (hit !== undefined) {
        matched = true;
        const cand = bestScore[i] + hit.score;
        if (cand > bestScore[j + 1]) {
          bestScore[j + 1] = cand;
          bestPrev[j + 1] = i;
          bestToken[j + 1] = hit.id;
        }
      }
    }
    if (!matched) {
      const unkScore = bestScore[i] - 20.0;
      if (unkScore > bestScore[i + 1]) {
        bestScore[i + 1] = unkScore;
        bestPrev[i + 1] = i;
        bestToken[i + 1] = tok.unkTokenId;
      }
    }
  }

  const rev = [];
  let cur = n;
  while (cur > 0) {
    const prev = bestPrev[cur];
    if (prev < 0) break;
    const tid = bestToken[cur];
    if (tid === tok.unkTokenId) {
      const rawSpan = chars.slice(prev, cur).join("");
      const rawBytes = utf8Encoder.encode(rawSpan);
      let allBytesFound = rawBytes.length > 0;
      const byteIds = [];
      for (const b of rawBytes) {
        const hex = `<0x${b.toString(16).toUpperCase().padStart(2, "0")}>`;
        if (typeof tok.vocab[hex] === "number") byteIds.push(tok.vocab[hex]);
        else {
          allBytesFound = false;
          break;
        }
      }
      if (allBytesFound) {
        for (let k = byteIds.length - 1; k >= 0; k -= 1) rev.push(byteIds[k]);
      } else {
        rev.push(tok.unkTokenId);
      }
    } else {
      rev.push(tid);
    }
    cur = prev;
  }
  rev.reverse();
  for (const tid of rev) ids.push(tid);
  const cap = Math.max(8, tok.maxLength - 1);
  if (ids.length > cap) ids.length = cap;
  ids.push(tok.eosTokenId);
  return ids;
}

export function decodeMarianTokens(tok, tokenIds) {
  let out = "";
  for (const tid of tokenIds) {
    if (tid === tok.eosTokenId || tid === tok.padTokenId) continue;
    if (tid < 0 || tid >= tok.idToPiece.length) continue;
    const piece = tok.idToPiece[tid];
    if (!piece || piece === "<pad>" || piece === "</s>" || piece === "<unk>") continue;
    out += piece;
  }
  return out.replace(/\u2581/g, " ").trim();
}

function isNgramBlocked(generated, candidate) {
  const n = generated.length;
  if (n < 3) return false;
  const a = generated[n - 2];
  const b = generated[n - 1];
  for (let i = 0; i + 2 < n; i += 1) {
    if (generated[i] === a && generated[i + 1] === b && generated[i + 2] === candidate) return true;
  }
  return false;
}

function pickBestLogit(logits, offset, vocabSize, tok, generated) {
  let bestTok = tok.eosTokenId;
  let bestVal = -Infinity;
  for (let v = 0; v < vocabSize; v += 1) {
    if (v === tok.padTokenId) continue;
    if (isNgramBlocked(generated, v)) continue;
    const val = logits[offset + v];
    if (val > bestVal) {
      bestVal = val;
      bestTok = v;
    }
  }
  return bestTok;
}

export function splitTranslationSentences(text) {
  const lines = String(text || "").split("\n");
  const out = [];
  for (const line of lines) {
    const trimmed = line.trim();
    if (!trimmed) continue;
    let cur = "";
    for (const ch of trimmed) {
      cur += ch;
      if (cur.length >= 100 && ["。", "！", "？", ".", "!", "?", "；", ";"].includes(ch)) {
        out.push(cur.trim());
        cur = "";
      }
    }
    if (cur.trim()) out.push(cur.trim());
  }
  return out;
}

async function readBinaryResource(src) {
  if (src instanceof Uint8Array) return src;
  if (src instanceof ArrayBuffer) return new Uint8Array(src);
  if (typeof src !== "string") throw new Error("invalid model resource");
  const isNodeLocal = Boolean(globalThis.process?.versions?.node) && !/^https?:\/\//i.test(src);
  if (isNodeLocal) {
    const fs = await import("node:fs/promises");
    const buf = await fs.readFile(src);
    return new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
  }
  const res = await fetch(src);
  if (!res.ok) throw new Error(`failed to fetch ${src} (${res.status})`);
  return new Uint8Array(await res.arrayBuffer());
}

async function readJsonResource(src) {
  if (src && typeof src === "object" && !(src instanceof Uint8Array) && !(src instanceof ArrayBuffer)) {
    return src;
  }
  const bytes = await readBinaryResource(src);
  return JSON.parse(new TextDecoder("utf-8").decode(bytes));
}

export async function createMarianHop(ort, source) {
  const files = typeof source === "string" ? requiredTranslationFiles(source) : source;
  const [config, vocab, spmBytes, encBytes, decBytes] = await Promise.all([
    readJsonResource(files.config),
    readJsonResource(files.vocab),
    readBinaryResource(files.sourceSpm),
    readBinaryResource(files.encoder),
    readBinaryResource(files.decoder),
  ]);
  const tok = buildMarianTokenizerData(config, vocab, spmBytes, files.targetPrefix || "");
  const sessionOpts = { executionProviders: ["wasm"], graphOptimizationLevel: "all" };
  const [encoder, decoder] = await Promise.all([
    ort.InferenceSession.create(encBytes, sessionOpts),
    ort.InferenceSession.create(decBytes, sessionOpts),
  ]);
  const hasMergedCache = Array.isArray(decoder.inputNames) && decoder.inputNames.includes("use_cache_branch");

  async function translateSentence(sentence) {
    const srcIds = encodeMarianText(tok, sentence);
    const seqLen = srcIds.length;
    const inputIds = new ort.Tensor("int64", BigInt64Array.from(srcIds.map((x) => BigInt(x))), [1, seqLen]);
    const attnMask = new ort.Tensor("int64", new BigInt64Array(seqLen).fill(1n), [1, seqLen]);
    const encOut = await encoder.run({ input_ids: inputIds, attention_mask: attnMask });
    const hidden = encOut.last_hidden_state;

    const generated = [tok.decoderStartTokenId];
    const maxSteps = Math.min(256, Math.max(32, seqLen * 4 + 16));

    if (hasMergedCache) {
      const L = tok.decoderLayers;
      const heads = tok.decoderHeads;
      const headDim = Math.floor(tok.dModel / heads);
      let past = {};
      for (let l = 0; l < L; l += 1) {
        for (const kind of ["decoder", "encoder"]) {
          for (const kv of ["key", "value"]) {
            past[`past_key_values.${l}.${kind}.${kv}`] = new ort.Tensor("float32", new Float32Array(0), [
              1,
              heads,
              0,
              headDim,
            ]);
          }
        }
      }
      for (let step = 0; step < maxSteps; step += 1) {
        const useCache = step > 0;
        const stepIds = new ort.Tensor(
          "int64",
          BigInt64Array.from([BigInt(generated[generated.length - 1])]),
          [1, 1],
        );
        const feeds = {
          encoder_attention_mask: attnMask,
          input_ids: stepIds,
          encoder_hidden_states: hidden,
          use_cache_branch: new ort.Tensor("bool", [useCache], [1]),
          ...past,
        };
        const decOut = await decoder.run(feeds);
        const logits = decOut.logits;
        const vocabSize = logits.dims[2] || tok.idToPiece.length;
        const nextTok = pickBestLogit(logits.data, 0, vocabSize, tok, generated);
        for (let l = 0; l < L; l += 1) {
          past[`past_key_values.${l}.decoder.key`] = decOut[`present.${l}.decoder.key`];
          past[`past_key_values.${l}.decoder.value`] = decOut[`present.${l}.decoder.value`];
          if (!useCache) {
            past[`past_key_values.${l}.encoder.key`] = decOut[`present.${l}.encoder.key`];
            past[`past_key_values.${l}.encoder.value`] = decOut[`present.${l}.encoder.value`];
          }
        }
        if (nextTok === tok.eosTokenId) break;
        generated.push(nextTok);
      }
    } else {
      for (let step = 0; step < maxSteps; step += 1) {
        const stepIds = new ort.Tensor(
          "int64",
          BigInt64Array.from(generated.map((x) => BigInt(x))),
          [1, generated.length],
        );
        const decOut = await decoder.run({
          encoder_attention_mask: attnMask,
          input_ids: stepIds,
          encoder_hidden_states: hidden,
        });
        const logits = decOut.logits;
        const vocabSize = logits.dims[2] || tok.idToPiece.length;
        const rowOffset = (generated.length - 1) * vocabSize;
        const nextTok = pickBestLogit(logits.data, rowOffset, vocabSize, tok, generated);
        if (nextTok === tok.eosTokenId) break;
        generated.push(nextTok);
      }
    }

    return decodeMarianTokens(tok, generated);
  }

  return {
    async translate(text) {
      const parts = splitTranslationSentences(text);
      if (parts.length === 0) return "";
      const out = [];
      for (const part of parts) {
        const piece = await translateSentence(part);
        if (piece) out.push(piece);
      }
      return out.join(" ");
    },
    async free() {
      for (const sess of [encoder, decoder]) {
        try {
          if (sess && typeof sess.release === "function") await sess.release();
        } catch {
          // Already released.
        }
      }
    },
  };
}

export async function createMarianTranslator(ort, hopSources) {
  const sources = Array.isArray(hopSources) ? hopSources.filter(Boolean) : [hopSources].filter(Boolean);
  if (sources.length === 0) {
    throw new Error("at least one translation model hop is required");
  }
  const hops = [];
  try {
    for (const src of sources) {
      hops.push(await createMarianHop(ort, src));
    }
  } catch (err) {
    for (const h of hops) await h.free();
    throw err;
  }
  return {
    async translate(text) {
      let cur = String(text || "");
      for (const hop of hops) {
        cur = await hop.translate(cur);
        if (!cur) break;
      }
      return cur;
    },
    async free() {
      for (const hop of hops) await hop.free();
    },
  };
}
