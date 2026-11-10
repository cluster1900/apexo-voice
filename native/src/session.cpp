#include "apexo_session.h"

#include <atomic>
#include <cstring>
#include <mutex>
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
  size_t break_bytes = 0;  // bytes of `current` up to and including the last break character
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

bool is_ascii_wordy(const std::string& text, bool at_end) {
  if (text.empty()) return false;
  const unsigned char c = static_cast<unsigned char>(at_end ? text.back() : text.front());
  return c < 0x80 && c != ' ' && c != '\n' && c != '\t' && c != '\r';
}

/* Joins two finals shown on one line. Non-CJK words get a space between them; CJK text is joined directly. */
void append_joined(std::string* line, const std::string& next) {
  if (is_ascii_wordy(*line, true) && is_ascii_wordy(next, false)) line->push_back(' ');
  line->append(next);
}

/* A summary model is a LiteRT-LM bundle. Which build it is (CPU or WebGPU) is the host's choice and is not
 * guessed from the file name. */
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

/* Gemma often opens with its own heading ("摘要：", "转写内容摘要：", "**总结**:", "Summary:"). The session lays
 * summaries out itself, so such a leading label is dropped. A label is at most a few words ending in 摘要/总结/
 * Summary, followed by a colon or a line break, so "总结一下……" and ordinary sentences are kept. */
std::string strip_label(const std::string& text) {
  size_t i = 0;
  auto is_space = [&](size_t k) {
    return text[k] == ' ' || text[k] == '\n' || text[k] == '\r' || text[k] == '\t';
  };
  while (i < text.size() && is_space(i)) ++i;
  const size_t start = i;
  while (i < text.size() && (text[i] == '#' || text[i] == '*' || text[i] == ' ')) ++i;
  static const char* const kLabels[] = {"摘要", "总结", "Summary", "summary", "SUMMARY"};
  static const char* const kStops[] = {"\n", ":", "\xEF\xBC\x9A" /* ： */, "\xEF\xBC\x8C" /* ， */,
                                       "\xE3\x80\x82" /* 。 */, ",", "."};
  /* Up to 8 characters may come before the label word, none of them punctuation that would end a heading. */
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

struct Segment {
  int32_t start_ms;
  int32_t end_ms;
  std::string text;
  bool closed = false;
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
  /* mu guards everything below except the two atomics. Engine calls for transcription run under it, so
   * concurrent callers are serialized. summarize() does not hold it while Gemma runs. */
  mutable std::mutex mu;
  std::vector<Segment> segments;
  std::string interim;
  int32_t interim_start = 0;
  int32_t interim_end = 0;
  std::string full;
  std::string display;
  std::string summary;
  size_t summarized_bytes = 0;
  uint64_t epoch = 0;
  std::string error;
  std::atomic<int> gemma_resident{0};
  std::atomic<bool> summarizing{false};
  /* One reference for the host plus one per running summary. Whoever drops the last one frees the session, so
   * destroy() during a summary never frees memory that summary is still using. */
  std::atomic<int> refs{1};
  int reset_calls = 0;
};

static void refresh_display(ApexoSession* session) {
  session->display.clear();
  const int total = static_cast<int>(session->segments.size());
  /* The unfinished line counts toward the line limit. */
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

static void accept_final(ApexoSession* session, const std::string& text, int32_t start_ms, int32_t end_ms) {
  session->interim.clear();
  session->interim_start = 0;
  session->interim_end = 0;
  /* A final with no words (for example an endpoint fired by silence) carries nothing to keep. */
  if (text.empty()) {
    refresh_display(session);
    return;
  }
  /* A repeat is only dropped when the audio really overlaps the previous final (the recognizer reported the same
   * stretch of speech twice). Two utterances that merely sit close together are both kept, even when they read
   * the same. */
  if (!session->segments.empty() && !session->segments.back().closed) {
    const Segment& last = session->segments.back();
    if (start_ms >= last.start_ms && start_ms < last.end_ms && last.text.size() >= text.size() &&
        last.text.compare(last.text.size() - text.size(), text.size(), text) == 0) {
      refresh_display(session);
      return;
    }
  }
  if (!session->full.empty()) session->full.push_back('\n');
  session->full += text;
  if (!session->segments.empty() && !session->segments.back().closed) {
    Segment& last = session->segments.back();
    const int gap = start_ms - last.end_ms;
    if (start_ms >= last.start_ms && gap < session->merge_gap_ms) {
      append_joined(&last.text, text);
      last.end_ms = end_ms;
      refresh_display(session);
      return;
    }
  }
  Segment segment;
  segment.start_ms = start_ms;
  segment.end_ms = end_ms;
  segment.text = text;
  session->segments.push_back(segment);
  refresh_display(session);
}

static void accept_event(ApexoSession* session, const ApexoAsrEvent& event) {
  if (event.is_final < 0) return;
  const std::string text(event.text, strnlen(event.text, sizeof(event.text)));
  if (event.is_final == 0) {
    session->interim = text;
    session->interim_start = event.start_ms;
    session->interim_end = event.end_ms;
    refresh_display(session);
    return;
  }
  accept_final(session, text, event.start_ms, event.end_ms);
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
  return session;
}

namespace {

void unref_session(ApexoSession* session) {
  if (session->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete session;
}

}  // namespace

/* A summary running on another thread keeps the session alive until it returns. The host must keep the engines
 * alive until then too, and must not start new calls on the session after destroy. */
void apexo_session_destroy(ApexoSession* session) {
  if (session == nullptr) return;
  unref_session(session);
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
  std::lock_guard<std::mutex> lock(session->mu);
  if (session->gemma_resident.load()) {
    session->error = "gemma is resident; live transcription does not run beside it";
    return -5;
  }
  ApexoAsrEvent event;
  std::memset(&event, 0, sizeof(event));
  event.is_final = -1;
  const int rc = session->engines.asr_push(session->engines.asr_user, pcm, count, sample_rate, &event);
  if (rc != 0) {
    session->error = rc == -3 ? "recognizer model files are missing" : "recognizer rejected pcm";
    return rc;
  }
  accept_event(session, event);
  return 0;
}

/* Caller holds mu. A line that never got its final is kept: it becomes final before the recognizer is reset. */
static void end_segment_locked(ApexoSession* session) {
  if (!session->interim.empty()) {
    const std::string text = session->interim;
    accept_final(session, text, session->interim_start, session->interim_end);
  }
  if (!session->segments.empty()) session->segments.back().closed = true;
  session->interim.clear();
  refresh_display(session);
  if (session->engines.asr_reset != nullptr) {
    session->engines.asr_reset(session->engines.asr_user);
    session->reset_calls += 1;
  }
}

int apexo_session_end_segment(ApexoSession* session) {
  if (session == nullptr) return -1;
  std::lock_guard<std::mutex> lock(session->mu);
  end_segment_locked(session);
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

const char* apexo_session_summary_text(const ApexoSession* session) {
  if (session == nullptr) return "";
  std::lock_guard<std::mutex> lock(session->mu);
  return session->summary.c_str();
}

namespace {

/* Holds a reference for the whole call. Constructed first in summarize, so it is released last. */
struct SessionRef {
  explicit SessionRef(ApexoSession* s) : session(s) { session->refs.fetch_add(1, std::memory_order_relaxed); }
  ~SessionRef() { unref_session(session); }
  ApexoSession* session;
};

struct SummarizeGuard {
  explicit SummarizeGuard(ApexoSession* s) : session(s) {}
  ~SummarizeGuard() { session->summarizing.store(false); }
  ApexoSession* session;
};

void unload_summarizer(ApexoSession* session) {
  if (session->engines.sum_unload != nullptr) session->engines.sum_unload(session->engines.sum_user);
  session->gemma_resident.store(0);
}

int generate_chunks(ApexoSession* session, const std::string& text, std::string* out_piece) {
  const int load_rc = session->engines.sum_load(session->engines.sum_user, session->model_path.c_str());
  if (load_rc != 0) {
    unload_summarizer(session);  // a half-built engine must not stay in memory
    if (load_rc == -3) {
      set_error(session, "summary model file was not found");
      return -3;
    }
    set_error(session, "gemma failed to load");
    return -5;
  }
  const std::vector<std::string> work = chunk_text(text, session->summary_chunk_chars);
  out_piece->clear();
  std::string piece(65536, '\0');
  for (size_t i = 0; i < work.size(); ++i) {
    const std::string prompt = std::string(kInstruction) + work[i];
    piece[0] = '\0';
    const int gen = session->engines.sum_generate(session->engines.sum_user, prompt.c_str(), &piece[0],
                                                  static_cast<int32_t>(piece.size()));
    if (gen != 0) {
      unload_summarizer(session);
      set_error(session, "gemma generation failed");
      return gen;
    }
    if (!out_piece->empty()) out_piece->push_back('\n');
    out_piece->append(strip_label(piece.c_str()));
  }
  unload_summarizer(session);
  return 0;
}

int copy_out(ApexoSession* session, const std::string& text, char* out, int32_t out_cap) {
  if (static_cast<int32_t>(text.size()) >= out_cap) {
    session->error = "summary buffer is too small";
    return -1;
  }
  std::memcpy(out, text.c_str(), text.size() + 1);
  return 0;
}

}  // namespace

int apexo_session_summarize(ApexoSession* session, char* out, int32_t out_cap) {
  if (session == nullptr || out == nullptr || out_cap < 1) return -1;
  out[0] = '\0';
  SessionRef ref(session);
  if (session->summarizing.exchange(true)) {
    set_error(session, "a summary is already running");
    return -5;
  }
  SummarizeGuard guard(session);
  std::string transcript;
  uint64_t start_epoch = 0;
  {
    std::lock_guard<std::mutex> lock(session->mu);
    const int path_rc = model_path_ok(session);
    if (path_rc != 0) return path_rc;
    if (session->engines.sum_load == nullptr || session->engines.sum_generate == nullptr) {
      session->error = "summarizer is not attached";
      return -3;
    }
    if (!session->interim.empty()) {
      const std::string text = session->interim;
      accept_final(session, text, session->interim_start, session->interim_end);
    }
    transcript = session->full;
    start_epoch = session->epoch;
    session->summary.clear();
    session->summarized_bytes = 0;
    /* Nothing was said: do not load the model just to have it write a summary of nothing. */
    if (transcript.empty()) return 0;
    /* Resident from the start of the load (under mu, so an in-flight push_pcm finishes first). */
    session->gemma_resident.store(1);
  }

  std::string summary;
  const int rc = generate_chunks(session, transcript, &summary);
  if (rc != 0) return rc;
  std::lock_guard<std::mutex> lock(session->mu);
  if (session->epoch == start_epoch) {
    session->summary = summary;
    session->summarized_bytes = transcript.size();
  }
  return copy_out(session, session->summary, out, out_cap);
}

int apexo_session_summarize_new(ApexoSession* session, char* out, int32_t out_cap) {
  if (session == nullptr || out == nullptr || out_cap < 1) return -1;
  out[0] = '\0';
  SessionRef ref(session);
  if (session->summarizing.exchange(true)) {
    set_error(session, "a summary is already running");
    return -5;
  }
  SummarizeGuard guard(session);
  std::string delta;
  size_t target_bytes = 0;
  uint64_t start_epoch = 0;
  {
    std::lock_guard<std::mutex> lock(session->mu);
    const int path_rc = model_path_ok(session);
    if (path_rc != 0) return path_rc;
    if (session->engines.sum_load == nullptr || session->engines.sum_generate == nullptr) {
      session->error = "summarizer is not attached";
      return -3;
    }
    /* Speech still being recognized is included: it is closed as a line here and the recognizer starts over, so
     * its later results do not write the same words again. */
    if (!session->interim.empty()) end_segment_locked(session);
    if (session->summarized_bytes >= session->full.size()) {
      return copy_out(session, session->summary, out, out_cap);
    }
    size_t start = session->summarized_bytes;
    while (start < session->full.size() && session->full[start] == '\n') ++start;
    delta = session->full.substr(start);
    target_bytes = session->full.size();
    start_epoch = session->epoch;
    if (delta.empty()) {
      session->summarized_bytes = target_bytes;
      return copy_out(session, session->summary, out, out_cap);
    }
  }

  std::string piece;
  const int rc = generate_chunks(session, delta, &piece);
  if (rc != 0) return rc;
  std::lock_guard<std::mutex> lock(session->mu);
  if (session->epoch == start_epoch && session->full.size() >= target_bytes) {
    if (!session->summary.empty() && !piece.empty()) session->summary.push_back('\n');
    session->summary += piece;
    session->summarized_bytes = target_bytes;
  }
  return copy_out(session, session->summary, out, out_cap);
}

void apexo_session_release(ApexoSession* session) {
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
  session->interim.clear();
  session->full.clear();
  session->display.clear();
  session->summary.clear();
  session->summarized_bytes = 0;
}

const char* apexo_session_last_error(const ApexoSession* session) {
  if (session == nullptr) return "";
  std::lock_guard<std::mutex> lock(session->mu);
  return session->error.c_str();
}
