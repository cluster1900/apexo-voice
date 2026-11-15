// Language normalization, model package catalog (streaming ASR + MarianMT ONNX translation),
// and localized summary prompts for Apexo SDK.

export const SUPPORTED_LANGUAGES = [
  { code: "zh", name: "中文", englishName: "Chinese", speechLang: "zh-CN" },
  { code: "en", name: "English", englishName: "English", speechLang: "en-US" },
  { code: "ja", name: "日本語", englishName: "Japanese", speechLang: "ja-JP" },
  { code: "ko", name: "한국어", englishName: "Korean", speechLang: "ko-KR" },
  { code: "fr", name: "Français", englishName: "French", speechLang: "fr-FR" },
  { code: "de", name: "Deutsch", englishName: "German", speechLang: "de-DE" },
  { code: "es", name: "Español", englishName: "Spanish", speechLang: "es-ES" },
  { code: "ru", name: "Русский", englishName: "Russian", speechLang: "ru-RU" },
];

const SHERPA_RELEASE_BASE = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models";

export const ASR_PACKAGES = {
  "asr-zh": {
    id: "asr-zh",
    kind: "asr",
    lang: "zh",
    title: "中文流式识别包 (Zipformer zh 14M)",
    sizeMB: 71,
    archiveUrl: `${SHERPA_RELEASE_BASE}/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23.tar.bz2`,
  },
  "asr-en": {
    id: "asr-en",
    kind: "asr",
    lang: "en",
    title: "English Streaming ASR (Zipformer Kroko)",
    sizeMB: 55,
    archiveUrl: `${SHERPA_RELEASE_BASE}/sherpa-onnx-streaming-zipformer-en-kroko-2025-08-06.tar.bz2`,
  },
  "asr-ja": {
    id: "asr-ja",
    kind: "asr",
    lang: "ja",
    title: "日本語ストリーミング認識 (Zipformer Multi)",
    sizeMB: 247,
    archiveUrl: `${SHERPA_RELEASE_BASE}/sherpa-onnx-streaming-zipformer-ar_en_id_ja_ru_th_vi_zh-2025-02-10.tar.bz2`,
  },
  "asr-ko": {
    id: "asr-ko",
    kind: "asr",
    lang: "ko",
    title: "한국어 스트리밍 음성인식 (Zipformer Korean)",
    sizeMB: 360,
    archiveUrl: `${SHERPA_RELEASE_BASE}/sherpa-onnx-streaming-zipformer-korean-2024-06-16-mobile.tar.bz2`,
  },
  "asr-fr": {
    id: "asr-fr",
    kind: "asr",
    lang: "fr",
    title: "Français Streaming ASR (Zipformer Kroko)",
    sizeMB: 55,
    archiveUrl: `${SHERPA_RELEASE_BASE}/sherpa-onnx-streaming-zipformer-fr-kroko-2025-08-06.tar.bz2`,
  },
  "asr-de": {
    id: "asr-de",
    kind: "asr",
    lang: "de",
    title: "Deutsch Streaming ASR (Zipformer Kroko)",
    sizeMB: 55,
    archiveUrl: `${SHERPA_RELEASE_BASE}/sherpa-onnx-streaming-zipformer-de-kroko-2025-08-06.tar.bz2`,
  },
  "asr-es": {
    id: "asr-es",
    kind: "asr",
    lang: "es",
    title: "Español Streaming ASR (Zipformer Kroko)",
    sizeMB: 119,
    archiveUrl: `${SHERPA_RELEASE_BASE}/sherpa-onnx-streaming-zipformer-es-kroko-2025-08-06.tar.bz2`,
  },
  "asr-ru": {
    id: "asr-ru",
    kind: "asr",
    lang: "ru",
    title: "Русский Streaming ASR (Zipformer Small int8)",
    sizeMB: 23,
    archiveUrl: `${SHERPA_RELEASE_BASE}/sherpa-onnx-streaming-zipformer-small-ru-vosk-int8-2025-08-16.tar.bz2`,
  },
};

function xenovaPack(id, from, to, repo, title, sizeMB = 108) {
  const base = `https://huggingface.co/${repo}/resolve/main`;
  return {
    id,
    kind: "trans",
    from,
    to,
    repo,
    title,
    sizeMB,
    files: {
      "config.json": `${base}/config.json`,
      "vocab.json": `${base}/vocab.json`,
      "source.spm": `${base}/source.spm`,
      "encoder_model.onnx": `${base}/onnx/encoder_model_quantized.onnx`,
      "decoder_model.onnx": `${base}/onnx/decoder_model_merged_quantized.onnx`,
    },
  };
}

export const TRANSLATION_PACKAGES = {
  "trans-zh-en": xenovaPack("trans-zh-en", "zh", "en", "Xenova/opus-mt-zh-en", "中文 → English 离线翻译包"),
  "trans-en-zh": xenovaPack("trans-en-zh", "en", "zh", "Xenova/opus-mt-en-zh", "English → 中文 离线翻译包"),
  "trans-ja-en": xenovaPack("trans-ja-en", "ja", "en", "Xenova/opus-mt-ja-en", "日本語 → English 离线翻译包"),
  "trans-en-ja": xenovaPack("trans-en-ja", "en", "ja", "Xenova/opus-mt-en-jap", "English → 日本語 离线翻译包"),
  "trans-ko-en": xenovaPack("trans-ko-en", "ko", "en", "Xenova/opus-mt-ko-en", "한국어 → English 离线翻译包"),
  "trans-en-ko": {
    id: "trans-en-ko",
    kind: "trans",
    from: "en",
    to: "ko",
    repo: "R4kSo1997/opus-mt-en-ko-onnx-int8",
    title: "English → 한국어 离线翻译包",
    sizeMB: 62,
    files: {
      "config.json": "https://huggingface.co/R4kSo1997/opus-mt-en-ko-onnx-int8/resolve/main/config.json",
      "vocab.json": "https://huggingface.co/R4kSo1997/opus-mt-en-ko-onnx-int8/resolve/main/vocab.json",
      "source.spm": "https://huggingface.co/R4kSo1997/opus-mt-en-ko-onnx-int8/resolve/main/source.spm",
      "encoder_model.onnx":
        "https://huggingface.co/R4kSo1997/opus-mt-en-ko-onnx-int8/resolve/main/encoder_model_quantized.onnx",
      "decoder_model.onnx":
        "https://huggingface.co/R4kSo1997/opus-mt-en-ko-onnx-int8/resolve/main/decoder_model_quantized.onnx",
    },
  },
  "trans-fr-en": xenovaPack("trans-fr-en", "fr", "en", "Xenova/opus-mt-fr-en", "Français → English 离线翻译包"),
  "trans-en-fr": xenovaPack("trans-en-fr", "en", "fr", "Xenova/opus-mt-en-fr", "English → Français 离线翻译包"),
  "trans-de-en": xenovaPack("trans-de-en", "de", "en", "Xenova/opus-mt-de-en", "Deutsch → English 离线翻译包"),
  "trans-en-de": xenovaPack("trans-en-de", "en", "de", "Xenova/opus-mt-en-de", "English → Deutsch 离线翻译包"),
  "trans-es-en": xenovaPack("trans-es-en", "es", "en", "Xenova/opus-mt-es-en", "Español → English 离线翻译包"),
  "trans-en-es": xenovaPack("trans-en-es", "en", "es", "Xenova/opus-mt-en-es", "English → Español 离线翻译包"),
  "trans-ru-en": xenovaPack("trans-ru-en", "ru", "en", "Xenova/opus-mt-ru-en", "Русский → English 离线翻译包"),
  "trans-en-ru": xenovaPack("trans-en-ru", "en", "ru", "Xenova/opus-mt-en-ru", "English → Русский 离线翻译包"),
};

export function normalizeLang(raw) {
  if (!raw || typeof raw !== "string") return "";
  const cleaned = raw.trim().toLowerCase().replace(/_/g, "-");
  if (!cleaned) return "";
  const primary = cleaned.split("-")[0];
  if (primary === "zh" || primary === "cmn" || primary === "yue" || primary === "zho" || primary === "chi") return "zh";
  if (primary === "en" || primary === "eng") return "en";
  if (primary === "ja" || primary === "jp" || primary === "jpn" || primary === "jap") return "ja";
  if (primary === "ko" || primary === "kr" || primary === "kor") return "ko";
  if (primary === "fr" || primary === "fra" || primary === "fre") return "fr";
  if (primary === "de" || primary === "deu" || primary === "ger") return "de";
  if (primary === "es" || primary === "spa") return "es";
  if (primary === "ru" || primary === "rus") return "ru";
  return primary;
}

export function speechRecognitionLangFor(langCode) {
  const norm = normalizeLang(langCode) || "zh";
  const found = SUPPORTED_LANGUAGES.find((item) => item.code === norm);
  return found ? found.speechLang : norm;
}

export function resolveTranslationPlan(sourceLang, targetLang) {
  const src = normalizeLang(sourceLang);
  const tgt = normalizeLang(targetLang);
  if (!tgt || !src || src === tgt) {
    return { mode: "passthrough", packs: [] };
  }
  const direct = TRANSLATION_PACKAGES[`trans-${src}-${tgt}`];
  if (direct) {
    return { mode: "direct", packs: [direct] };
  }
  const hop1 = TRANSLATION_PACKAGES[`trans-${src}-en`];
  const hop2 = TRANSLATION_PACKAGES[`trans-en-${tgt}`];
  if (hop1 && hop2) {
    return { mode: "pivot", packs: [hop1, hop2] };
  }
  return { mode: "unsupported", packs: [] };
}

export function getLanguagePackPlan(sourceLang, targetLang) {
  const src = normalizeLang(sourceLang) || "zh";
  const tgt = normalizeLang(targetLang) || src;
  const asrPack = ASR_PACKAGES[`asr-${src}`] || null;
  const transPlan = resolveTranslationPlan(src, tgt);
  return {
    sourceLang: src,
    targetLang: tgt,
    asrPack,
    translationMode: transPlan.mode,
    translationPacks: transPlan.packs,
  };
}

export const CHUNK_INSTRUCTIONS = {
  zh:
    "仅依据以下转写提取简洁事实摘要。\n" +
    "保留决定、待办、明确的负责人和时间，以及对先前决定的修改或撤销。\n" +
    "没有明确给出的信息不要补充；尚未确定的事项保持未确定。\n" +
    "按原文发生顺序概括，保留“取消”“改为”“最终决定”等关系。\n" +
    "转写：\n",
  en:
    "Extract a concise factual summary strictly based on the following transcript. Write strictly in English.\n" +
    "Keep decisions, action items, explicit owners and times, and any modifications or cancellations of earlier decisions.\n" +
    "Do not add unstated information; keep undecided items marked as undecided.\n" +
    "Summarize in chronological order, preserving relations such as \"cancelled\", \"changed to\", and \"final decision\".\n" +
    "Transcript:\n",
  ja:
    "以下の文字起こしのみに基づいて簡潔な事実要約を作成してください。出力は必ず日本語のみで記述してください。\n" +
    "決定事項、ToDo、明確な担当者と日時、および以前の決定に対する変更や取り消しを保持してください。\n" +
    "明示されていない情報を補完せず、未確定の事項は未確定のままにしてください。\n" +
    "発言順に要約し、「中止」「変更」「最終決定」などの関係を保持してください。\n" +
    "文字起こし：\n",
  ko:
    "다음 전사 내용만을 바탕으로 간결한 사실 요약을 추출하세요. 반드시 한국어로만 작성하세요.\n" +
    "결정 사항, 할 일, 명확한 담당자와 시간, 이전 결정에 대한 수정이나 취소를 유지하세요.\n" +
    "명시되지 않은 정보는 추가하지 말고, 미정인 사항은 미정으로 유지하세요.\n" +
    "발생 순서대로 요약하며 '취소', '변경', '최종 결정' 등의 관계를 유지하세요.\n" +
    "전사 내용:\n",
  fr:
    "Extrayez un résumé factuel concis basé uniquement sur la transcription suivante. Rédigez strictement en français.\n" +
    "Conservez les décisions, les actions à entreprendre, les responsables et horaires explicites, ainsi que les modifications ou annulations.\n" +
    "N'ajoutez aucune information non mentionnée ; laissez les points indécis comme non résolus.\n" +
    "Résumez dans l'ordre chronologique en préservant les relations telles que « annulé », « remplacé par » et « décision finale ».\n" +
    "Transcription :\n",
  de:
    "Erstellen Sie eine prägnante sachliche Zusammenfassung ausschließlich auf Basis des folgenden Transkripts. Schreiben Sie ausschließlich auf Deutsch.\n" +
    "Behalten Sie Entscheidungen, Aufgaben, explizite Verantwortliche und Zeiten sowie Änderungen oder Stornierungen früherer Entscheidungen bei.\n" +
    "Fügen Sie keine nicht genannten Informationen hinzu; lassen Sie ungeklärte Punkte als offen stehen.\n" +
    "Fassen Sie chronologisch zusammen und bewahren Sie Bezüge wie „storniert“, „geändert in“ und „endgültige Entscheidung“.\n" +
    "Transkript:\n",
  es:
    "Extraiga un resumen factual conciso basado únicamente en la siguiente transcripción. Escriba estrictamente en español.\n" +
    "Conserve las decisiones, tareas pendientes, responsables y fechas/horas explícitas, así como modificaciones o cancelaciones.\n" +
    "No añada información no mencionada; mantenga los asuntos no decididos como pendientes.\n" +
    "Resuma en orden cronológico conservando relaciones como «cancelado», «cambiado a» y «decisión final».\n" +
    "Transcripción:\n",
  ru:
    "Извлеките краткую фактическую сводку строго на основе следующей расшифровки. Пишите строго на русском языке.\n" +
    "Сохраняйте решения, задачи, явно указанных ответственных и сроки, а также изменения или отмены прежних решений.\n" +
    "Не добавляйте неуказанную информацию; нерешённые вопросы оставляйте нерешёнными.\n" +
    "Резюмируйте в хронологическом порядке, сохраняя связи «отменено», «изменено на», «окончательное решение».\n" +
    "Расшифровка:\n",
};

export const SYNTHESIS_INSTRUCTIONS = {
  zh:
    "以下内容按会议发生顺序排列。请归纳成一份简洁的会议总结。\n" +
    "合并重复事项；对于明确被后续内容修改或撤销的决定，以最后明确决定为准。\n" +
    "保留仍未确定或互相矛盾且没有结论的事项，不自行猜测。\n" +
    "只写材料中有依据的要点、最终决定和待办，不补负责人或日期。\n" +
    "材料：\n",
  en:
    "The following content is arranged in chronological meeting order. Synthesize it into a concise meeting summary written strictly in English.\n" +
    "Merge duplicate items; for decisions explicitly modified or cancelled later, keep the final decision.\n" +
    "Retain undecided or contradictory items without guessing.\n" +
    "Write only evidence-backed key points, final decisions, and action items without inventing owners or dates.\n" +
    "Material:\n",
  ja:
    "以下の内容は会議の時系列順に並んでいます。簡潔な会議のまとめに統合し、必ず日本語のみで記述してください。\n" +
    "重複項目を統合し、後の発言で明確に変更または取り消された決定については最終決定を優先してください。\n" +
    "未確定または矛盾して結論が出ていない事項は推測せずそのまま残してください。\n" +
    "根拠のある要点、最終決定、ToDoのみを記載し、担当者や日付を勝手に補わないでください。\n" +
    "資料：\n",
  ko:
    "다음 내용은 회의 진행 순서대로 정리되어 있습니다. 간결한 회의 요약으로 통합하되 반드시 한국어로만 작성하세요.\n" +
    "중복 항목을 병합하고, 이후 내용에서 명확히 수정되거나 취소된 결정은 최종 결정을 기준으로 하세요.\n" +
    "미정이거나 상충되어 결론이 나지 않은 사항은 추측하지 말고 그대로 유지하세요.\n" +
    "근거가 있는 핵심 요점, 최종 결정 및 할 일만 작성하고 담당자나 날짜를 임의로 추가하지 마세요.\n" +
    "자료:\n",
  fr:
    "Le contenu suivant est classé par ordre chronologique. Synthétisez-le en un compte rendu concis rédigé strictement en français.\n" +
    "Fusionnez les doublons ; pour les décisions modifiées ou annulées ultérieurement, retenez la décision finale.\n" +
    "Conservez les points indécis ou contradictoires sans deviner.\n" +
    "N'écrivez que les points clés, décisions finales et actions fondés sur le texte, sans inventer de responsables ni de dates.\n" +
    "Matériel :\n",
  de:
    "Die folgenden Inhalte sind chronologisch geordnet. Fassen Sie sie zu einer prägnanten Besprechungszusammenfassung ausschließlich auf Deutsch zusammen.\n" +
    "Führen Sie Duplikate zusammen; bei später geänderten oder stornierten Entscheidungen gilt die letzte eindeutige Entscheidung.\n" +
    "Behalten Sie offene oder widersprüchliche Punkte bei, ohne zu raten.\n" +
    "Schreiben Sie nur belegte Kernpunkte, endgültige Entscheidungen und Aufgaben, ohne Verantwortliche oder Daten zu erfinden.\n" +
    "Material:\n",
  es:
    "El siguiente contenido está ordenado cronológicamente. Sintetícelo en un resumen conciso de la reunión escrito estrictamente en español.\n" +
    "Combine elementos duplicados; para decisiones modificadas o canceladas posteriormente, mantenga la decisión final.\n" +
    "Conserve los puntos pendientes o contradictorios sin adivinar.\n" +
    "Escriba solo puntos clave, decisiones finales y tareas respaldados por el material, sin inventar responsables ni fechas.\n" +
    "Material:\n",
  ru:
    "Следующие материалы расположены в хронологическом порядке встречи. Объедините их в краткое резюме встречи строго на русском языке.\n" +
    "Объедините дубликаты; если решение было явно изменено или отменено позже, опирайтесь на окончательное решение.\n" +
    "Сохраняйте нерешённые или противоречивые вопросы без догадок.\n" +
    "Указывайте только подтверждённые тезисы, итоговые решения и задачи, не додумывая ответственных или даты.\n" +
    "Материал:\n",
};

export function buildSummaryPrompts(targetLang) {
  const norm = normalizeLang(targetLang);
  if (!norm || norm === "zh") {
    return {
      chunkInstruction: CHUNK_INSTRUCTIONS.zh,
      synthesisInstruction: SYNTHESIS_INSTRUCTIONS.zh,
    };
  }
  if (CHUNK_INSTRUCTIONS[norm] && SYNTHESIS_INSTRUCTIONS[norm]) {
    return {
      chunkInstruction: CHUNK_INSTRUCTIONS[norm],
      synthesisInstruction: SYNTHESIS_INSTRUCTIONS[norm],
    };
  }
  const found = SUPPORTED_LANGUAGES.find((item) => item.code === norm);
  const label = found ? found.englishName : norm;
  return {
    chunkInstruction:
      `Extract a concise factual summary strictly based on the following transcript. Write strictly in ${label}.\n` +
      "Keep decisions, action items, explicit owners and times, and any modifications or cancellations.\n" +
      "Do not add unstated information; summarize in chronological order.\nTranscript:\n",
    synthesisInstruction:
      `The following content is arranged in chronological order. Synthesize it into a concise summary written strictly in ${label}.\n` +
      "Merge duplicate items; keep final decisions when earlier ones were modified or cancelled; do not invent facts.\nMaterial:\n",
  };
}
