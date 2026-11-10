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
  int finish_count;
  int finish_rc;
  ApexoAsrEvent finish_event;
  int has_finish_event;
  int load_count;
  int load_rc;
  int generate_rc;
  int loaded;
  int resident_during_generate;
  int release_during_generate;
  int reset_during_generate;
  int destroy_during_generate;
  int push_during_load;
  int push_during_generate;
  int push_during_unload;
  int push_rc_during_load;
  int push_rc_during_generate;
  int push_rc_during_unload;
  int busy_rc_during_generate;
  int shrink_synthesis;
  const char* reply;
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

int asr_finish(void* user, ApexoAsrEvent* out) {
  Scripted* script = static_cast<Scripted*>(user);
  script->finish_count += 1;
  if (script->finish_rc != 0) return script->finish_rc;
  if (script->has_finish_event) {
    *out = script->finish_event;
  } else {
    out->is_final = -1;
  }
  return 0;
}

int sum_is_loaded(void* user) { return static_cast<Scripted*>(user)->loaded; }

int sum_load(void* user, const char*) {
  Scripted* script = static_cast<Scripted*>(user);
  if (script->push_during_load && script->session != nullptr) {
    float sample = 0.1f;
    script->push_rc_during_load = apexo_session_push_pcm(script->session, &sample, 1, APEXO_SAMPLE_RATE);
  }
  if (script->load_rc != 0) return script->load_rc;
  script->loaded = 1;
  script->load_count += 1;
  return 0;
}

int sum_unload(void* user) {
  Scripted* script = static_cast<Scripted*>(user);
  if (script->push_during_unload && script->session != nullptr) {
    float sample = 0.1f;
    script->push_rc_during_unload = apexo_session_push_pcm(script->session, &sample, 1, APEXO_SAMPLE_RATE);
  }
  script->loaded = 0;
  return 0;
}

int sum_generate(void* user, const char* prompt, char* out, int32_t out_cap) {
  Scripted* script = static_cast<Scripted*>(user);
  script->resident_during_generate = apexo_session_gemma_resident(script->session);
  script->prompts.push_back(prompt != nullptr ? prompt : "");
  if (script->push_during_generate && script->session != nullptr) {
    float sample = 0.1f;
    script->push_rc_during_generate = apexo_session_push_pcm(script->session, &sample, 1, APEXO_SAMPLE_RATE);
  }
  if (script->busy_rc_during_generate == 0 && script->session != nullptr) {
    char busy_out[64];
    const int r1 = apexo_session_summarize(script->session, busy_out, sizeof(busy_out));
    const int r2 = apexo_session_summarize_new(script->session, busy_out, sizeof(busy_out));
    const int r3 = apexo_session_summarize_text(script->session, "再试一次", busy_out, sizeof(busy_out));
    script->busy_rc_during_generate = (r1 == APEXO_ERR_BUSY && r2 == APEXO_ERR_BUSY && r3 == APEXO_ERR_BUSY) ? APEXO_ERR_BUSY : -999;
  }
  if (script->release_during_generate) apexo_session_release(script->session);
  if (script->reset_during_generate) apexo_session_reset(script->session);
  if (script->destroy_during_generate) apexo_session_destroy(script->session);
  if (script->generate_rc != 0) return script->generate_rc;
  std::string DynamicReply;
  const char* text = script->reply != nullptr ? script->reply : "ok";
  if (script->shrink_synthesis && prompt != nullptr && std::strstr(prompt, "材料：\n") != nullptr) {
    DynamicReply = "汇总结论";
    text = DynamicReply.c_str();
  }
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

ApexoSession* open_session_ext(Scripted* script, const char* model, int web, bool with_finish) {
  script->index = 0;
  script->reset_count = 0;
  script->finish_count = 0;
  script->finish_rc = 0;
  std::memset(&script->finish_event, 0, sizeof(script->finish_event));
  script->has_finish_event = 0;
  script->load_count = 0;
  script->load_rc = 0;
  script->generate_rc = 0;
  script->loaded = 0;
  script->resident_during_generate = -1;
  script->release_during_generate = 0;
  script->reset_during_generate = 0;
  script->destroy_during_generate = 0;
  script->push_during_load = 0;
  script->push_during_generate = 0;
  script->push_during_unload = 0;
  script->push_rc_during_load = -999;
  script->push_rc_during_generate = -999;
  script->push_rc_during_unload = -999;
  script->busy_rc_during_generate = -999;
  script->shrink_synthesis = 0;
  script->reply = nullptr;
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
  engines.asr_finish = with_finish ? asr_finish : nullptr;
  engines.sum_user = script;
  engines.sum_is_loaded = sum_is_loaded;
  engines.sum_load = sum_load;
  engines.sum_unload = sum_unload;
  engines.sum_generate = sum_generate;
  ApexoSession* session = apexo_session_create(&config, &engines);
  script->session = session;
  return session;
}

ApexoSession* open_session(Scripted* script, const char* model, int web) {
  return open_session_ext(script, model, web, false);
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
  script.events.push_back(final_event("重复", 200, 600));
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
  const char* syn_marker = "材料：\n";
  bool chunks_ok = script.prompts.size() >= 3;
  bool text_only = !script.prompts.empty();
  for (size_t i = 0; i + 1 < script.prompts.size(); ++i) {
    const std::string& prompt = script.prompts[i];
    const size_t pos = prompt.find(marker);
    if (pos == std::string::npos || prompt.find("仅依据以下转写提取简洁事实摘要") == std::string::npos ||
        prompt.find("没有明确给出的信息不要补充") == std::string::npos) {
      text_only = false;
      chunks_ok = false;
      break;
    }
    const std::string chunk = prompt.substr(pos + std::strlen(marker));
    if (chunk.size() == 0 || chunk.size() > APEXO_SUMMARY_CHUNK_CHARS) chunks_ok = false;
    if (prompt.find("pcm") != std::string::npos) text_only = false;
  }
  if (!script.prompts.empty()) {
    const std::string& last = script.prompts.back();
    if (last.find(syn_marker) == std::string::npos ||
        last.find("请归纳成一份简洁的会议总结") == std::string::npos) {
      chunks_ok = false;
      text_only = false;
    }
  }
  expect(chunks_ok, "chunk-size: a long transcript is summarized in chunks of about 1500 characters and synthesized");
  expect(text_only, "text-only-summary: the summarizer is given text only and told not to invent missing content");
  float bad = 0.0f;
  expect(apexo_session_push_pcm(session, &bad, 1, 48000) == -2, "pcm contract rejects a non-16 kHz buffer");
  char rejected[64];
  ApexoSession* web = open_session(&script, "", 1);
  expect(apexo_session_summarize_text(web, "需要总结的文本", rejected, sizeof(rejected)) == -4, "web summary needs a model name");
  ApexoSession* notbundle = open_session(&script, "model.bin", 0);
  expect(apexo_session_summarize_text(notbundle, "需要总结的文本", rejected, sizeof(rejected)) == -4,
         "the summary model must be a .litertlm file");
  apexo_session_destroy(notbundle);
  apexo_session_release(session);
  expect(apexo_session_gemma_resident(session) == 0, "release leaves the Gemma engine unloaded");
  apexo_session_destroy(session);
  apexo_session_destroy(web);
}

ApexoAsrEvent interim_event(const char* text, int start, int end) {
  ApexoAsrEvent event = final_event(text, start, end);
  event.is_final = 0;
  return event;
}

void test_fixes() {
  // Silence endpoints deliver finals with no words. They must not leave blank lines or empty display lines.
  {
    Scripted script;
    script.events.push_back(final_event("你好", 0, 500));
    script.events.push_back(final_event("", 2400, 2500));
    script.events.push_back(final_event("", 4800, 4900));
    script.events.push_back(final_event("世界", 9000, 9500));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    for (int i = 0; i < 4; ++i) push(session);
    expect(std::strcmp(apexo_session_full_transcript(session), "你好\n世界") == 0,
           "empty-final: finals without words are not added to the transcript");
    expect(std::strcmp(apexo_session_display_text(session), "你好\n世界") == 0,
           "empty-final: finals without words do not take a display line");
    apexo_session_destroy(session);
  }
  // Fallback when asr_finish is not provided: end_segment promotes the interim line and calls asr_reset.
  {
    Scripted script;
    script.events.push_back(final_event("第一句", 0, 500));
    script.events.push_back(interim_event("还没说完", 5000, 5400));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    push(session);
    push(session);
    apexo_session_end_segment(session);
    expect(std::strcmp(apexo_session_full_transcript(session), "第一句\n还没说完") == 0,
           "end-segment fallback: the unfinished interim becomes final when asr_finish is absent");
    expect(script.reset_count == 1, "end-segment fallback: the recognizer is reset when asr_finish is absent");
    apexo_session_destroy(session);
  }
  // Latin words that merge onto one line keep a space between them.
  {
    Scripted script;
    script.events.push_back(final_event("HELLO WORLD", 0, 1000));
    script.events.push_back(final_event("HOW ARE YOU", 1500, 2500));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    push(session);
    push(session);
    expect(std::strcmp(apexo_session_display_text(session), "HELLO WORLD HOW ARE YOU") == 0,
           "merge: latin finals are joined with a space");
    apexo_session_destroy(session);
  }
  // The interim line counts toward the line limit.
  {
    Scripted script;
    for (int i = 0; i < 6; ++i) {
      char text[16];
      std::snprintf(text, sizeof(text), "L%d", i);
      script.events.push_back(final_event(text, i * 5000, i * 5000 + 100));
    }
    script.events.push_back(interim_event("interim", 40000, 40100));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    for (int i = 0; i < 7; ++i) push(session);
    int lines = 1;
    for (const char* c = apexo_session_display_text(session); *c; ++c) {
      if (*c == '\n') lines += 1;
    }
    expect(lines == APEXO_DISPLAY_LINES, "display: interim plus finals stay within the line limit");
    apexo_session_destroy(session);
  }
  // Nothing was said: Gemma is not loaded and no summary is written.
  {
    Scripted script;
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    char out[64];
    std::memset(out, 'x', sizeof(out));
    const int rc = apexo_session_summarize(session, out, sizeof(out));
    expect(rc == 0 && out[0] == '\0' && script.load_count == 0 && script.prompts.empty(),
           "empty-transcript: summarize does not load or call Gemma");
    apexo_session_destroy(session);
  }
  // Chunks end on a sentence when there is one in the second half of the chunk.
  {
    Scripted script;
    std::string text;
    for (int i = 0; i < 130; ++i) text += "Sentence number one is right here. ";
    script.events.push_back(final_event(text.c_str(), 0, 500));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    push(session);
    char out[512];
    apexo_session_summarize(session, out, sizeof(out));
    bool on_boundary = script.prompts.size() >= 3;
    const std::string marker = "转写：\n";
    for (size_t i = 0; i + 2 < script.prompts.size(); ++i) {
      const std::string& prompt = script.prompts[i];
      const std::string chunk = prompt.substr(prompt.find(marker) + marker.size());
      const size_t end = chunk.find_last_not_of(' ');
      if (end == std::string::npos || chunk[end] != '.') on_boundary = false;
    }
    expect(on_boundary, "chunk-boundary: chunks are cut after a sentence end");
    apexo_session_destroy(session);
  }
  // T01: Calling summarize without end_segment does NOT promote interim or reset ASR; fuller final arrives once.
  {
    Scripted script;
    script.events.push_back(interim_event("直接总结", 0, 600));
    script.events.push_back(final_event("直接总结这句完整的话", 0, 1200));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    push(session);  // interim only
    char out[128];
    std::memset(out, 'x', sizeof(out));
    const int rc1 = apexo_session_summarize(session, out, sizeof(out));
    expect(rc1 == 0 && out[0] == '\0' && script.load_count == 0 &&
               std::strcmp(apexo_session_full_transcript(session), "") == 0 &&
               std::strcmp(apexo_session_display_text(session), "直接总结") == 0 &&
               script.reset_count == 0,
           "T01: summarize does not promote interim or reset ASR");
    push(session);  // fuller final arrives
    expect(std::strcmp(apexo_session_full_transcript(session), "直接总结这句完整的话") == 0 &&
               std::strcmp(apexo_session_display_text(session), "直接总结这句完整的话") == 0,
           "T01: fuller final enters transcript once without duplication");
    const int rc2 = apexo_session_summarize(session, out, sizeof(out));
    expect(rc2 == 0 && std::strcmp(out, "ok") == 0 && script.prompts.size() == 1 &&
               script.prompts[0].find("直接总结这句完整的话") != std::string::npos &&
               script.reset_count == 0,
           "T01: summarize summarizes the finalized utterance without resetting ASR");
    apexo_session_destroy(session);
  }
  // Rolling incremental summary (summarize_new): summarizes only finalized segments, preserves interim & ASR state,
  // sends only new finalized text on later calls, and appends stage summaries.
  {
    Scripted script;
    script.events.push_back(final_event("第一段定稿", 0, 500));
    script.events.push_back(interim_event("第二段还在说", 3000, 3400));
    script.events.push_back(final_event("第二段说完了", 3000, 3800));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    push(session);  // final: 第一段定稿
    push(session);  // interim: 第二段还在说
    char out[256];
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == 0 && std::strcmp(out, "ok") == 0,
           "summarize_new: summarizes the first finalized segment");
    expect(script.prompts.size() == 1 && script.prompts[0].find("第一段定稿") != std::string::npos &&
               script.prompts[0].find("第二段还在说") == std::string::npos &&
               std::strcmp(apexo_session_full_transcript(session), "第一段定稿") == 0 &&
               std::strcmp(apexo_session_display_text(session), "第一段定稿\n第二段还在说") == 0 &&
               script.reset_count == 0,
           "summarize_new: excludes unfinished interim, keeps interim on display, and does not reset the recognizer");
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == 0 && script.load_count == 1 &&
               std::strcmp(out, "ok") == 0,
           "summarize_new: does not reload Gemma when no new final has arrived");
    push(session);  // final: 第二段说完了
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == 0 && std::strcmp(out, "ok\nok") == 0 &&
               std::strcmp(apexo_session_summary_text(session), "ok\nok") == 0 &&
               script.reset_count == 0,
           "summarize_new: appends the new segment summary to summary_text without resetting ASR");
    expect(script.prompts.size() == 2 && script.prompts[1].find("第一段定稿") == std::string::npos &&
               script.prompts[1].find("第二段说完了") != std::string::npos,
           "summarize_new: sends only the newly finalized delta to Gemma");
    apexo_session_destroy(session);
  }
  // Separate identical spoken utterances (after end_segment or >= merge_gap_ms) are both retained.
  {
    Scripted script;
    script.events.push_back(final_event("同样一句话", 0, 500));
    script.events.push_back(final_event("同样一句话", 0, 500));
    script.events.push_back(final_event("同样一句话", 3000, 3500));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    push(session);
    apexo_session_end_segment(session);
    push(session);
    push(session);
    expect(std::strcmp(apexo_session_full_transcript(session), "同样一句话\n同样一句话\n同样一句话") == 0,
           "dedup: separate identical utterances after end_segment or >= merge_gap_ms are retained");
    apexo_session_destroy(session);
  }
  // A heading the model writes itself ("摘要：", "**总结**:", "Summary:") is dropped; ordinary text is kept.
  {
    const char* cases[][2] = {{"摘要：\n\n要点一", "要点一"}, {"**总结**: 要点二", "要点二"},
                              {"## 摘要\n要点三", "要点三"}, {"Summary: point", "point"},
                              {"总结一下今天的会", "总结一下今天的会"}, {"转写内容摘要：  要点四", "要点四"},
                              {"以下是会议总结：\n要点五", "要点五"}, {"总结，今天先到这里", "总结，今天先到这里"}};
    bool ok = true;
    for (auto& c : cases) {
      Scripted script;
      script.events.push_back(final_event("会议内容", 0, 500));
      ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
      script.reply = c[0];
      push(session);
      char out[256];
      ok = ok && apexo_session_summarize(session, out, sizeof(out)) == 0 && std::strcmp(out, c[1]) == 0;
      if (std::strcmp(out, c[1]) != 0) std::printf("  label case [%s] -> [%s]\n", c[0], out);
      apexo_session_destroy(session);
    }
    expect(ok, "summary: a leading label written by the model is removed");
  }
  // Two close utterances that read the same, with no audio overlap, are both kept even without end_segment.
  {
    Scripted script;
    script.events.push_back(final_event("确认", 0, 500));
    script.events.push_back(final_event("确认", 1800, 2300));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    push(session);
    push(session);
    expect(std::strcmp(apexo_session_full_transcript(session), "确认\n确认") == 0,
           "dedup: close but non-overlapping identical finals are both kept");
    apexo_session_destroy(session);
  }
  // Destroying the session while a summary runs: the summary finishes and then frees it (run under ASan).
  {
    Scripted script;
    script.events.push_back(final_event("总结中关闭", 0, 500));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    script.destroy_during_generate = 1;
    push(session);
    char out[256];
    expect(apexo_session_summarize(session, out, sizeof(out)) == 0 && std::strcmp(out, "ok") == 0,
           "destroy during summarize lets the summary finish and frees the session afterwards");
  }
  // Calling reset() or release() while summarize or summarize_new is generating does not write back stale summaries.
  {
    Scripted script;
    script.events.push_back(final_event("释放前的话", 0, 500));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    script.release_during_generate = 1;
    push(session);
    char out[256];
    expect(apexo_session_summarize(session, out, sizeof(out)) == 0 && std::strcmp(out, "") == 0 &&
               std::strcmp(apexo_session_summary_text(session), "") == 0,
           "release during summarize does not write back stale summary");
    script.events.push_back(final_event("第二次释放前的话", 0, 500));
    script.release_during_generate = 0;
    script.reset_during_generate = 1;
    push(session);
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == 0 && std::strcmp(out, "") == 0 &&
               std::strcmp(apexo_session_summary_text(session), "") == 0,
           "reset during summarize_new does not write back stale summary");
    apexo_session_destroy(session);
  }
}

struct CallbackCollector {
  ApexoSession* session;
  std::vector<ApexoAsrEvent> events;
  std::vector<std::string> seen_full;
};

void on_transcript_cb(void* user, const ApexoAsrEvent* event) {
  CallbackCollector* col = static_cast<CallbackCollector*>(user);
  col->events.push_back(*event);
  if (col->session != nullptr) {
    col->seen_full.push_back(apexo_session_full_transcript(col->session));
  }
}

void test_v020_contracts() {
  // T02: Continuous push_pcm during sum_load, sum_generate, and sum_unload succeeds and enters ASR;
  // new finals are summarized on the next summarize_new() call.
  {
    Scripted script;
    script.events.push_back(final_event("快照第一句", 0, 500));
    script.events.push_back(final_event("加载期间新句", 3000, 3500));
    script.events.push_back(final_event("生成期间新句", 6000, 6500));
    script.events.push_back(final_event("卸载期间新句", 9000, 9500));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    push(session);  // 快照第一句
    script.push_during_load = 1;
    script.push_during_generate = 1;
    script.push_during_unload = 1;
    char out[256];
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == 0 && std::strcmp(out, "ok") == 0,
           "T02: summarize_new succeeds while audio is pushed concurrently");
    expect(script.push_rc_during_load == 0 && script.push_rc_during_generate == 0 && script.push_rc_during_unload == 0,
           "T02: push_pcm succeeds during load, generate, and unload phases");
    expect(script.prompts.size() == 1 && script.prompts[0].find("快照第一句") != std::string::npos &&
               script.prompts[0].find("加载期间新句") == std::string::npos,
           "T02: current summary only covers the snapshot taken before generation");
    script.push_during_load = 0;
    script.push_during_generate = 0;
    script.push_during_unload = 0;
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == 0 && std::strcmp(out, "ok\nok") == 0 &&
               script.prompts.size() == 2 &&
               script.prompts[1].find("加载期间新句") != std::string::npos &&
               script.prompts[1].find("生成期间新句") != std::string::npos &&
               script.prompts[1].find("卸载期间新句") != std::string::npos,
           "T02: finals arriving during summary are processed by the next summarize_new");
    apexo_session_destroy(session);
  }
  // T03: Alternating summarize_new() and summarize() keeps cursor accurate.
  {
    Scripted script;
    script.events.push_back(final_event("阶段A", 0, 500));
    script.events.push_back(final_event("阶段B", 3000, 3500));
    script.events.push_back(final_event("阶段C", 6000, 6500));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    char out[256];
    push(session);  // 阶段A
    script.reply = "摘A";
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == 0 && std::strcmp(out, "摘A") == 0,
           "T03: first summarize_new summarizes A");
    push(session);  // 阶段B
    script.reply = "全AB";
    expect(apexo_session_summarize(session, out, sizeof(out)) == 0 && std::strcmp(out, "全AB") == 0,
           "T03: summarize() summarizes full A+B and advances cursor to end of A+B");
    const int loads_before = script.load_count;
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == 0 &&
               std::strcmp(out, "全AB") == 0 && script.load_count == loads_before,
           "T03: summarize_new right after summarize() sees no new final and does not reload model");
    push(session);  // 阶段C
    script.reply = "摘C";
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == 0 &&
               std::strcmp(out, "全AB\n摘C") == 0 &&
               script.prompts.back().find("阶段C") != std::string::npos &&
               script.prompts.back().find("阶段A") == std::string::npos,
           "T03: subsequent summarize_new summarizes only C and appends to summary");
    apexo_session_destroy(session);
  }
  // T04: Failure after an existing summary preserves old summary and cursor.
  {
    Scripted script;
    script.events.push_back(final_event("第一段成功", 0, 500));
    script.events.push_back(final_event("第二段将失败", 3000, 3500));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    char out[256];
    push(session);
    script.reply = "已有摘要";
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == 0 && std::strcmp(out, "已有摘要") == 0,
           "T04: baseline summary created");
    push(session);
    script.generate_rc = -5;
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == -5 &&
               std::strcmp(apexo_session_summary_text(session), "已有摘要") == 0,
           "T04: generation failure (-5) preserves previous summary_text");
    script.generate_rc = 0;
    script.reply = "过长的摘要内容无法放入极小缓冲区";
    char tiny[4];
    expect(apexo_session_summarize_new(session, tiny, sizeof(tiny)) == -1 &&
               std::strcmp(apexo_session_summary_text(session), "已有摘要") == 0,
           "T04: insufficient out_cap (-1) preserves previous summary_text and cursor");
    script.reply = "补上第二段";
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == 0 &&
               std::strcmp(out, "已有摘要\n补上第二段") == 0 &&
               std::strcmp(apexo_session_last_error(session), "") == 0,
           "T04: retry after failure still summarizes the unsummarized delta and clears last_error");
    apexo_session_destroy(session);
  }
  // T05: External text summary (apexo_session_summarize_text) does not touch ASR or session transcript/summary.
  {
    Scripted script;
    script.events.push_back(final_event("会话内原稿", 0, 500));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    push(session);
    script.reply = "会话内摘要";
    char session_sum[256];
    apexo_session_summarize_new(session, session_sum, sizeof(session_sum));
    script.reply = "外部文本摘要";
    char ext_out[256];
    expect(apexo_session_summarize_text(session, "这是一段外部传入的独立文章。", ext_out, sizeof(ext_out)) == 0 &&
               std::strcmp(ext_out, "外部文本摘要") == 0,
           "T05: summarize_text summarizes external text");
    expect(std::strcmp(apexo_session_full_transcript(session), "会话内原稿") == 0 &&
               std::strcmp(apexo_session_summary_text(session), "会话内摘要") == 0 &&
               script.index == 1 && script.reset_count == 0,
           "T05: summarize_text does not pollute session transcript, summary, cursor, or ASR state");
    const int loads_before = script.load_count;
    std::memset(ext_out, 'x', sizeof(ext_out));
    expect(apexo_session_summarize_text(session, "   \n\t  ", ext_out, sizeof(ext_out)) == 0 &&
               ext_out[0] == '\0' && script.load_count == loads_before,
           "T05/T06: whitespace-only external text returns empty without loading Gemma");
    expect(apexo_session_summarize_text(session, nullptr, ext_out, sizeof(ext_out)) == -1,
           "T05: null text returns -1");
    apexo_session_destroy(session);
  }
  // T06: Concurrent summary calls return APEXO_ERR_BUSY (-6).
  {
    Scripted script;
    script.events.push_back(final_event("并发测试内容", 0, 500));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    push(session);
    script.busy_rc_during_generate = 0;
    char out[256];
    expect(apexo_session_summarize(session, out, sizeof(out)) == 0 &&
               script.busy_rc_during_generate == APEXO_ERR_BUSY,
           "T06: overlapping summarize / summarize_new / summarize_text calls return APEXO_ERR_BUSY (-6)");
    apexo_session_destroy(session);
  }
  // T07: end_segment calls asr_finish when bound, accepts final tail once, and does not call asr_reset.
  {
    Scripted script;
    script.events.push_back(final_event("前半段", 0, 500));
    script.events.push_back(interim_event("最后半句", 2500, 2900));
    ApexoSession* session = open_session_ext(&script, "gemma-4-E2B-it.litertlm", 0, true);
    script.has_finish_event = 1;
    script.finish_event = final_event("最后半句完整尾词", 2500, 3100);
    push(session);
    push(session);
    expect(apexo_session_end_segment(session) == 0 &&
               script.finish_count == 1 && script.reset_count == 0 &&
               std::strcmp(apexo_session_full_transcript(session), "前半段\n最后半句完整尾词") == 0 &&
               std::strcmp(apexo_session_display_text(session), "前半段\n最后半句完整尾词") == 0,
           "T07: end_segment uses asr_finish to flush final tail words without calling asr_reset");
    script.finish_rc = -5;
    expect(apexo_session_end_segment(session) == -5 &&
               std::strcmp(apexo_session_full_transcript(session), "前半段\n最后半句完整尾词") == 0,
           "T07: end_segment propagates asr_finish error and preserves existing transcript");
    apexo_session_destroy(session);
  }
  // T09: Cross-chunk synthesis multi-round reduction and non-convergence guard (-5).
  {
    Scripted script;
    std::string long_text(3200, 'b');
    script.events.push_back(final_event(long_text.c_str(), 0, 500));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    push(session);
    char out[4096];
    script.reply = "第一段摘要";
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == 0, "T09: baseline stage summary");
    // Now set reply to a 900-char string (> 750 chars each, so 3 chunks = 2700 chars > 1500 chars)
    // that does NOT shrink across synthesis rounds -> must trigger non-convergence guard (-5).
    std::string non_shrinking(900, 'x');
    script.reply = non_shrinking.c_str();
    expect(apexo_session_summarize(session, out, sizeof(out)) == -5 &&
               std::strcmp(apexo_session_summary_text(session), "第一段摘要\n第一段摘要\n第一段摘要") == 0,
           "T09: non-converging multi-round synthesis returns -5 and preserves existing summary");
    // When synthesis shrinks properly, multi-round synthesis succeeds.
    script.shrink_synthesis = 1;
    expect(apexo_session_summarize(session, out, sizeof(out)) == 0 &&
               std::strcmp(out, "汇总结论") == 0,
           "T09: shrinking multi-round synthesis converges to final summary");
    apexo_session_destroy(session);
  }
  // D07: Transcript callback and reset().
  {
    Scripted script;
    script.events.push_back(interim_event("草稿", 0, 200));
    script.events.push_back(final_event("第一句", 0, 500));
    script.events.push_back(final_event("第一句", 100, 600));  // overlapping dedup -> ignored
    script.events.push_back(final_event("第二句", 800, 1200));  // merges on display, still separate callback
    ApexoSession* session = open_session_ext(&script, "gemma-4-E2B-it.litertlm", 0, true);
    script.has_finish_event = 1;
    script.finish_event = final_event("收尾句", 3500, 4000);
    CallbackCollector col{session, {}, {}};
    apexo_session_set_transcript_callback(session, on_transcript_cb, &col);
    for (int i = 0; i < 4; ++i) push(session);
    apexo_session_end_segment(session);
    expect(col.events.size() == 4 &&
               col.events[0].is_final == 0 && std::strcmp(col.events[0].text, "草稿") == 0 &&
               col.events[1].is_final == 1 && std::strcmp(col.events[1].text, "第一句") == 0 &&
               col.events[2].is_final == 1 && std::strcmp(col.events[2].text, "第二句") == 0 &&
               col.events[3].is_final == 1 && std::strcmp(col.events[3].text, "收尾句") == 0 &&
               col.seen_full.back() == "第一句\n第二句\n收尾句" &&
               std::strcmp(apexo_session_display_text(session), "第一句第二句\n收尾句") == 0,
           "D07: transcript callback receives interim, non-deduped finals, and finish event outside session lock");
    apexo_session_reset(session);
    expect(std::strcmp(apexo_session_full_transcript(session), "") == 0 &&
               std::strcmp(apexo_session_display_text(session), "") == 0 &&
               std::strcmp(apexo_session_summary_text(session), "") == 0 &&
               script.reset_count == 1,
           "D07: apexo_session_reset clears transcript, display, summary, and resets ASR");
    apexo_session_destroy(session);
  }
}

}  // namespace

int main() {
  test_merge();
  test_apexo_buffer();
  test_full_transcript();
  test_summary();
  test_fixes();
  test_v020_contracts();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all session assertions passed\n");
  return 0;
}
