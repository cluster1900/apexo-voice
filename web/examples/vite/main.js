import { createSession } from "@apexo/sdk";

const transcriptEl = document.querySelector("#transcript");
const summaryEl = document.querySelector("#summary");
const statusEl = document.querySelector("#status");

export const session = createSession({
  asrModelPath: "/meeting/models/asr/",
  summaryModelPath: "/meeting/models/gemma-4-E2B-it-gpu.litertlm",
  sherpaUrl: "/meeting/assets/apexo/sherpa/",
  litertWasmUrl: "/meeting/assets/apexo/litert/",
  onTranscript(event) {
    transcriptEl.textContent = session.displayText;
    statusEl.textContent = event.isFinal ? "收到定稿" : "识别中";
  },
});

window.__apexoViteSession = session;
