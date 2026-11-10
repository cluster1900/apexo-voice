// Runs the native adapter against the real sherpa-onnx C library and a real streaming model. Opt-in:
//
//   APEXO_SHERPA_LIBRARY=/path/to/libsherpa-onnx-c-api.dylib \
//   real_engine_test /path/to/model_dir /path/to/16k_mono_16bit.wav "expected text"
//
// model_dir holds encoder.onnx, decoder.onnx, joiner.onnx and tokens.txt. See run-tests.sh.
#include "apexo_session.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::vector<float> read_wav(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
  std::vector<float> out;
  for (size_t i = 44; i + 1 < bytes.size(); i += 2) {
    int16_t v;
    std::memcpy(&v, &bytes[i], 2);
    out.push_back(v / 32768.0f);
  }
  return out;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: real_engine_test model_dir wav expected_text\n");
    return 2;
  }
  int failures = 0;
  auto expect = [&](bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures += 1;
  };
  void* state = apexo_engine_state_create(argv[1], "/none/gemma.litertlm");
  ApexoEngines engines;
  apexo_engine_state_bind(state, &engines);
  ApexoConfig config;
  std::memset(&config, 0, sizeof(config));
  config.summary_model_path = "/none/gemma.litertlm";
  ApexoSession* session = apexo_session_create(&config, &engines);
  const std::vector<float> pcm = read_wav(argv[2]);
  // 1) Explicit finish without trailing silence: cut right at the end of speech (if wav has trailing silence) or feed
  // the wav directly, then call apexo_session_end_segment(session) to flush tail frames via InputFinished.
  size_t trim_len = pcm.size();
  while (trim_len > 1600 && std::abs(pcm[trim_len - 1]) < 0.005f) {
    --trim_len;
  }
  int bad = 0;
  for (size_t i = 0; i < trim_len; i += 1600) {
    bad += apexo_session_push_pcm(session, pcm.data() + i,
                                  static_cast<int32_t>(std::min<size_t>(1600, trim_len - i)), 16000) != 0;
  }
  const int end_rc = apexo_session_end_segment(session);
  const std::string finish_transcript = apexo_session_full_transcript(session);
  expect(bad == 0 && end_rc == 0, "the real sherpa-onnx library accepts every chunk and finishes via InputFinished");
  expect(finish_transcript.find(argv[3]) != std::string::npos,
         "explicit end_segment without long silence preserves the final word in the transcript");

  // 2) Reset session and verify silence-driven endpointing also produces no blank lines.
  apexo_session_reset(session);
  for (size_t i = 0; i < pcm.size(); i += 1600) {
    bad += apexo_session_push_pcm(session, pcm.data() + i,
                                  static_cast<int32_t>(std::min<size_t>(1600, pcm.size() - i)), 16000) != 0;
  }
  const std::vector<float> silence(1600, 0.0f);
  for (int i = 0; i < 48; ++i) bad += apexo_session_push_pcm(session, silence.data(), 1600, 16000) != 0;
  const std::string transcript = apexo_session_full_transcript(session);
  expect(transcript.find(argv[3]) != std::string::npos, "the real model's text enters the transcript");
  expect(transcript.find("\n\n") == std::string::npos && !transcript.empty() && transcript.back() != '\n',
         "silence after the speech adds no blank lines");
  apexo_session_destroy(session);
  apexo_engine_state_destroy(state);
  return failures == 0 ? 0 : 1;
}
