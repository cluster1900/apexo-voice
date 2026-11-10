#include "apexo_session.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool ok, const char* name) {
  if (ok) {
    std::cout << "PASS " << name << "\n";
    return;
  }
  std::cout << "FAIL " << name << "\n";
  g_failures += 1;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void write_file(const std::string& path, const std::string& body) {
  std::ofstream out(path);
  out << body;
}

std::string make_dir() {
  char tmpl[] = "apexo-adapter-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return "";
  return tmpl;
}

void remove_tree(const std::string& dir) {
  const std::string command = "rm -rf '" + dir + "'";
  std::system(command.c_str());
}

ApexoSession* open_session(const std::string& asr_dir, const char* gemma, void** state_out) {
  void* state = apexo_engine_state_create(asr_dir.c_str(), gemma);
  ApexoEngines engines;
  if (apexo_engine_state_bind(state, &engines) != 0) {
    apexo_engine_state_destroy(state);
    return nullptr;
  }
  ApexoConfig config;
  std::memset(&config, 0, sizeof(config));
  config.merge_gap_ms = APEXO_MERGE_GAP_MS;
  config.summary_chunk_chars = APEXO_SUMMARY_CHUNK_CHARS;
  config.display_lines = APEXO_DISPLAY_LINES;
  config.summary_model_path = gemma;
  config.web_summary = 0;
  ApexoSession* session = apexo_session_create(&config, &engines);
  if (session == nullptr) apexo_engine_state_destroy(state);
  else *state_out = state;
  return session;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: adapter_test fake_sherpa fake_litert\n";
    return 2;
  }
  setenv("APEXO_SHERPA_LIBRARY", argv[1], 1);
  setenv("APEXO_LITERT_LIBRARY", argv[2], 1);
  const std::string dir = make_dir();
  expect(!dir.empty(), "adapter temp dir");
  if (dir.empty()) return 1;
  write_file(dir + "/tokens.txt", "a\n");
  write_file(dir + "/encoder.onnx", "e");
  write_file(dir + "/decoder.onnx", "d");
  write_file(dir + "/joiner.onnx", "j");
  const std::string gemma = dir + "/gemma-4-E2B-it.litertlm";
  write_file(gemma, "gemma");
  const std::string log_path = dir + "/calls.log";
  setenv("APEXO_FAKE_LOG", log_path.c_str(), 1);

  void* state = nullptr;
  ApexoSession* session = open_session(dir, gemma.c_str(), &state);
  expect(session != nullptr, "adapter session create");
  // Calling end_segment before any audio was pushed returns 0 without creating recognizer or calling input_finished.
  expect(apexo_session_end_segment(session) == 0, "end_segment without audio returns 0");
  expect(read_file(log_path).find("input_finished") == std::string::npos,
         "end_segment without audio does not call input_finished");
  std::vector<float> pcm(1600, 0.1f);
  const int push = apexo_session_push_pcm(session, pcm.data(), static_cast<int32_t>(pcm.size()), 16000);
  expect(push == 0, "pushPcm accepts 16 kHz PCM");
  const std::string transcript = apexo_session_full_transcript(session);
  expect(transcript.find("适配器转写") != std::string::npos, "sherpa-onnx result enters the transcript");
  expect(apexo_session_gemma_resident(session) == 0, "Gemma-not-resident-during-transcription");
  char summary[4096];
  const int summarized = apexo_session_summarize(session, summary, sizeof(summary));
  expect(summarized == 0, "summarize return");
  expect(std::string(summary).find("适配器摘要") != std::string::npos, "LiteRT-LM generation is returned");
  expect(apexo_session_gemma_resident(session) == 0, "Gemma unloads after summarize");
  const std::string log = read_file(log_path);
  expect(log.find("accept 16000 1600") != std::string::npos, "sherpa-onnx acceptWaveform 16000");
  expect(log.find("vad ") == std::string::npos, "no unused vad is fed");
  expect(log.find("settings " + gemma) != std::string::npos, "litert-lm loads the gemma file");
  expect(log.find("只写材料中有依据的要点") != std::string::npos, "text-only-summary");
  expect(log.find("材料：") != std::string::npos, "generates from the transcript-only prompt");
  expect(log.find("适配器转写") != std::string::npos, "prompt contains the transcript");
  expect(log.find("pcm") == std::string::npos, "prompt does not contain pcm");
  expect(log.find("engine_delete") != std::string::npos, "litert-lm unloads");
  // Test explicit end_segment flushing tail frames via SherpaOnnxOnlineStreamInputFinished when no endpoint occurred.
  setenv("APEXO_FAKE_NO_ENDPOINT", "1", 1);
  setenv("APEXO_FAKE_FINISH_TEXT", "尾词收尾完整", 1);
  expect(apexo_session_push_pcm(session, pcm.data(), static_cast<int32_t>(pcm.size()), 16000) == 0,
         "push_pcm with no endpoint delivers interim");
  expect(std::string(apexo_session_full_transcript(session)).find("尾词收尾完整") == std::string::npos,
         "tail word is not finalized before end_segment");
  expect(apexo_session_end_segment(session) == 0, "end_segment flushes stream via input_finished");
  expect(std::string(apexo_session_full_transcript(session)).find("尾词收尾完整") != std::string::npos,
         "end_segment writes the flushed tail word into full_transcript");
  const std::string finish_log = read_file(log_path);
  expect(finish_log.find("accept 16000 4800") != std::string::npos &&
             finish_log.find("input_finished") != std::string::npos,
         "asr_finish feeds tail padding and calls SherpaOnnxOnlineStreamInputFinished");
  unsetenv("APEXO_FAKE_NO_ENDPOINT");
  unsetenv("APEXO_FAKE_FINISH_TEXT");
  setenv("APEXO_FAKE_JSON", "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"\\u4f60\\u597d \\ud83d\\ude00 a\\nb\\\"c\"}]}", 1);
  char escaped[256];
  expect(apexo_session_summarize(session, escaped, sizeof(escaped)) == 0, "summarize with escaped json");
  expect(std::string(escaped) == "你好 😀 a\nb\"c", "\\uXXXX escapes, surrogate pairs and newlines are decoded");
  setenv("APEXO_FAKE_JSON", "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"id\":\"meta\",\"text\":\"真实摘要\"}]}", 1);
  expect(apexo_session_summarize(session, escaped, sizeof(escaped)) == 0 && std::string(escaped) == "真实摘要",
         "json parser skips string values like \"type\":\"text\" and finds the \"text\" key");
  unsetenv("APEXO_FAKE_JSON");
  apexo_session_destroy(session);
  apexo_engine_state_destroy(state);

  write_file(log_path, "");
  // No asr directory: no invented text, a clear "not attached" code; but summarize_text (summary-only) works!
  void* absent_state = nullptr;
  ApexoSession* absent = open_session(dir + "/no-asr", gemma.c_str(), &absent_state);
  char text_only_sum[256];
  expect(apexo_session_summarize_text(absent, "外部独立文本，不需要ASR模型即可总结。", text_only_sum, sizeof(text_only_sum)) == 0 &&
             std::string(text_only_sum).find("适配器摘要") != std::string::npos,
         "summarize_text works without an ASR model directory (summary-only scenario)");
  expect(std::string(apexo_session_full_transcript(absent)).empty() &&
             std::string(apexo_session_summary_text(absent)).empty(),
         "summarize_text leaves session transcript and summary_text untouched");
  expect(apexo_session_push_pcm(absent, pcm.data(), static_cast<int32_t>(pcm.size()), 16000) == -3,
         "missing asr model returns -3");
  expect(std::string(apexo_session_display_text(absent)).empty() && std::string(apexo_session_full_transcript(absent)).empty(),
         "missing asr model paints no text");
  apexo_session_destroy(absent);
  apexo_engine_state_destroy(absent_state);
  write_file(log_path, "");
  // A directory that exists but lacks one of the four files is also -3.
  void* partial_state = nullptr;
  const std::string partial_dir = dir + "/partial";
  std::system(("mkdir -p '" + partial_dir + "' && cp '" + dir + "/tokens.txt' '" + partial_dir + "/'").c_str());
  ApexoSession* partial = open_session(partial_dir, gemma.c_str(), &partial_state);
  expect(apexo_session_push_pcm(partial, pcm.data(), static_cast<int32_t>(pcm.size()), 16000) == -3,
         "an asr directory with missing files returns -3");
  apexo_session_destroy(partial);
  apexo_engine_state_destroy(partial_state);
  // Missing Gemma file: summarize says so; it does not return a made-up summary.
  const std::string missing = dir + "/missing-gemma-4-E2B-it.litertlm";
  void* nogemma_state = nullptr;
  ApexoSession* nogemma = open_session(dir, missing.c_str(), &nogemma_state);
  apexo_session_push_pcm(nogemma, pcm.data(), static_cast<int32_t>(pcm.size()), 16000);
  char none[256];
  std::memset(none, 'x', sizeof(none));
  expect(apexo_session_summarize(nogemma, none, sizeof(none)) == -3, "missing gemma file returns -3");
  expect(none[0] == '\0' && apexo_session_gemma_resident(nogemma) == 0, "missing gemma file leaves no summary and no resident engine");
  const std::string absent_log = read_file(log_path);
  expect(absent_log.find("settings ") == std::string::npos, "missing gemma file does not call LiteRT-LM");
  expect(absent_log.find("engine_create") == std::string::npos, "missing gemma file does not create an engine");
  apexo_session_destroy(nogemma);
  apexo_engine_state_destroy(nogemma_state);

  // Library paths given through the API win over the environment.
  setenv("APEXO_SHERPA_LIBRARY", "/no/such/libsherpa-onnx.dylib", 1);
  setenv("APEXO_LITERT_LIBRARY", "/no/such/liblitert_lm.dylib", 1);
  {
    void* by_api = apexo_engine_state_create(dir.c_str(), gemma.c_str());
    apexo_engine_state_set_libraries(by_api, argv[1], argv[2]);
    ApexoEngines engines;
    apexo_engine_state_bind(by_api, &engines);
    ApexoConfig config;
    std::memset(&config, 0, sizeof(config));
    config.summary_model_path = gemma.c_str();
    ApexoSession* via_api = apexo_session_create(&config, &engines);
    expect(apexo_session_push_pcm(via_api, pcm.data(), static_cast<int32_t>(pcm.size()), 16000) == 0,
           "set_libraries: the recognizer loads from the path given through the API");
    char api_summary[256];
    expect(apexo_session_summarize(via_api, api_summary, sizeof(api_summary)) == 0 &&
               std::string(api_summary).find("适配器摘要") != std::string::npos,
           "set_libraries: LiteRT-LM loads from the path given through the API");
    apexo_session_destroy(via_api);
    apexo_engine_state_destroy(by_api);
  }
  setenv("APEXO_SHERPA_LIBRARY", argv[1], 1);
  setenv("APEXO_LITERT_LIBRARY", argv[2], 1);

  // Libraries that cannot be loaded: errors, never invented text.
  setenv("APEXO_SHERPA_LIBRARY", "/no/such/libsherpa-onnx.dylib", 1);
  void* broken_state = nullptr;
  ApexoSession* broken = open_session(dir, gemma.c_str(), &broken_state);
  expect(apexo_session_push_pcm(broken, pcm.data(), static_cast<int32_t>(pcm.size()), 16000) != 0,
         "present asr model does not fall back when sherpa-onnx is missing");
  apexo_session_destroy(broken);
  apexo_engine_state_destroy(broken_state);

  setenv("APEXO_SHERPA_LIBRARY", argv[1], 1);
  setenv("APEXO_LITERT_LIBRARY", "/no/such/liblitert_lm.dylib", 1);
  void* nolib_state = nullptr;
  ApexoSession* nolib = open_session(dir, gemma.c_str(), &nolib_state);
  apexo_session_push_pcm(nolib, pcm.data(), static_cast<int32_t>(pcm.size()), 16000);
  char nolib_summary[256];
  std::memset(nolib_summary, 0, sizeof(nolib_summary));
  expect(apexo_session_summarize(nolib, nolib_summary, sizeof(nolib_summary)) != 0,
         "present gemma file does not fall back when LiteRT-LM is missing");
  expect(nolib_summary[0] == '\0' && apexo_session_gemma_resident(nolib) == 0, "missing library does not invent a summary");
  apexo_session_destroy(nolib);
  apexo_engine_state_destroy(nolib_state);

  remove_tree(dir);
  if (g_failures != 0) {
    std::cout << g_failures << " failure(s)\n";
    return 1;
  }
  std::cout << "all adapter assertions passed\n";
  return 0;
}
