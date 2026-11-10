#include "apexo_session.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

const char* kInstruction =
    "只根据下面的转写写摘要。转写里没有的内容不要补。\n转写：\n";

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

bool is_cjk(uint32_t cp) { return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF); }

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

std::vector<std::string> chunk_text(const std::string& text, int max_chars) {
  std::vector<std::string> parts;
  if (max_chars < 1) max_chars = 1;
  std::string current;
  int chars = 0;
  for (size_t i = 0; i < text.size();) {
    int len = utf8_len(static_cast<unsigned char>(text[i]));
    if (i + static_cast<size_t>(len) > text.size()) len = 1;
    if (chars >= max_chars) {
      parts.push_back(current);
      current.clear();
      chars = 0;
    }
    current.append(text, i, static_cast<size_t>(len));
    chars += 1;
    i += static_cast<size_t>(len);
  }
  if (!current.empty()) parts.push_back(current);
  return parts;
}

bool is_web_gemma_file(const std::string& path) {
  return path.find("-web") != std::string::npos;
}

struct Segment {
  int32_t start_ms;
  int32_t end_ms;
  std::string text;
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
  std::vector<Segment> segments;
  std::string interim;
  std::string full;
  std::string display;
  std::string summary;
  std::string error;
  int gemma_resident;
  int reset_calls;
};

static void refresh_display(ApexoSession* session) {
  session->display.clear();
  const int total = static_cast<int>(session->segments.size());
  int start = 0;
  if (session->display_lines > 0 && total > session->display_lines) {
    start = total - session->display_lines;
  }
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

static void accept_event(ApexoSession* session, const ApexoAsrEvent& event) {
  if (event.is_final < 0) return;
  if (event.is_final == 0) {
    session->interim = event.text;
    refresh_display(session);
    return;
  }
  session->interim.clear();
  if (!session->segments.empty() && event.text[0] != '\0') {
    const std::string& last = session->segments.back().text;
    const std::string incoming = event.text;
    if (!incoming.empty() && last.size() >= incoming.size() &&
        last.compare(last.size() - incoming.size(), incoming.size(), incoming) == 0) {
      refresh_display(session);
      return;
    }
  }
  if (!session->full.empty()) session->full.push_back('\n');
  session->full += event.text;
  if (!session->segments.empty()) {
    const Segment& last = session->segments.back();
    const int gap = event.start_ms - last.end_ms;
    if (gap < session->merge_gap_ms) {
      session->segments.back().text += event.text;
      session->segments.back().end_ms = event.end_ms;
      refresh_display(session);
      return;
    }
  }
  Segment segment;
  segment.start_ms = event.start_ms;
  segment.end_ms = event.end_ms;
  segment.text = event.text;
  session->segments.push_back(segment);
  refresh_display(session);
}

static int model_path_ok(ApexoSession* session) {
  const bool web_file = is_web_gemma_file(session->model_path);
  if (session->web_summary) {
    if (session->model_path.empty() || !web_file) {
      session->error = "web summary requires a web Gemma file such as gemma-4-E2B-it-web.litertlm";
      return -4;
    }
  } else if (web_file) {
    session->error = "native summary requires a non-web Gemma 4 E2B .litertlm";
    return -4;
  }
  return 0;
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
  session->gemma_resident = 0;
  session->reset_calls = 0;
  return session;
}

void apexo_session_destroy(ApexoSession* session) { delete session; }

int apexo_session_push_pcm(ApexoSession* session, const float* pcm, int32_t count, int32_t sample_rate) {
  if (session == nullptr || pcm == nullptr || count < 0) return -1;
  if (sample_rate != APEXO_SAMPLE_RATE) {
    session->error = "pcm must be 16000 Hz mono float32";
    return -2;
  }
  if (session->engines.asr_push == nullptr) {
    session->error = "recognizer is not attached";
    return -3;
  }
  if (session->gemma_resident) {
    session->error = "gemma is resident; live transcription does not run beside it";
    return -5;
  }
  ApexoAsrEvent event;
  std::memset(&event, 0, sizeof(event));
  event.is_final = -1;
  const int rc = session->engines.asr_push(session->engines.asr_user, pcm, count, sample_rate, &event);
  if (rc != 0) {
    session->error = "recognizer rejected pcm";
    return rc;
  }
  accept_event(session, event);
  return 0;
}

int apexo_session_end_segment(ApexoSession* session) {
  if (session == nullptr) return -1;
  session->interim.clear();
  refresh_display(session);
  if (session->engines.asr_reset != nullptr) {
    session->engines.asr_reset(session->engines.asr_user);
    session->reset_calls += 1;
  }
  return 0;
}

int apexo_session_gemma_resident(const ApexoSession* session) {
  if (session == nullptr) return 0;
  return session->gemma_resident;
}

const char* apexo_session_display_text(const ApexoSession* session) {
  if (session == nullptr) return "";
  return session->display.c_str();
}

const char* apexo_session_full_transcript(const ApexoSession* session) {
  if (session == nullptr) return "";
  return session->full.c_str();
}

int apexo_session_summarize(ApexoSession* session, char* out, int32_t out_cap) {
  if (session == nullptr || out == nullptr || out_cap < 1) return -1;
  out[0] = '\0';
  const int path_rc = model_path_ok(session);
  if (path_rc != 0) return path_rc;
  if (session->engines.sum_load == nullptr || session->engines.sum_generate == nullptr) {
    session->error = "summarizer is not attached";
    return -3;
  }
  if (session->engines.sum_load(session->engines.sum_user, session->model_path.c_str()) != 0) {
    session->error = "gemma failed to load";
    return -5;
  }
  session->gemma_resident = 1;
  const std::vector<std::string> chunks = chunk_text(session->full, session->summary_chunk_chars);
  const std::vector<std::string> work = chunks.empty() ? std::vector<std::string>{""} : chunks;
  session->summary.clear();
  for (size_t i = 0; i < work.size(); ++i) {
    const std::string prompt = std::string(kInstruction) + work[i];
    std::string piece(8192, '\0');
    const int gen = session->engines.sum_generate(session->engines.sum_user, prompt.c_str(), piece.data(),
                                                   static_cast<int32_t>(piece.size()));
    if (gen != 0) {
      if (session->engines.sum_unload != nullptr) session->engines.sum_unload(session->engines.sum_user);
      session->gemma_resident = 0;
      session->error = "gemma generation failed";
      return gen;
    }
    piece.resize(std::strlen(piece.c_str()));
    if (!session->summary.empty()) session->summary.push_back('\n');
    session->summary += piece;
  }
  if (session->engines.sum_unload != nullptr) session->engines.sum_unload(session->engines.sum_user);
  session->gemma_resident = 0;
  if (static_cast<int32_t>(session->summary.size()) >= out_cap) {
    session->error = "summary buffer is too small";
    return -1;
  }
  std::memcpy(out, session->summary.c_str(), session->summary.size() + 1);
  return 0;
}

void apexo_session_release(ApexoSession* session) {
  if (session == nullptr) return;
  if (session->gemma_resident && session->engines.sum_unload != nullptr) {
    session->engines.sum_unload(session->engines.sum_user);
  }
  session->gemma_resident = 0;
  if (session->engines.asr_reset != nullptr) session->engines.asr_reset(session->engines.asr_user);
  session->segments.clear();
  session->interim.clear();
  session->full.clear();
  session->display.clear();
  session->summary.clear();
}

const char* apexo_session_last_error(const ApexoSession* session) {
  if (session == nullptr) return "";
  return session->error.c_str();
}
