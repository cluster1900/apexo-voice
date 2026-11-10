#include "apexo_session.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const char* name) {
  if (ok) {
    std::printf("PASS %s\n", name);
  } else {
    std::printf("FAIL %s\n", name);
    failures += 1;
  }
}

struct Scripted {
  std::vector<ApexoAsrEvent> events;
  int index;
  int reset_count;
  int load_count;
  int loaded;
  int resident_during_generate;
  ApexoSession* session;
  std::vector<std::string> prompts;
};

int asr_push(void* user, const float*, int32_t, int32_t, ApexoAsrEvent* out) {
  Scripted* script = static_cast<Scripted*>(user);
  if (script->index >= static_cast<int>(script->events.size())) {
    out->is_final = -1;
    return 0;
  }
  *out = script->events[static_cast<size_t>(script->index)];
  script->index += 1;
  return 0;
}

void asr_reset(void* user) { static_cast<Scripted*>(user)->reset_count += 1; }

int sum_is_loaded(void* user) { return static_cast<Scripted*>(user)->loaded; }

int sum_load(void* user, const char*) {
  Scripted* script = static_cast<Scripted*>(user);
  script->loaded = 1;
  script->load_count += 1;
  return 0;
}

int sum_unload(void* user) {
  static_cast<Scripted*>(user)->loaded = 0;
  return 0;
}

int sum_generate(void* user, const char* prompt, char* out, int32_t out_cap) {
  Scripted* script = static_cast<Scripted*>(user);
  script->resident_during_generate = apexo_session_gemma_resident(script->session);
  script->prompts.push_back(prompt != nullptr ? prompt : "");
  const char* text = "ok";
  if (static_cast<int32_t>(std::strlen(text)) >= out_cap) return -1;
  std::strcpy(out, text);
  return 0;
}

ApexoAsrEvent final_event(const char* text, int start, int end) {
  ApexoAsrEvent event;
  std::memset(&event, 0, sizeof(event));
  std::snprintf(event.text, sizeof(event.text), "%s", text);
  event.is_final = 1;
  event.start_ms = start;
  event.end_ms = end;
  return event;
}

ApexoSession* open_session(Scripted* script, const char* model, int web) {
  script->index = 0;
  script->reset_count = 0;
  script->load_count = 0;
  script->loaded = 0;
  script->resident_during_generate = -1;
  script->prompts.clear();
  ApexoConfig config;
  std::memset(&config, 0, sizeof(config));
  config.merge_gap_ms = APEXO_MERGE_GAP_MS;
  config.summary_chunk_chars = APEXO_SUMMARY_CHUNK_CHARS;
  config.display_lines = APEXO_DISPLAY_LINES;
  config.summary_model_path = model;
  config.web_summary = web;
  ApexoEngines engines;
  std::memset(&engines, 0, sizeof(engines));
  engines.asr_user = script;
  engines.asr_push = asr_push;
  engines.asr_reset = asr_reset;
  engines.sum_user = script;
  engines.sum_is_loaded = sum_is_loaded;
  engines.sum_load = sum_load;
  engines.sum_unload = sum_unload;
  engines.sum_generate = sum_generate;
  ApexoSession* session = apexo_session_create(&config, &engines);
  script->session = session;
  return session;
}

void push(ApexoSession* session) {
  float sample = 0.1f;
  apexo_session_push_pcm(session, &sample, 1, APEXO_SAMPLE_RATE);
}

void test_merge() {
  Scripted script;
  script.events.push_back(final_event("你好", 0, 1000));
  script.events.push_back(final_event("世界", 2500, 3000));
  script.events.push_back(final_event("下一句", 5000, 5800));
  ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
  push(session);
  push(session);
  const std::string merged = apexo_session_display_text(session);
  push(session);
  const std::string both = apexo_session_display_text(session);
  expect(merged == "你好世界", "merge: finals separated by less than 1.5-2 seconds merge into one display segment");
  expect(both == "你好世界\n下一句", "merge: a gap of 2000 ms or more stays a separate display segment");
  apexo_session_end_segment(session);
  expect(script.reset_count == 1, "end segment resets the recognizer");
  apexo_session_destroy(session);
}

void test_apexo_buffer() {
  Scripted script;
  script.events.push_back(final_event("重复", 0, 400));
  script.events.push_back(final_event("重复", 800, 1200));
  std::string words;
  for (int i = 0; i < 250; ++i) {
    if (i) words.push_back(' ');
    words += "w" + std::to_string(i);
  }
  script.events.push_back(final_event(words.c_str(), 4000, 9000));
  ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
  push(session);
  push(session);
  expect(std::strcmp(apexo_session_display_text(session), "重复") == 0,
         "apexo-dedup: overlapping chunk text is not repeated");
  push(session);
  const std::string display = apexo_session_display_text(session);
  const std::string full = apexo_session_full_transcript(session);
  expect(display.find("w0 ") == std::string::npos && display.find("w249") != std::string::npos &&
             full.find("w0 ") != std::string::npos,
         "apexo-transcript-buffer: maxUiLines=4 maxWordCount=200 fillGapMs=2000");
  apexo_session_destroy(session);
}

void test_full_transcript() {
  Scripted script;
  for (int i = 0; i < 6; ++i) {
    char text[32];
    std::snprintf(text, sizeof(text), "line-%d", i);
    script.events.push_back(final_event(text, i * 3000, i * 3000 + 500));
  }
  ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
  for (int i = 0; i < 6; ++i) push(session);
  const std::string display = apexo_session_display_text(session);
  const std::string full = apexo_session_full_transcript(session);
  int display_lines = 1;
  for (char c : display) {
    if (c == '\n') display_lines += 1;
  }
  expect(display_lines == APEXO_DISPLAY_LINES && display.find("line-0") == std::string::npos &&
             display.find("line-5") != std::string::npos,
         "full-transcript: display keeps only recent lines");
  expect(full.find("line-0") != std::string::npos && full.find("line-5") != std::string::npos,
         "full-transcript: the full transcript is kept even when the display keeps only recent lines");
  apexo_session_destroy(session);
}

void test_summary() {
  Scripted script;
  std::string long_text(3200, 'a');
  script.events.push_back(final_event(long_text.c_str(), 0, 500));
  ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
  for (int i = 0; i < 3; ++i) {
    float sample = 0.2f;
    apexo_session_push_pcm(session, &sample, 1, APEXO_SAMPLE_RATE);
  }
  expect(script.load_count == 0 && apexo_session_gemma_resident(session) == 0,
         "Gemma-not-resident-during-transcription");
  char summary[256];
  const int rc = apexo_session_summarize(session, summary, sizeof(summary));
  expect(rc == 0 && script.load_count == 1 && apexo_session_gemma_resident(session) == 0,
         "Gemma-not-resident-during-transcription: Gemma loads only when a summary is requested and unloads after");
  expect(script.resident_during_generate == 1, "Gemma-not-resident-during-transcription: resident only inside summarize");
  const char* marker = "转写：\n";
  bool chunks_ok = script.prompts.size() >= 2;
  bool text_only = !script.prompts.empty();
  for (const std::string& prompt : script.prompts) {
    const size_t pos = prompt.find(marker);
    if (pos == std::string::npos || prompt.find("只根据下面的转写写摘要") == std::string::npos ||
        prompt.find("转写里没有的内容不要补") == std::string::npos) {
      text_only = false;
      chunks_ok = false;
      break;
    }
    const std::string chunk = prompt.substr(pos + std::strlen(marker));
    if (chunk.size() == 0 || chunk.size() > APEXO_SUMMARY_CHUNK_CHARS) chunks_ok = false;
    if (prompt.find("pcm") != std::string::npos) text_only = false;
  }
  expect(chunks_ok, "chunk-size: a long transcript is summarized in chunks of about 1500 characters");
  expect(text_only, "text-only-summary: the summarizer is given text only and told not to invent missing content");
  float bad = 0.0f;
  expect(apexo_session_push_pcm(session, &bad, 1, 48000) == -2, "pcm contract rejects a non-16 kHz buffer");
  char rejected[64];
  ApexoSession* web = open_session(&script, "gemma-4-E2B-it.litertlm", 1);
  expect(apexo_session_summarize(web, rejected, sizeof(rejected)) == -4,
         "web summary does not accept the non-web .litertlm");
  apexo_session_release(session);
  expect(apexo_session_gemma_resident(session) == 0, "release leaves the Gemma engine unloaded");
  apexo_session_destroy(session);
  apexo_session_destroy(web);
}

}  // namespace

int main() {
  test_merge();
  test_apexo_buffer();
  test_full_transcript();
  test_summary();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all session assertions passed\n");
  return 0;
}
