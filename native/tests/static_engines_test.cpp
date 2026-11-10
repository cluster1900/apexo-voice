// Static engine mode (APEXO_STATIC_ENGINES), the mode used on iOS: the engines are linked into the program.
// Built together with fake_sherpa.c and fake_litert.c, which stand in for the linked engines.
#include "apexo_session.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

static int failures = 0;
static void expect(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) failures += 1;
}

static void touch(const std::string& path) { std::ofstream(path) << "x"; }

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  char tmpl[] = "apexo-static-XXXXXX";
  const std::string dir = mkdtemp(tmpl);
  for (const char* name : {"encoder.onnx", "decoder.onnx", "joiner.onnx", "tokens.txt"}) touch(dir + "/" + name);
  const std::string gemma = dir + "/gemma.litertlm";
  touch(gemma);

  void* state = apexo_engine_state_create(dir.c_str(), gemma.c_str());
  ApexoEngines engines;
  apexo_engine_state_bind(state, &engines);
  ApexoConfig config;
  std::memset(&config, 0, sizeof(config));
  config.summary_model_path = gemma.c_str();
  ApexoSession* session = apexo_session_create(&config, &engines);

  std::vector<float> pcm(1600, 0.1f);
  const int pushed = apexo_session_push_pcm(session, pcm.data(), 1600, 16000);
  char summary[256];
  expect(pushed == 0, "the recognizer is bound to the linked symbols");
  expect(std::string(apexo_session_full_transcript(session)).find("适配器转写") != std::string::npos,
         "the linked recognizer's text enters the transcript");
  setenv("APEXO_FAKE_NO_ENDPOINT", "1", 1);
  setenv("APEXO_FAKE_FINISH_TEXT", "静态引擎收尾词", 1);
  expect(apexo_session_push_pcm(session, pcm.data(), 1600, 16000) == 0 &&
             apexo_session_end_segment(session) == 0 &&
             std::string(apexo_session_full_transcript(session)).find("静态引擎收尾词") != std::string::npos,
         "static engine mode binds SherpaOnnxOnlineStreamInputFinished and flushes tail speech on end_segment");
  unsetenv("APEXO_FAKE_NO_ENDPOINT");
  unsetenv("APEXO_FAKE_FINISH_TEXT");
  expect(apexo_session_summarize(session, summary, sizeof(summary)) == 0 &&
             std::string(summary).find("适配器摘要") != std::string::npos,
         "the linked LiteRT-LM generates the summary");
  char text_sum[256];
  expect(apexo_session_summarize_text(session, "外部文本", text_sum, sizeof(text_sum)) == 0 &&
             std::string(text_sum).find("适配器摘要") != std::string::npos,
         "static engine mode summarizes external text via apexo_session_summarize_text");
  apexo_session_destroy(session);
  apexo_engine_state_destroy(state);

  std::system(("rm -rf '" + dir + "'").c_str());
  return failures == 0 ? 0 : 1;
}
