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
  int release_during_generate;
  int destroy_during_generate;
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
  if (script->release_during_generate) apexo_session_release(script->session);
  if (script->destroy_during_generate) apexo_session_destroy(script->session);
  const char* text = script->reply != nullptr ? script->reply : "ok";
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
  script->release_during_generate = 0;
  script->destroy_during_generate = 0;
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
  ApexoSession* web = open_session(&script, "", 1);
  expect(apexo_session_summarize(web, rejected, sizeof(rejected)) == -4, "web summary needs a model name");
  ApexoSession* notbundle = open_session(&script, "model.bin", 0);
  expect(apexo_session_summarize(notbundle, rejected, sizeof(rejected)) == -4, "the summary model must be a .litertlm file");
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
  // The interim line is not lost when a segment ends.
  {
    Scripted script;
    script.events.push_back(final_event("第一句", 0, 500));
    script.events.push_back(interim_event("还没说完", 5000, 5400));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    push(session);
    push(session);
    apexo_session_end_segment(session);
    expect(std::strcmp(apexo_session_full_transcript(session), "第一句\n还没说完") == 0,
           "end-segment: the unfinished interim becomes final");
    expect(script.reset_count == 1, "end-segment: the recognizer is still reset");
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
    bool on_boundary = script.prompts.size() >= 2;
    const std::string marker = "转写：\n";
    for (size_t i = 0; i + 1 < script.prompts.size(); ++i) {
      const std::string& prompt = script.prompts[i];
      const std::string chunk = prompt.substr(prompt.find(marker) + marker.size());
      const size_t end = chunk.find_last_not_of(' ');
      if (end == std::string::npos || chunk[end] != '.') on_boundary = false;
    }
    expect(on_boundary, "chunk-boundary: chunks are cut after a sentence end");
    apexo_session_destroy(session);
  }
  // Calling summarize without end_segment still keeps the unfinished interim line.
  {
    Scripted script;
    script.events.push_back(interim_event("直接总结这句", 0, 600));
    ApexoSession* session = open_session(&script, "gemma-4-E2B-it.litertlm", 0);
    push(session);
    char out[128];
    const int rc = apexo_session_summarize(session, out, sizeof(out));
    expect(rc == 0 && std::strcmp(apexo_session_full_transcript(session), "直接总结这句") == 0 &&
               script.prompts.size() == 1 && script.prompts[0].find("直接总结这句") != std::string::npos,
           "summarize promotes an unfinished interim line before summarizing");
    apexo_session_destroy(session);
  }
  // Rolling incremental summary: includes the line still being recognized (closing it and resetting the
  // recognizer), sends only new text on later calls, and does not block push_pcm.
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
               script.prompts[0].find("第二段还在说") != std::string::npos &&
               std::strcmp(apexo_session_full_transcript(session), "第一段定稿\n第二段还在说") == 0 &&
               script.reset_count == 1,
           "summarize_new: includes the unfinished line, keeps it in the transcript and resets the recognizer");
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == 0 && script.load_count == 1 &&
               std::strcmp(out, "ok") == 0,
           "summarize_new: does not reload Gemma when no new final has arrived");
    push(session);  // final: 第二段说完了
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == 0 && std::strcmp(out, "ok\nok") == 0 &&
               std::strcmp(apexo_session_summary_text(session), "ok\nok") == 0,
           "summarize_new: appends the new segment summary to summary_text");
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
  // Calling release() while summarize or summarize_new is generating does not write back stale summaries.
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
    push(session);
    expect(apexo_session_summarize_new(session, out, sizeof(out)) == 0 && std::strcmp(out, "") == 0 &&
               std::strcmp(apexo_session_summary_text(session), "") == 0,
           "release during summarize_new does not write back stale summary");
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
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all session assertions passed\n");
  return 0;
}
