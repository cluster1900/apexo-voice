#include "apexo_session.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

const char* kChunkInstructionZh =
    "仅依据以下转写提取简洁事实摘要。\n"
    "保留决定、待办、明确的负责人和时间，以及对先前决定的修改或撤销。\n"
    "没有明确给出的信息不要补充；尚未确定的事项保持未确定。\n"
    "按原文发生顺序概括，保留“取消”“改为”“最终决定”等关系。\n"
    "转写：\n";

const char* kSynthesizeInstructionZh =
    "以下内容按会议发生顺序排列。请归纳成一份简洁的会议总结。\n"
    "合并重复事项；对于明确被后续内容修改或撤销的决定，以最后明确决定为准。\n"
    "保留仍未确定或互相矛盾且没有结论的事项，不自行猜测。\n"
    "只写材料中有依据的要点、最终决定和待办，不补负责人或日期。\n"
    "材料：\n";

std::string normalize_lang(const char* raw) {
  if (raw == nullptr) return "";
  std::string s(raw);
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
    s.erase(s.begin());
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
    s.pop_back();
  }
  std::string out;
  for (char c : s) {
    if (c == '-' || c == '_') break;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    out.push_back(c);
  }
  if (out == "zho" || out == "cmn") return "zh";
  if (out == "eng") return "en";
  if (out == "jpn" || out == "jap") return "ja";
  if (out == "kor") return "ko";
  if (out == "fra" || out == "fre") return "fr";
  if (out == "deu" || out == "ger") return "de";
  if (out == "spa") return "es";
  if (out == "rus") return "ru";
  return out;
}

const char* language_english_name(const std::string& lang) {
  if (lang == "zh") return "Simplified Chinese";
  if (lang == "en") return "English";
  if (lang == "ja") return "Japanese";
  if (lang == "ko") return "Korean";
  if (lang == "fr") return "French";
  if (lang == "de") return "German";
  if (lang == "es") return "Spanish";
  if (lang == "ru") return "Russian";
  return nullptr;
}

std::string chunk_instruction_for(const std::string& target_lang) {
  if (target_lang.empty() || target_lang == "zh") return kChunkInstructionZh;
  if (target_lang == "en") {
    return "Extract a concise factual summary strictly based on the following transcript. Write strictly in English.\n"
           "Keep decisions, action items, explicit owners and times, and any modifications or cancellations of earlier decisions.\n"
           "Do not add unstated information; keep undecided items marked as undecided.\n"
           "Summarize in chronological order, preserving relations such as \"cancelled\", \"changed to\", and \"final decision\".\n"
           "Transcript:\n";
  }
  if (target_lang == "ja") {
    return "以下の文字起こしのみに基づいて簡潔な事実要約を作成してください。出力は必ず日本語のみで記述してください。\n"
           "決定事項、ToDo、明確な担当者と日時、および以前の決定に対する変更や取り消しを保持してください。\n"
           "明示されていない情報を補完せず、未確定の事項は未確定のままにしてください。\n"
           "発言順に要約し、「中止」「変更」「最終決定」などの関係を保持してください。\n"
           "文字起こし：\n";
  }
  if (target_lang == "ko") {
    return "다음 전사 내용만을 바탕으로 간결한 사실 요약을 추출하세요. 반드시 한국어로만 작성하세요.\n"
           "결정 사항, 할 일, 명확한 담당자와 시간, 이전 결정에 대한 수정이나 취소를 유지하세요.\n"
           "명시되지 않은 정보는 추가하지 말고, 미정인 사항은 미정으로 유지하세요.\n"
           "발생 순서대로 요약하며 '취소', '변경', '최종 결정' 등의 관계를 유지하세요.\n"
           "전사 내용:\n";
  }
  if (target_lang == "fr") {
    return "Extrayez un résumé factuel concis basé uniquement sur la transcription suivante. Rédigez strictement en français.\n"
           "Conservez les décisions, les actions à entreprendre, les responsables et horaires explicites, ainsi que les modifications ou annulations.\n"
           "N'ajoutez aucune information non mentionnée ; laissez les points indécis comme non résolus.\n"
           "Résumez dans l'ordre chronologique en préservant les relations telles que « annulé », « remplacé par » et « décision finale ».\n"
           "Transcription :\n";
  }
  if (target_lang == "de") {
    return "Erstellen Sie eine prägnante sachliche Zusammenfassung ausschließlich auf Basis des folgenden Transkripts. Schreiben Sie ausschließlich auf Deutsch.\n"
           "Behalten Sie Entscheidungen, Aufgaben, explizite Verantwortliche und Zeiten sowie Änderungen oder Stornierungen früherer Entscheidungen bei.\n"
           "Fügen Sie keine nicht genannten Informationen hinzu; lassen Sie ungeklärte Punkte als offen stehen.\n"
           "Fassen Sie chronologisch zusammen und bewahren Sie Bezüge wie „storniert“, „geändert in“ und „endgültige Entscheidung“.\n"
           "Transkript:\n";
  }
  if (target_lang == "es") {
    return "Extraiga un resumen factual conciso basado únicamente en la siguiente transcripción. Escriba estrictamente en español.\n"
           "Conserve las decisiones, tareas pendientes, responsables y fechas/horas explícitas, así como modificaciones o cancelaciones.\n"
           "No añada información no mencionada; mantenga los asuntos no decididos como pendientes.\n"
           "Resuma en orden cronológico conservando relaciones como «cancelado», «cambiado a» y «decisión final».\n"
           "Transcripción:\n";
  }
  if (target_lang == "ru") {
    return "Извлеките краткую фактическую сводку строго на основе следующей расшифровки. Пишите строго на русском языке.\n"
           "Сохраняйте решения, задачи, явно указанных ответственных и сроки, а также изменения или отмены прежних решений.\n"
           "Не добавляйте неуказанную информацию; нерешённые вопросы оставляйте нерешёнными.\n"
           "Резюмируйте в хронологическом порядке, сохраняя связи «отменено», «изменено на», «окончательное решение».\n"
           "Расшифровка:\n";
  }
  const char* name = language_english_name(target_lang);
  const std::string label = name != nullptr ? name : target_lang;
  return "Extract a concise factual summary strictly based on the following transcript. Write strictly in " + label +
         ".\nKeep decisions, action items, explicit owners and times, and any modifications or cancellations.\n"
         "Do not add unstated information; summarize in chronological order.\nTranscript:\n";
}

std::string synthesize_instruction_for(const std::string& target_lang) {
  if (target_lang.empty() || target_lang == "zh") return kSynthesizeInstructionZh;
  if (target_lang == "en") {
    return "The following content is arranged in chronological meeting order. Synthesize it into a concise meeting summary written strictly in English.\n"
           "Merge duplicate items; for decisions explicitly modified or cancelled later, keep the final decision.\n"
           "Retain undecided or contradictory items without guessing.\n"
           "Write only evidence-backed key points, final decisions, and action items without inventing owners or dates.\n"
           "Material:\n";
  }
  if (target_lang == "ja") {
    return "以下の内容は会議の時系列順に並んでいます。簡潔な会議のまとめに統合し、必ず日本語のみで記述してください。\n"
           "重複項目を統合し、後の発言で明確に変更または取り消された決定については最終決定を優先してください。\n"
           "未確定または矛盾して結論が出ていない事項は推測せずそのまま残してください。\n"
           "根拠のある要点、最終決定、ToDoのみを記載し、担当者や日付を勝手に補わないでください。\n"
           "資料：\n";
  }
  if (target_lang == "ko") {
    return "다음 내용은 회의 진행 순서대로 정리되어 있습니다. 간결한 회의 요약으로 통합하되 반드시 한국어로만 작성하세요.\n"
           "중복 항목을 병합하고, 이후 내용에서 명확히 수정되거나 취소된 결정은 최종 결정을 기준으로 하세요.\n"
           "미정이거나 상충되어 결론이 나지 않은 사항은 추측하지 말고 그대로 유지하세요.\n"
           "근거가 있는 핵심 요점, 최종 결정 및 할 일만 작성하고 담당자나 날짜를 임의로 추가하지 마세요.\n"
           "자료:\n";
  }
  if (target_lang == "fr") {
    return "Le contenu suivant est classé par ordre chronologique. Synthétisez-le en un compte rendu concis rédigé strictement en français.\n"
           "Fusionnez les doublons ; pour les décisions modifiées ou annulées ultérieurement, retenez la décision finale.\n"
           "Conservez les points indécis ou contradictoires sans deviner.\n"
           "N'écrivez que les points clés, décisions finales et actions fondés sur le texte, sans inventer de responsables ni de dates.\n"
           "Matériel :\n";
  }
  if (target_lang == "de") {
    return "Die folgenden Inhalte sind chronologisch geordnet. Fassen Sie sie zu einer prägnanten Besprechungszusammenfassung ausschließlich auf Deutsch zusammen.\n"
           "Führen Sie Duplikate zusammen; bei später geänderten oder stornierten Entscheidungen gilt die letzte eindeutige Entscheidung.\n"
           "Behalten Sie offene oder widersprüchliche Punkte bei, ohne zu raten.\n"
           "Schreiben Sie nur belegte Kernpunkte, endgültige Entscheidungen und Aufgaben, ohne Verantwortliche oder Daten zu erfinden.\n"
           "Material:\n";
  }
  if (target_lang == "es") {
    return "El siguiente contenido está ordenado cronológicamente. Sintetícelo en un resumen conciso de la reunión escrito estrictamente en español.\n"
           "Combine elementos duplicados; para decisiones modificadas o canceladas posteriormente, mantenga la decisión final.\n"
           "Conserve los puntos pendientes o contradictorios sin adivinar.\n"
           "Escriba solo puntos clave, decisiones finales y tareas respaldados por el material, sin inventar responsables ni fechas.\n"
           "Material:\n";
  }
  if (target_lang == "ru") {
    return "Следующие материалы расположены в хронологическом порядке встречи. Объедините их в краткое резюме встречи строго на русском языке.\n"
           "Объедините дубликаты; если решение было явно изменено или отменено позже, опирайтесь на окончательное решение.\n"
           "Сохраняйте нерешённые или противоречивые вопросы без догадок.\n"
           "Указывайте только подтверждённые тезисы, итоговые решения и задачи, не додумывая ответственных или даты.\n"
           "Материал:\n";
  }
  const char* name = language_english_name(target_lang);
  const std::string label = name != nullptr ? name : target_lang;
  return "The following content is arranged in chronological order. Synthesize it into a concise summary written strictly in " +
         label +
         ".\nMerge duplicate items; keep final decisions when earlier ones were modified or cancelled; do not invent facts.\nMaterial:\n";
}

int utf8_len(unsigned char lead) {
  if ((lead & 0x80) == 0) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;
}

uint32_t decode_cp(const std::string& text, size_t i, int& len) {
  const unsigned char lead = static_cast<unsigned char>(text[i]);
  len = utf8_len(lead);
  if (i + static_cast<size_t>(len) > text.size()) len = 1;
  if (len == 1) return lead;
  uint32_t cp = lead & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
  for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i + static_cast<size_t>(k)]) & 0x3F);
  return cp;
}

bool is_cjk(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x3040 && cp <= 0x30FF);
}

struct Token {
  std::string text;
  bool word;
};

std::vector<Token> tokenize(const std::string& text) {
  std::vector<Token> tokens;
  std::string latin;
  auto flush = [&]() {
    if (latin.empty()) return;
    tokens.push_back({latin, true});
    latin.clear();
  };
  for (size_t i = 0; i < text.size();) {
    int len = 1;
    const uint32_t cp = decode_cp(text, i, len);
    const std::string piece = text.substr(i, static_cast<size_t>(len));
    i += static_cast<size_t>(len);
    if (cp == ' ' || cp == '\n' || cp == '\t' || cp == '\r') {
      flush();
      tokens.push_back({piece, false});
    } else if (is_cjk(cp)) {
      flush();
      tokens.push_back({piece, true});
    } else {
      latin += piece;
    }
  }
  flush();
  return tokens;
}

std::string trim_words(const std::string& text, int max_words) {
  const std::vector<Token> tokens = tokenize(text);
  int words = 0;
  for (const Token& token : tokens) {
    if (token.word) words += 1;
  }
  if (words <= max_words) return text;
  int drop = words - max_words;
  std::string kept;
  bool skipping = true;
  for (const Token& token : tokens) {
    if (skipping) {
      if (token.word) {
        drop -= 1;
        if (drop == 0) skipping = false;
      }
      continue;
    }
    kept += token.text;
  }
  return kept;
}

bool is_break_cp(uint32_t cp) {
  return cp == '\n' || cp == 0x3002 /* 。 */ || cp == 0xFF01 /* ！ */ || cp == 0xFF1F /* ？ */ || cp == '.' ||
         cp == '!' || cp == '?';
}

/* Splits at most max_chars Unicode characters per chunk. When a chunk is full the cut moves back to the last
 * line or sentence end inside it (if that keeps at least half of the chunk), so sentences are not cut in half. */
std::vector<std::string> chunk_text(const std::string& text, int max_chars) {
  std::vector<std::string> parts;
  if (max_chars < 1) max_chars = 1;
  std::string current;
  int chars = 0;
  size_t break_bytes = 0;
  int break_chars = 0;
  for (size_t i = 0; i < text.size();) {
    int len = utf8_len(static_cast<unsigned char>(text[i]));
    if (i + static_cast<size_t>(len) > text.size()) len = 1;
    if (chars >= max_chars) {
      if (break_bytes > 0 && break_chars * 2 >= max_chars && break_bytes < current.size()) {
        parts.push_back(current.substr(0, break_bytes));
        current.erase(0, break_bytes);
        chars -= break_chars;
      } else {
        parts.push_back(current);
        current.clear();
        chars = 0;
      }
      break_bytes = 0;
      break_chars = 0;
    }
    int ignored = 1;
    const uint32_t cp = decode_cp(text, i, ignored);
    current.append(text, i, static_cast<size_t>(len));
    chars += 1;
    if (is_break_cp(cp)) {
      break_bytes = current.size();
      break_chars = chars;
    }
    i += static_cast<size_t>(len);
  }
  if (!current.empty()) parts.push_back(current);
  return parts;
}

bool is_cjk_edge(const std::string& text, bool at_end) {
  if (text.empty()) return false;
  if (!at_end) {
    int len = 1;
    return is_cjk(decode_cp(text, 0, len));
  }
  size_t pos = text.size() - 1;
  while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) --pos;
  int len = 1;
  return is_cjk(decode_cp(text, pos, len));
}

bool is_space_edge(const std::string& text, bool at_end) {
  if (text.empty()) return true;
  const char c = at_end ? text.back() : text.front();
  return c == ' ' || c == '\n' || c == '\t' || c == '\r';
}

/* Joins two finals shown on one line. Non-CJK words (ASCII, Hangul, Cyrillic, accented Latin) get a space
 * between them; CJK text is joined directly. */
void append_joined(std::string* line, const std::string& next) {
  if (!line->empty() && !next.empty() && !is_space_edge(*line, true) && !is_space_edge(next, false) &&
      !is_cjk_edge(*line, true) && !is_cjk_edge(next, false)) {
    line->push_back(' ');
  }
  line->append(next);
}

bool has_litertlm_suffix(const std::string& path) {
  static const char kSuffix[] = ".litertlm";
  const size_t n = sizeof(kSuffix) - 1;
  if (path.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    char c = path[path.size() - n + i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != kSuffix[i]) return false;
  }
  return true;
}

/* Strips leading heading labels emitted by Gemma across supported target languages. */
std::string strip_label(const std::string& text) {
  size_t i = 0;
  auto is_space = [&](size_t k) {
    return text[k] == ' ' || text[k] == '\n' || text[k] == '\r' || text[k] == '\t';
  };
  while (i < text.size() && is_space(i)) ++i;
  const size_t start = i;
  while (i < text.size() && (text[i] == '#' || text[i] == '*' || text[i] == ' ')) ++i;
  static const char* const kLabels[] = {
      "摘要",           "总结",          "要約",    "まとめ",  "요약",   "Summary", "summary", "SUMMARY",
      "Résumé",         "résumé",        "RESUME",  "Zusammenfassung",    "Resumen", "resumen", "Резюме",
      "резюме",
  };
  static const char* const kStops[] = {"\n", ":", "\xEF\xBC\x9A" /* ： */, "\xEF\xBC\x8C" /* ， */,
                                       "\xE3\x80\x82" /* 。 */, ",", "."};
  for (int chars = 0; chars <= 8 && i < text.size(); ++chars) {
    for (const char* label : kLabels) {
      const size_t n = std::strlen(label);
      if (text.compare(i, n, label) != 0) continue;
      size_t k = i + n;
      while (k < text.size() && (text[k] == '*' || text[k] == ' ')) ++k;
      if (text.compare(k, 3, "\xEF\xBC\x9A") == 0) {
        k += 3;
      } else if (k < text.size() && (text[k] == ':' || text[k] == '\n')) {
        k += 1;
      } else {
        continue;
      }
      while (k < text.size() && is_space(k)) ++k;
      return text.substr(k);
    }
    for (const char* stop : kStops) {
      if (text.compare(i, std::strlen(stop), stop) == 0) return text.substr(start);
    }
    i += static_cast<size_t>(utf8_len(static_cast<unsigned char>(text[i])));
  }
  return text.substr(start);
}

size_t utf8_char_count(const std::string& text) {
  size_t count = 0;
  for (size_t i = 0; i < text.size();) {
    int len = utf8_len(static_cast<unsigned char>(text[i]));
    if (i + static_cast<size_t>(len) > text.size()) len = 1;
    i += static_cast<size_t>(len);
    count += 1;
  }
  return count;
}

bool is_blank_text(const std::string& text) {
  for (char c : text) {
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') return false;
  }
  return true;
}

void fill_callback_event(ApexoAsrEvent* out, const std::string& text, int32_t is_final, int32_t start_ms,
                         int32_t end_ms) {
  std::memset(out, 0, sizeof(*out));
  size_t len = text.size();
  const size_t cap = sizeof(out->text) - 1;
  if (len > cap) {
    len = cap;
    while (len > 0 && (static_cast<unsigned char>(text[len]) & 0xC0) == 0x80) --len;
  }
  std::memcpy(out->text, text.data(), len);
  out->text[len] = '\0';
  out->is_final = is_final;
  out->start_ms = start_ms;
  out->end_ms = end_ms;
}

struct Segment {
  int32_t start_ms;
  int32_t end_ms;
  std::string text;
  bool closed = false;
};

struct Utterance {
  int32_t start_ms = 0;
  int32_t end_ms = 0;
  std::string src_text;
  std::string trans_text;
  bool has_trans = false;
  bool closed_after = false;
};

}  // namespace

struct ApexoSession {
  ApexoEngines engines;
  int32_t merge_gap_ms;
  int32_t summary_chunk_chars;
  int32_t display_lines;
  int32_t display_words;
  std::string model_path;
  int32_t web_summary;
  mutable std::mutex mu;
  mutable std::mutex trans_mu;
  std::string source_lang;
  std::string target_lang;
  ApexoTranscriptCallback transcript_cb = nullptr;
  void* transcript_user = nullptr;
  ApexoTranscriptCallback translation_cb = nullptr;
  void* translation_user = nullptr;
  std::vector<Segment> segments;
  std::vector<Segment> trans_segments;
  std::vector<Utterance> utterances;
  std::string interim;
  int32_t interim_start = 0;
  int32_t interim_end = 0;
  std::string full;
  std::string display;
  std::string full_trans;
  std::string display_trans;
  std::string summary;
  size_t summarized_bytes = 0;
  size_t summarized_utterances = 0;
  uint64_t epoch = 0;
  std::string error;
  std::atomic<int> gemma_resident{0};
  std::atomic<bool> summarizing{false};
  std::atomic<int> refs{1};
};

namespace {

void unref_session(ApexoSession* session) {
  if (session->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete session;
}

struct SessionRef {
  explicit SessionRef(ApexoSession* s) : session(s) { session->refs.fetch_add(1, std::memory_order_relaxed); }
  ~SessionRef() { unref_session(session); }
  ApexoSession* session;
};

}  // namespace

static void refresh_display(ApexoSession* session) {
  session->display.clear();
  const int total = static_cast<int>(session->segments.size());
  int limit = session->display_lines;
  if (!session->interim.empty() && limit > 1) limit -= 1;
  int start = 0;
  if (limit > 0 && total > limit) start = total - limit;
  for (int i = start; i < total; ++i) {
    if (!session->display.empty()) session->display.push_back('\n');
    session->display += session->segments[static_cast<size_t>(i)].text;
  }
  if (!session->interim.empty()) {
    if (!session->display.empty()) session->display.push_back('\n');
    session->display += session->interim;
  }
  session->display = trim_words(session->display, session->display_words);
}

static void rebuild_translation_views(ApexoSession* session) {
  session->full_trans.clear();
  session->trans_segments.clear();
  session->display_trans.clear();
  for (const Utterance& u : session->utterances) {
    if (!u.has_trans || u.trans_text.empty()) {
      if (u.closed_after && !session->trans_segments.empty()) {
        session->trans_segments.back().closed = true;
      }
      continue;
    }
    if (!session->full_trans.empty()) session->full_trans.push_back('\n');
    session->full_trans += u.trans_text;
    bool merged = false;
    if (!session->trans_segments.empty() && !session->trans_segments.back().closed) {
      Segment& last = session->trans_segments.back();
      const int gap = u.start_ms - last.end_ms;
      if (u.start_ms >= last.start_ms && gap < session->merge_gap_ms) {
        append_joined(&last.text, u.trans_text);
        last.end_ms = u.end_ms;
        merged = true;
      }
    }
    if (!merged) {
      Segment seg;
      seg.start_ms = u.start_ms;
      seg.end_ms = u.end_ms;
      seg.text = u.trans_text;
      session->trans_segments.push_back(seg);
    }
    if (u.closed_after && !session->trans_segments.empty()) {
      session->trans_segments.back().closed = true;
    }
  }
  const int total = static_cast<int>(session->trans_segments.size());
  const int limit = session->display_lines;
  int start = 0;
  if (limit > 0 && total > limit) start = total - limit;
  for (int i = start; i < total; ++i) {
    if (!session->display_trans.empty()) session->display_trans.push_back('\n');
    session->display_trans += session->trans_segments[static_cast<size_t>(i)].text;
  }
  session->display_trans = trim_words(session->display_trans, session->display_words);
}

static bool accept_final(ApexoSession* session, const std::string& text, int32_t start_ms, int32_t end_ms,
                         size_t* out_utterance_idx = nullptr) {
  session->interim.clear();
  session->interim_start = 0;
  session->interim_end = 0;
  if (text.empty()) {
    refresh_display(session);
    return false;
  }
  if (!session->segments.empty() && !session->segments.back().closed) {
    const Segment& last = session->segments.back();
    if (start_ms >= last.start_ms && start_ms < last.end_ms && last.text.size() >= text.size() &&
        last.text.compare(last.text.size() - text.size(), text.size(), text) == 0) {
      refresh_display(session);
      return false;
    }
  }
  if (!session->full.empty()) session->full.push_back('\n');
  session->full += text;

  Utterance utt;
  utt.start_ms = start_ms;
  utt.end_ms = end_ms;
  utt.src_text = text;
  session->utterances.push_back(utt);
  if (out_utterance_idx != nullptr) *out_utterance_idx = session->utterances.size() - 1;

  if (!session->segments.empty() && !session->segments.back().closed) {
    Segment& last = session->segments.back();
    const int gap = start_ms - last.end_ms;
    if (start_ms >= last.start_ms && gap < session->merge_gap_ms) {
      append_joined(&last.text, text);
      last.end_ms = end_ms;
      refresh_display(session);
      return true;
    }
  }
  Segment segment;
  segment.start_ms = start_ms;
  segment.end_ms = end_ms;
  segment.text = text;
  session->segments.push_back(segment);
  refresh_display(session);
  return true;
}

static bool accept_event(ApexoSession* session, const ApexoAsrEvent& event, ApexoAsrEvent* cb_out,
                         size_t* out_utterance_idx = nullptr) {
  if (event.is_final < 0) return false;
  const std::string text(event.text, strnlen(event.text, sizeof(event.text)));
  if (event.is_final == 0) {
    session->interim = text;
    session->interim_start = event.start_ms;
    session->interim_end = event.end_ms;
    refresh_display(session);
    if (cb_out != nullptr) fill_callback_event(cb_out, text, 0, event.start_ms, event.end_ms);
    return true;
  }
  const bool accepted = accept_final(session, text, event.start_ms, event.end_ms, out_utterance_idx);
  if (accepted && cb_out != nullptr) fill_callback_event(cb_out, text, 1, event.start_ms, event.end_ms);
  return accepted;
}

static void set_error(ApexoSession* session, const char* message) {
  std::lock_guard<std::mutex> lock(session->mu);
  session->error = message;
}

static int model_path_ok(ApexoSession* session) {
  if (session->web_summary && session->model_path.empty()) {
    session->error = "web summary needs a model name ending in .litertlm";
    return -4;
  }
  if (!session->model_path.empty() && !has_litertlm_suffix(session->model_path)) {
    session->error = "the summary model must be a .litertlm file";
    return -4;
  }
  return 0;
}

/* Translates a newly finalized utterance outside session->mu and updates full_trans / display_trans / callback. */
static void translate_accepted_utterance(ApexoSession* session, size_t utterance_idx, uint64_t start_epoch,
                                         const std::string& src_text, int32_t start_ms, int32_t end_ms) {
  SessionRef ref(session);
  std::string src_lang;
  std::string tgt_lang;
  {
    std::lock_guard<std::mutex> lock(session->mu);
    if (session->epoch != start_epoch || utterance_idx >= session->utterances.size()) return;
    src_lang = session->source_lang;
    tgt_lang = session->target_lang;
  }
  if (tgt_lang.empty()) return;

  std::string translated;
  if (!src_lang.empty() && src_lang == tgt_lang) {
    translated = src_text;
  } else {
    if (session->engines.trans_translate == nullptr) return;
    std::string buf(65536, '\0');
    int rc = 0;
    {
      std::lock_guard<std::mutex> tlock(session->trans_mu);
      rc = session->engines.trans_translate(session->engines.trans_user, src_text.c_str(), &buf[0],
                                            static_cast<int32_t>(buf.size()));
    }
    if (rc != 0) {
      std::lock_guard<std::mutex> lock(session->mu);
      if (session->epoch == start_epoch) {
        session->error = rc == -3 ? "translation model files are missing" : "translation failed";
      }
      return;
    }
    translated.assign(buf.c_str());
  }

  ApexoTranscriptCallback tcb = nullptr;
  void* tcb_user = nullptr;
  ApexoAsrEvent tevent;
  bool should_notify = false;
  {
    std::lock_guard<std::mutex> lock(session->mu);
    if (session->epoch != start_epoch || utterance_idx >= session->utterances.size()) return;
    Utterance& u = session->utterances[utterance_idx];
    u.trans_text = translated;
    u.has_trans = true;
    rebuild_translation_views(session);
    if (!translated.empty() && session->translation_cb != nullptr) {
      fill_callback_event(&tevent, translated, 1, start_ms, end_ms);
      tcb = session->translation_cb;
      tcb_user = session->translation_user;
      should_notify = true;
    }
  }
  if (should_notify && tcb != nullptr) tcb(tcb_user, &tevent);
}

ApexoSession* apexo_session_create(const ApexoConfig* config, const ApexoEngines* engines) {
  if (config == nullptr || engines == nullptr) return nullptr;
  ApexoSession* session = new ApexoSession();
  session->engines = *engines;
  session->merge_gap_ms = config->merge_gap_ms > 0 ? config->merge_gap_ms : APEXO_MERGE_GAP_MS;
  session->summary_chunk_chars =
      config->summary_chunk_chars > 0 ? config->summary_chunk_chars : APEXO_SUMMARY_CHUNK_CHARS;
  session->display_lines = config->display_lines > 0 ? config->display_lines : APEXO_DISPLAY_LINES;
  session->display_words = APEXO_DISPLAY_WORDS;
  session->model_path = config->summary_model_path != nullptr ? config->summary_model_path : "";
  session->web_summary = config->web_summary;
  session->source_lang = normalize_lang(config->source_lang);
  session->target_lang = normalize_lang(config->target_lang);
  return session;
}

void apexo_session_destroy(ApexoSession* session) {
  if (session == nullptr) return;
  unref_session(session);
}

void apexo_session_set_transcript_callback(ApexoSession* session, ApexoTranscriptCallback callback, void* user) {
  if (session == nullptr) return;
  std::lock_guard<std::mutex> lock(session->mu);
  session->transcript_cb = callback;
  session->transcript_user = user;
}

void apexo_session_set_translation_callback(ApexoSession* session, ApexoTranscriptCallback callback, void* user) {
  if (session == nullptr) return;
  std::lock_guard<std::mutex> lock(session->mu);
  session->translation_cb = callback;
  session->translation_user = user;
}

void apexo_session_set_languages(ApexoSession* session, const char* source_lang, const char* target_lang) {
  if (session == nullptr) return;
  std::lock_guard<std::mutex> lock(session->mu);
  session->source_lang = normalize_lang(source_lang);
  session->target_lang = normalize_lang(target_lang);
}

const char* apexo_session_source_lang(const ApexoSession* session) {
  if (session == nullptr) return "";
  std::lock_guard<std::mutex> lock(session->mu);
  return session->source_lang.c_str();
}

const char* apexo_session_target_lang(const ApexoSession* session) {
  if (session == nullptr) return "";
  std::lock_guard<std::mutex> lock(session->mu);
  return session->target_lang.c_str();
}

int apexo_session_push_pcm(ApexoSession* session, const float* pcm, int32_t count, int32_t sample_rate) {
  if (session == nullptr || pcm == nullptr || count < 0) return -1;
  if (sample_rate != APEXO_SAMPLE_RATE) {
    set_error(session, "pcm must be 16000 Hz mono float32");
    return -2;
  }
  if (session->engines.asr_push == nullptr) {
    set_error(session, "recognizer is not attached");
    return -3;
  }
  ApexoTranscriptCallback cb = nullptr;
  void* cb_user = nullptr;
  ApexoAsrEvent cb_event;
  bool notify = false;
  bool accepted_final = false;
  size_t utterance_idx = 0;
  uint64_t start_epoch = 0;
  std::string finalized_text;
  int32_t fin_start = 0;
  int32_t fin_end = 0;
  {
    std::lock_guard<std::mutex> lock(session->mu);
    ApexoAsrEvent event;
    std::memset(&event, 0, sizeof(event));
    event.is_final = -1;
    const int rc = session->engines.asr_push(session->engines.asr_user, pcm, count, sample_rate, &event);
    if (rc != 0) {
      session->error = rc == -3 ? "recognizer model files are missing"
                                : (rc == -5 ? "recognizer failed to load" : "recognizer rejected pcm");
      return rc;
    }
    notify = accept_event(session, event, &cb_event, &utterance_idx);
    if (notify && cb_event.is_final == 1) {
      accepted_final = true;
      start_epoch = session->epoch;
      finalized_text = cb_event.text;
      fin_start = cb_event.start_ms;
      fin_end = cb_event.end_ms;
    }
    cb = session->transcript_cb;
    cb_user = session->transcript_user;
    session->error.clear();
  }
  if (notify && cb != nullptr) cb(cb_user, &cb_event);
  if (accepted_final) {
    translate_accepted_utterance(session, utterance_idx, start_epoch, finalized_text, fin_start, fin_end);
  }
  return 0;
}

int apexo_session_end_segment(ApexoSession* session) {
  if (session == nullptr) return -1;
  ApexoTranscriptCallback cb = nullptr;
  void* cb_user = nullptr;
  ApexoAsrEvent cb_event;
  bool notify = false;
  size_t utterance_idx = 0;
  uint64_t start_epoch = 0;
  std::string finalized_text;
  int32_t fin_start = 0;
  int32_t fin_end = 0;
  {
    std::lock_guard<std::mutex> lock(session->mu);
    if (session->engines.asr_finish != nullptr) {
      ApexoAsrEvent event;
      std::memset(&event, 0, sizeof(event));
      event.is_final = -1;
      const int rc = session->engines.asr_finish(session->engines.asr_user, &event);
      if (rc != 0) {
        session->error = rc == -3 ? "recognizer model files are missing" : "recognizer failed to finish segment";
        return rc;
      }
      if (event.is_final == 1) {
        const std::string text(event.text, strnlen(event.text, sizeof(event.text)));
        if (!text.empty()) {
          if (accept_final(session, text, event.start_ms, event.end_ms, &utterance_idx)) {
            fill_callback_event(&cb_event, text, 1, event.start_ms, event.end_ms);
            notify = true;
            start_epoch = session->epoch;
            finalized_text = text;
            fin_start = event.start_ms;
            fin_end = event.end_ms;
          }
        } else {
          session->interim.clear();
          session->interim_start = 0;
          session->interim_end = 0;
          refresh_display(session);
        }
      } else {
        session->interim.clear();
        session->interim_start = 0;
        session->interim_end = 0;
        refresh_display(session);
      }
      if (!session->segments.empty()) session->segments.back().closed = true;
      if (!session->utterances.empty()) {
        session->utterances.back().closed_after = true;
        rebuild_translation_views(session);
      }
      session->error.clear();
    } else {
      if (!session->interim.empty()) {
        const std::string text = session->interim;
        const int32_t s_ms = session->interim_start;
        const int32_t e_ms = session->interim_end;
        if (accept_final(session, text, s_ms, e_ms, &utterance_idx)) {
          fill_callback_event(&cb_event, text, 1, s_ms, e_ms);
          notify = true;
          start_epoch = session->epoch;
          finalized_text = text;
          fin_start = s_ms;
          fin_end = e_ms;
        }
      }
      if (!session->segments.empty()) session->segments.back().closed = true;
      if (!session->utterances.empty()) {
        session->utterances.back().closed_after = true;
        rebuild_translation_views(session);
      }
      session->interim.clear();
      refresh_display(session);
      if (session->engines.asr_reset != nullptr) {
        session->engines.asr_reset(session->engines.asr_user);
      }
      session->error.clear();
    }
    cb = session->transcript_cb;
    cb_user = session->transcript_user;
  }
  if (notify && cb != nullptr) cb(cb_user, &cb_event);
  if (notify) {
    translate_accepted_utterance(session, utterance_idx, start_epoch, finalized_text, fin_start, fin_end);
  }
  return 0;
}

int apexo_session_gemma_resident(const ApexoSession* session) {
  if (session == nullptr) return 0;
  return session->gemma_resident.load();
}

const char* apexo_session_display_text(const ApexoSession* session) {
  if (session == nullptr) return "";
  std::lock_guard<std::mutex> lock(session->mu);
  return session->display.c_str();
}

const char* apexo_session_full_transcript(const ApexoSession* session) {
  if (session == nullptr) return "";
  std::lock_guard<std::mutex> lock(session->mu);
  return session->full.c_str();
}

const char* apexo_session_translated_display_text(const ApexoSession* session) {
  if (session == nullptr) return "";
  std::lock_guard<std::mutex> lock(session->mu);
  return session->display_trans.c_str();
}

const char* apexo_session_full_translation(const ApexoSession* session) {
  if (session == nullptr) return "";
  std::lock_guard<std::mutex> lock(session->mu);
  return session->full_trans.c_str();
}

const char* apexo_session_summary_text(const ApexoSession* session) {
  if (session == nullptr) return "";
  std::lock_guard<std::mutex> lock(session->mu);
  return session->summary.c_str();
}

namespace {

struct SummarizeGuard {
  explicit SummarizeGuard(ApexoSession* s) : session(s) {}
  ~SummarizeGuard() { session->summarizing.store(false); }
  ApexoSession* session;
};

int copy_out(ApexoSession* session, const std::string& text, char* out, int32_t out_cap) {
  if (static_cast<int32_t>(text.size()) >= out_cap) {
    session->error = "summary buffer is too small";
    return -1;
  }
  std::memcpy(out, text.c_str(), text.size() + 1);
  return 0;
}

void unload_summarizer(ApexoSession* session) {
  if (session->engines.sum_unload != nullptr) session->engines.sum_unload(session->engines.sum_user);
  session->gemma_resident.store(0);
}

int generate_one(ApexoSession* session, const std::string& prompt, std::string* out_text) {
  std::string piece(65536, '\0');
  const int gen = session->engines.sum_generate(session->engines.sum_user, prompt.c_str(), &piece[0],
                                                static_cast<int32_t>(piece.size()));
  if (gen != 0) return gen;
  *out_text = strip_label(piece.c_str());
  return 0;
}

int run_summary_generation(ApexoSession* session, const std::string& text, bool synthesize,
                           const std::string& target_lang, std::string* out_piece) {
  session->gemma_resident.store(1);
  const int load_rc = session->engines.sum_load(session->engines.sum_user, session->model_path.c_str());
  if (load_rc != 0) {
    unload_summarizer(session);
    if (load_rc == -3) {
      set_error(session, "summary model file was not found");
      return -3;
    }
    set_error(session, "gemma failed to load");
    return -5;
  }
  const std::string chunk_inst = chunk_instruction_for(target_lang);
  const std::string synth_inst = synthesize_instruction_for(target_lang);
  const std::vector<std::string> work = chunk_text(text, session->summary_chunk_chars);
  out_piece->clear();
  if (!synthesize) {
    for (size_t i = 0; i < work.size(); ++i) {
      std::string piece;
      const int gen = generate_one(session, chunk_inst + work[i], &piece);
      if (gen != 0) {
        unload_summarizer(session);
        set_error(session, "gemma generation failed");
        return gen;
      }
      if (!out_piece->empty() && !piece.empty()) out_piece->push_back('\n');
      out_piece->append(piece);
    }
    unload_summarizer(session);
    return 0;
  }

  if (work.size() == 1) {
    const int gen = generate_one(session, synth_inst + work[0], out_piece);
    if (gen != 0) {
      unload_summarizer(session);
      set_error(session, "gemma generation failed");
      return gen;
    }
    unload_summarizer(session);
    return 0;
  }

  std::string ordered_notes;
  for (size_t i = 0; i < work.size(); ++i) {
    std::string note;
    const int gen = generate_one(session, chunk_inst + work[i], &note);
    if (gen != 0) {
      unload_summarizer(session);
      set_error(session, "gemma generation failed");
      return gen;
    }
    if (!ordered_notes.empty() && !note.empty()) ordered_notes.push_back('\n');
    ordered_notes.append(note);
  }

  while (chunk_text(ordered_notes, session->summary_chunk_chars).size() > 1) {
    const size_t prev_chars = utf8_char_count(ordered_notes);
    const std::vector<std::string> groups = chunk_text(ordered_notes, session->summary_chunk_chars);
    std::string reduced;
    for (size_t i = 0; i < groups.size(); ++i) {
      std::string note;
      const int gen = generate_one(session, synth_inst + groups[i], &note);
      if (gen != 0) {
        unload_summarizer(session);
        set_error(session, "gemma generation failed");
        return gen;
      }
      if (!reduced.empty() && !note.empty()) reduced.push_back('\n');
      reduced.append(note);
    }
    const size_t next_chars = utf8_char_count(reduced);
    if (next_chars >= prev_chars) {
      unload_summarizer(session);
      set_error(session, "gemma summary reduction did not converge");
      return -5;
    }
    ordered_notes = reduced;
  }

  const int final_gen = generate_one(session, synth_inst + ordered_notes, out_piece);
  if (final_gen != 0) {
    unload_summarizer(session);
    set_error(session, "gemma generation failed");
    return final_gen;
  }
  unload_summarizer(session);
  return 0;
}

/* Prepares summary input from a snapshot of utterances [from_idx, to_idx). When cross-language translation is
 * active, uses already-translated B-language lines (and lazily translates any untranslated lines if a translator
 * is attached), falling back to source lines so multilingual Gemma can still summarize in target_lang. */
std::string build_summary_material(ApexoSession* session, const std::vector<Utterance>& snapshot, size_t from_idx,
                                   size_t to_idx, uint64_t start_epoch, const std::string& src_lang,
                                   const std::string& tgt_lang) {
  const bool want_trans = !tgt_lang.empty() && (src_lang.empty() || src_lang != tgt_lang);
  std::string combined;
  for (size_t i = from_idx; i < to_idx && i < snapshot.size(); ++i) {
    std::string line = snapshot[i].src_text;
    if (want_trans) {
      if (snapshot[i].has_trans && !snapshot[i].trans_text.empty()) {
        line = snapshot[i].trans_text;
      } else if (session->engines.trans_translate != nullptr && !snapshot[i].src_text.empty()) {
        std::string buf(65536, '\0');
        int trc = 0;
        {
          std::lock_guard<std::mutex> tlock(session->trans_mu);
          trc = session->engines.trans_translate(session->engines.trans_user, snapshot[i].src_text.c_str(), &buf[0],
                                                 static_cast<int32_t>(buf.size()));
        }
        if (trc == 0 && buf[0] != '\0') {
          line.assign(buf.c_str());
          std::lock_guard<std::mutex> lock(session->mu);
          if (session->epoch == start_epoch && i < session->utterances.size() && !session->utterances[i].has_trans) {
            session->utterances[i].trans_text = line;
            session->utterances[i].has_trans = true;
            rebuild_translation_views(session);
          }
        }
      }
    }
    if (!line.empty()) {
      if (!combined.empty()) combined.push_back('\n');
      combined += line;
    }
  }
  return combined;
}

}  // namespace

int apexo_session_translate_text(ApexoSession* session, const char* text, char* out, int32_t out_cap) {
  if (session == nullptr || text == nullptr || out == nullptr || out_cap < 1) {
    if (session != nullptr) set_error(session, "invalid translate_text arguments");
    return -1;
  }
  out[0] = '\0';
  const std::string input_copy(text);
  SessionRef ref(session);
  std::string src_lang;
  std::string tgt_lang;
  {
    std::lock_guard<std::mutex> lock(session->mu);
    src_lang = session->source_lang;
    tgt_lang = session->target_lang;
    if (is_blank_text(input_copy)) {
      session->error.clear();
      return 0;
    }
    if (!src_lang.empty() && src_lang == tgt_lang) {
      if (static_cast<int32_t>(input_copy.size()) >= out_cap) {
        session->error = "translation buffer is too small";
        return -1;
      }
      std::memcpy(out, input_copy.c_str(), input_copy.size() + 1);
      session->error.clear();
      return 0;
    }
    if (session->engines.trans_translate == nullptr) {
      session->error = "translator is not attached";
      return -3;
    }
  }
  std::string buf(65536, '\0');
  int rc = 0;
  {
    std::lock_guard<std::mutex> tlock(session->trans_mu);
    rc = session->engines.trans_translate(session->engines.trans_user, input_copy.c_str(), &buf[0],
                                          static_cast<int32_t>(buf.size()));
  }
  std::lock_guard<std::mutex> lock(session->mu);
  if (rc != 0) {
    session->error = rc == -3 ? "translation model files are missing" : "translation failed";
    return rc;
  }
  const std::string result(buf.c_str());
  if (static_cast<int32_t>(result.size()) >= out_cap) {
    session->error = "translation buffer is too small";
    return -1;
  }
  std::memcpy(out, result.c_str(), result.size() + 1);
  session->error.clear();
  return 0;
}

int apexo_session_summarize(ApexoSession* session, char* out, int32_t out_cap) {
  if (session == nullptr || out == nullptr || out_cap < 1) return -1;
  out[0] = '\0';
  SessionRef ref(session);
  if (session->summarizing.exchange(true)) {
    set_error(session, "a summary is already running");
    return APEXO_ERR_BUSY;
  }
  SummarizeGuard guard(session);
  std::string raw_full;
  std::vector<Utterance> snapshot;
  std::string src_lang;
  std::string tgt_lang;
  uint64_t start_epoch = 0;
  {
    std::lock_guard<std::mutex> lock(session->mu);
    const int path_rc = model_path_ok(session);
    if (path_rc != 0) return path_rc;
    if (session->engines.sum_load == nullptr || session->engines.sum_generate == nullptr) {
      session->error = "summarizer is not attached";
      return -3;
    }
    raw_full = session->full;
    snapshot = session->utterances;
    src_lang = session->source_lang;
    tgt_lang = session->target_lang;
    start_epoch = session->epoch;
    if (is_blank_text(raw_full)) {
      session->summary.clear();
      session->summarized_bytes = 0;
      session->summarized_utterances = 0;
      session->error.clear();
      return 0;
    }
  }

  const std::string material =
      snapshot.empty() ? raw_full
                       : build_summary_material(session, snapshot, 0, snapshot.size(), start_epoch, src_lang, tgt_lang);
  std::string generated;
  const int rc = run_summary_generation(session, material, true, tgt_lang, &generated);
  if (rc != 0) return rc;
  std::lock_guard<std::mutex> lock(session->mu);
  if (session->epoch != start_epoch) {
    session->error.clear();
    return 0;
  }
  const int copy_rc = copy_out(session, generated, out, out_cap);
  if (copy_rc != 0) return copy_rc;
  session->summary = generated;
  session->summarized_bytes = raw_full.size();
  session->summarized_utterances = snapshot.size();
  session->error.clear();
  return 0;
}

int apexo_session_summarize_text(ApexoSession* session, const char* text, char* out, int32_t out_cap) {
  if (session == nullptr || text == nullptr || out == nullptr || out_cap < 1) {
    if (session != nullptr) set_error(session, "invalid summarize_text arguments");
    return -1;
  }
  out[0] = '\0';
  const std::string input_copy(text);
  SessionRef ref(session);
  if (session->summarizing.exchange(true)) {
    set_error(session, "a summary is already running");
    return APEXO_ERR_BUSY;
  }
  SummarizeGuard guard(session);
  uint64_t start_epoch = 0;
  std::string tgt_lang;
  {
    std::lock_guard<std::mutex> lock(session->mu);
    const int path_rc = model_path_ok(session);
    if (path_rc != 0) return path_rc;
    if (session->engines.sum_load == nullptr || session->engines.sum_generate == nullptr) {
      session->error = "summarizer is not attached";
      return -3;
    }
    start_epoch = session->epoch;
    tgt_lang = session->target_lang;
    if (is_blank_text(input_copy)) {
      session->error.clear();
      return 0;
    }
  }

  std::string generated;
  const int rc = run_summary_generation(session, input_copy, true, tgt_lang, &generated);
  if (rc != 0) return rc;
  std::lock_guard<std::mutex> lock(session->mu);
  if (session->epoch != start_epoch) {
    session->error.clear();
    return 0;
  }
  const int copy_rc = copy_out(session, generated, out, out_cap);
  if (copy_rc != 0) return copy_rc;
  session->error.clear();
  return 0;
}

int apexo_session_summarize_new(ApexoSession* session, char* out, int32_t out_cap) {
  if (session == nullptr || out == nullptr || out_cap < 1) return -1;
  out[0] = '\0';
  SessionRef ref(session);
  if (session->summarizing.exchange(true)) {
    set_error(session, "a summary is already running");
    return APEXO_ERR_BUSY;
  }
  SummarizeGuard guard(session);
  std::vector<Utterance> snapshot;
  size_t from_utt = 0;
  size_t target_utts = 0;
  size_t target_bytes = 0;
  std::string src_lang;
  std::string tgt_lang;
  uint64_t start_epoch = 0;
  {
    std::lock_guard<std::mutex> lock(session->mu);
    const int path_rc = model_path_ok(session);
    if (path_rc != 0) return path_rc;
    if (session->engines.sum_load == nullptr || session->engines.sum_generate == nullptr) {
      session->error = "summarizer is not attached";
      return -3;
    }
    if (session->summarized_utterances >= session->utterances.size()) {
      const int copy_rc = copy_out(session, session->summary, out, out_cap);
      if (copy_rc == 0) session->error.clear();
      return copy_rc;
    }
    snapshot = session->utterances;
    from_utt = session->summarized_utterances;
    target_utts = snapshot.size();
    target_bytes = session->full.size();
    src_lang = session->source_lang;
    tgt_lang = session->target_lang;
    start_epoch = session->epoch;
  }

  const std::string delta = build_summary_material(session, snapshot, from_utt, target_utts, start_epoch, src_lang, tgt_lang);
  if (is_blank_text(delta)) {
    std::lock_guard<std::mutex> lock(session->mu);
    if (session->epoch != start_epoch) {
      session->error.clear();
      return 0;
    }
    const int copy_rc = copy_out(session, session->summary, out, out_cap);
    if (copy_rc == 0) {
      session->summarized_bytes = target_bytes;
      session->summarized_utterances = target_utts;
      session->error.clear();
    }
    return copy_rc;
  }

  std::string piece;
  const int rc = run_summary_generation(session, delta, false, tgt_lang, &piece);
  if (rc != 0) return rc;
  std::lock_guard<std::mutex> lock(session->mu);
  if (session->epoch != start_epoch || session->utterances.size() < target_utts) {
    session->error.clear();
    return 0;
  }
  std::string combined = session->summary;
  if (!combined.empty() && !piece.empty()) combined.push_back('\n');
  combined += piece;
  const int copy_rc = copy_out(session, combined, out, out_cap);
  if (copy_rc != 0) return copy_rc;
  session->summary = combined;
  session->summarized_bytes = target_bytes;
  session->summarized_utterances = target_utts;
  session->error.clear();
  return 0;
}

void apexo_session_reset(ApexoSession* session) {
  if (session == nullptr) return;
  std::lock_guard<std::mutex> lock(session->mu);
  if (!session->summarizing.load()) {
    if (session->gemma_resident.load() && session->engines.sum_unload != nullptr) {
      session->engines.sum_unload(session->engines.sum_user);
    }
    session->gemma_resident.store(0);
  }
  if (session->engines.asr_reset != nullptr) session->engines.asr_reset(session->engines.asr_user);
  session->epoch += 1;
  session->segments.clear();
  session->trans_segments.clear();
  session->utterances.clear();
  session->interim.clear();
  session->interim_start = 0;
  session->interim_end = 0;
  session->full.clear();
  session->display.clear();
  session->full_trans.clear();
  session->display_trans.clear();
  session->summary.clear();
  session->summarized_bytes = 0;
  session->summarized_utterances = 0;
  session->error.clear();
}

void apexo_session_release(ApexoSession* session) { apexo_session_reset(session); }

const char* apexo_session_last_error(const ApexoSession* session) {
  if (session == nullptr) return "";
  std::lock_guard<std::mutex> lock(session->mu);
  return session->error.c_str();
}
