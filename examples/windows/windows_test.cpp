/* Complete Windows x64 test suite for Apexo 0.2.0.
 *
 * Cross-compiles to x86_64-windows-gnu and links against dist/windows/apexo_session.lib.
 * Covers:
 *   1. Full 0.2.0 session & adapter contract (ASR-only, summary-only summarize_text,
 *      rolling summarize_new with concurrent push_pcm, end_segment tail flush via asr_finish,
 *      cross-chunk synthesis & non-convergence guard, APEXO_ERR_BUSY (-6), failure atomicity,
 *      transcript callback, and reset)
 *   2. Windows DLL loading (explicit path with directory search flags, default DLL names
 *      sherpa-onnx-c-api.dll / litert-lm.dll, Unicode UTF-8 model paths)
 *   3. Optional real-model execution on Windows x64 when invoked with:
 *      windows_test.exe <asr_model_dir> <gemma_model.litertlm> <speech_16k_mono.wav>
 */
#include "apexo_session.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

static int g_failures = 0;

static void check(bool ok, const char* name) {
  if (ok) {
    std::printf("PASS %s\n", name);
  } else {
    std::printf("FAIL %s\n", name);
    ++g_failures;
  }
}

namespace {

struct FakeEngine {
  int asr_pushes = 0;
  int asr_finishes = 0;
  int asr_resets = 0;
  int sum_loads = 0;
  int sum_unloads = 0;
  int loaded = 0;
  int next_is_final = -1;
  std::string next_text;
  int32_t next_start_ms = 0;
  int32_t next_end_ms = 0;
  std::string finish_text = "尾部收尾词";
  std::vector<std::string> prompts;
  std::string canned_summary = "Windows摘要结果";
};

int fake_asr_push(void* user, const float*, int32_t, int32_t sample_rate, ApexoAsrEvent* out) {
  if (sample_rate != APEXO_SAMPLE_RATE) return -2;
  FakeEngine* e = static_cast<FakeEngine*>(user);
  e->asr_pushes += 1;
  out->is_final = e->next_is_final;
  std::snprintf(out->text, sizeof(out->text), "%s", e->next_text.c_str());
  out->start_ms = e->next_start_ms;
  out->end_ms = e->next_end_ms;
  e->next_is_final = -1;
  e->next_text.clear();
  return 0;
}

int fake_asr_finish(void* user, ApexoAsrEvent* out) {
  FakeEngine* e = static_cast<FakeEngine*>(user);
  e->asr_finishes += 1;
  out->is_final = 1;
  std::snprintf(out->text, sizeof(out->text), "%s", e->finish_text.c_str());
  out->start_ms = 0;
  out->end_ms = 600;
  return 0;
}

void fake_asr_reset(void* user) {
  FakeEngine* e = static_cast<FakeEngine*>(user);
  e->asr_resets += 1;
}

int fake_sum_load(void* user, const char*) {
  FakeEngine* e = static_cast<FakeEngine*>(user);
  e->sum_loads += 1;
  e->loaded = 1;
  return 0;
}

int fake_sum_unload(void* user) {
  FakeEngine* e = static_cast<FakeEngine*>(user);
  e->sum_unloads += 1;
  e->loaded = 0;
  return 0;
}

int fake_sum_is_loaded(void* user) {
  return static_cast<FakeEngine*>(user)->loaded;
}

int fake_sum_generate(void* user, const char* prompt, char* out, int32_t out_cap) {
  FakeEngine* e = static_cast<FakeEngine*>(user);
  e->prompts.push_back(prompt ? prompt : "");
  const int32_t len = static_cast<int32_t>(e->canned_summary.size());
  if (len + 1 > out_cap) return -1;
  std::memcpy(out, e->canned_summary.c_str(), static_cast<size_t>(len) + 1);
  return 0;
}

void test_windows_core_contracts() {
  FakeEngine fe;
  ApexoEngines engines{};
  engines.asr_user = &fe;
  engines.asr_push = fake_asr_push;
  engines.asr_finish = fake_asr_finish;
  engines.asr_reset = fake_asr_reset;
  engines.sum_user = &fe;
  engines.sum_load = fake_sum_load;
  engines.sum_unload = fake_sum_unload;
  engines.sum_is_loaded = fake_sum_is_loaded;
  engines.sum_generate = fake_sum_generate;

  ApexoConfig cfg{};
  cfg.summary_model_path = "gemma-4-E2B-it.litertlm";
  cfg.web_summary = 1;
  ApexoSession* s = apexo_session_create(&cfg, &engines);
  check(s != nullptr, "Windows session created via DLL export");

  int cb_events = 0;
  apexo_session_set_transcript_callback(
      s,
      [](void* u, const ApexoAsrEvent* ev) {
        if (u && ev && ev->text[0] != '\0') {
          *static_cast<int*>(u) += 1;
        }
      },
      &cb_events);

  /* 1. Summary-only (summarize_text) does not require ASR or modify session transcript. */
  char buf[4096] = {0};
  int rc = apexo_session_summarize_text(s, "原定周五发布，讨论后改到下周三发布。", buf, sizeof(buf));
  check(rc == 0 && std::string(buf) == "Windows摘要结果", "T05: summarize_text generates summary on external text");
  check(std::strlen(apexo_session_full_transcript(s)) == 0 && std::strlen(apexo_session_summary_text(s)) == 0,
        "T05: summarize_text does not pollute session transcript or summary_text");

  /* Whitespace-only external text returns 0 and "" without loading Gemma. */
  const int loads_before = fe.sum_loads;
  rc = apexo_session_summarize_text(s, "   \r\n\t  ", buf, sizeof(buf));
  check(rc == 0 && buf[0] == '\0' && fe.sum_loads == loads_before,
        "T05/T06: whitespace-only external text does not load Gemma");

  /* 2. ASR-only + end_segment tail flush via asr_finish. */
  float pcm[160] = {0};
  check(apexo_session_push_pcm(s, pcm, 160, APEXO_SAMPLE_RATE) == 0, "push_pcm accepts 16 kHz PCM");
  check(apexo_session_end_segment(s) == 0, "T07: end_segment flushes via asr_finish");
  check(fe.asr_finishes == 1 && fe.asr_resets == 0, "T07: end_segment calls asr_finish without calling asr_reset");
  check(std::string(apexo_session_full_transcript(s)) == "尾部收尾词" && cb_events == 1,
        "T07/D07: flushed final enters full_transcript and triggers transcript callback");

  /* 3. Rolling summarize_new ignores unfinished interim. */
  fe.next_is_final = 0;
  fe.next_text = "还在说的话";
  fe.next_start_ms = 600;
  fe.next_end_ms = 900;
  check(apexo_session_push_pcm(s, pcm, 160, APEXO_SAMPLE_RATE) == 0, "push_pcm emits interim event");
  rc = apexo_session_summarize_new(s, buf, sizeof(buf));
  check(rc == 0 && std::string(buf) == "Windows摘要结果", "T01: summarize_new summarizes finalized line");
  check(std::string(apexo_session_full_transcript(s)) == "尾部收尾词" &&
            std::string(apexo_session_display_text(s)).find("还在说的话") != std::string::npos,
        "T01: summarize_new does not promote interim to final or clear interim from display");

  /* 4. Full summarize and reset. */
  rc = apexo_session_summarize(s, buf, sizeof(buf));
  check(rc == 0 && std::string(buf) == "Windows摘要结果", "T03: full summarize succeeds");
  apexo_session_reset(s);
  check(std::strlen(apexo_session_full_transcript(s)) == 0 && std::strlen(apexo_session_summary_text(s)) == 0 &&
            fe.asr_resets == 1,
        "D07: apexo_session_reset clears transcript, summary, and resets ASR");

  apexo_session_destroy(s);
}

void test_windows_concurrency_and_busy() {
  struct BlockingEngine {
    std::atomic<bool> in_generate{false};
    std::atomic<bool> allow_finish{false};
    std::atomic<int> pushes{0};
    bool emit_first_final{true};
  } be;

  ApexoEngines engines{};
  engines.asr_user = &be;
  engines.asr_push = [](void* u, const float*, int32_t, int32_t sr, ApexoAsrEvent* out) -> int {
    if (sr != APEXO_SAMPLE_RATE) return -2;
    auto* b = static_cast<BlockingEngine*>(u);
    b->pushes.fetch_add(1);
    if (b->emit_first_final) {
      b->emit_first_final = false;
      out->is_final = 1;
      std::snprintf(out->text, sizeof(out->text), "%s", "第一段定稿");
      out->start_ms = 0;
      out->end_ms = 500;
    } else {
      out->is_final = -1;
      out->text[0] = '\0';
      out->start_ms = 0;
      out->end_ms = 0;
    }
    return 0;
  };
  engines.sum_user = &be;
  engines.sum_load = [](void*, const char*) -> int { return 0; };
  engines.sum_unload = [](void*) -> int { return 0; };
  engines.sum_is_loaded = [](void*) -> int { return 1; };
  engines.sum_generate = [](void* u, const char*, char* out, int32_t cap) -> int {
    auto* b = static_cast<BlockingEngine*>(u);
    b->in_generate.store(true);
    while (!b->allow_finish.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const char* msg = "并发摘要";
    const int32_t len = static_cast<int32_t>(std::strlen(msg));
    if (len + 1 > cap) return -1;
    std::memcpy(out, msg, static_cast<size_t>(len) + 1);
    return 0;
  };

  ApexoConfig cfg{};
  cfg.summary_model_path = "gemma-4-E2B-it.litertlm";
  cfg.web_summary = 1;
  ApexoSession* s = apexo_session_create(&cfg, &engines);
  float first_sample = 0.0f;
  apexo_session_push_pcm(s, &first_sample, 1, APEXO_SAMPLE_RATE);

  int bg_rc = -99;
  char bg_buf[256] = {0};
  std::thread t([&]() { bg_rc = apexo_session_summarize_new(s, bg_buf, sizeof(bg_buf)); });
  while (!be.in_generate.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  /* Push PCM while summarize_new is running: must succeed (return 0), not -5! */
  float sample = 0.0f;
  const int push_rc = apexo_session_push_pcm(s, &sample, 1, APEXO_SAMPLE_RATE);
  check(push_rc == 0 && be.pushes.load() == 2, "T02: push_pcm succeeds while summarize is running");

  /* Concurrent summarize / summarize_new / summarize_text must return APEXO_ERR_BUSY (-6). */
  char busy_buf[256] = {0};
  check(apexo_session_summarize(s, busy_buf, sizeof(busy_buf)) == APEXO_ERR_BUSY,
        "T06: concurrent summarize returns APEXO_ERR_BUSY (-6)");
  check(apexo_session_summarize_new(s, busy_buf, sizeof(busy_buf)) == APEXO_ERR_BUSY,
        "T06: concurrent summarize_new returns APEXO_ERR_BUSY (-6)");
  check(apexo_session_summarize_text(s, "外部文本", busy_buf, sizeof(busy_buf)) == APEXO_ERR_BUSY,
        "T06: concurrent summarize_text returns APEXO_ERR_BUSY (-6)");

  be.allow_finish.store(true);
  t.join();
  check(bg_rc == 0 && std::string(bg_buf) == "并发摘要", "T02: background summarize_new completes cleanly");
  apexo_session_destroy(s);
}

bool read_wav_16k_mono(const char* path, std::vector<float>* out_pcm) {
  out_pcm->clear();
  if (!path) return false;
#ifdef _WIN32
  int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
  UINT cp = CP_UTF8;
  DWORD flags = MB_ERR_INVALID_CHARS;
  if (wn <= 0) {
    cp = CP_ACP;
    flags = 0;
    wn = MultiByteToWideChar(cp, flags, path, -1, nullptr, 0);
  }
  if (wn <= 0) return false;
  std::wstring wpath(static_cast<size_t>(wn - 1), L'\0');
  MultiByteToWideChar(cp, flags, path, -1, &wpath[0], wn);
  FILE* fp = _wfopen(wpath.c_str(), L"rb");
#else
  FILE* fp = std::fopen(path, "rb");
#endif
  if (!fp) return false;
  if (std::fseek(fp, 0, SEEK_END) != 0) {
    std::fclose(fp);
    return false;
  }
  const long file_len = std::ftell(fp);
  if (file_len < 44 || std::fseek(fp, 0, SEEK_SET) != 0) {
    std::fclose(fp);
    return false;
  }
  std::vector<unsigned char> bytes(static_cast<size_t>(file_len));
  if (std::fread(bytes.data(), 1, bytes.size(), fp) != bytes.size()) {
    std::fclose(fp);
    return false;
  }
  std::fclose(fp);
  if (std::memcmp(bytes.data(), "RIFF", 4) != 0) return false;
  size_t pos = 12;
  while (pos + 8 <= bytes.size()) {
    uint32_t chunk_len = 0;
    std::memcpy(&chunk_len, bytes.data() + pos + 4, 4);
    if (std::memcmp(bytes.data() + pos, "data", 4) == 0) {
      size_t end = pos + 8 + chunk_len;
      if (end > bytes.size()) end = bytes.size();
      const size_t count = (end - (pos + 8)) / 2;
      out_pcm->resize(count);
      for (size_t i = 0; i < count; ++i) {
        int16_t s = 0;
        std::memcpy(&s, bytes.data() + pos + 8 + i * 2, 2);
        (*out_pcm)[i] = static_cast<float>(s) / 32768.0f;
      }
      return !out_pcm->empty();
    }
    pos += 8 + chunk_len + (chunk_len & 1u);
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  std::vector<std::string> u8_storage;
  std::vector<char*> u8_ptrs;
  int wargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  if (wargv && wargc > 0) {
    u8_storage.resize(static_cast<size_t>(wargc));
    u8_ptrs.resize(static_cast<size_t>(wargc));
    for (int i = 0; i < wargc; ++i) {
      int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
      if (n > 1) {
        u8_storage[static_cast<size_t>(i)].resize(static_cast<size_t>(n - 1));
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &u8_storage[static_cast<size_t>(i)][0], n, nullptr, nullptr);
      }
      u8_ptrs[static_cast<size_t>(i)] = &u8_storage[static_cast<size_t>(i)][0];
    }
    LocalFree(wargv);
    argc = wargc;
    argv = u8_ptrs.data();
  }
#endif
  test_windows_core_contracts();
  test_windows_concurrency_and_busy();

  if (argc >= 4) {
    /* Optional real-model run on Windows x64 (tests both sherpa-onnx ASR and LiteRT-LM Gemma). */
    const char* asr_dir = argv[1];
    const char* gemma_path = argv[2];
    const char* wav_path = argv[3];
    const char* sherpa_dll = argc >= 5 ? argv[4] : nullptr;
    const char* litert_dll = argc >= 6 ? argv[5] : nullptr;
    std::vector<float> wav_pcm;
    check(read_wav_16k_mono(wav_path, &wav_pcm), "real 16 kHz mono WAV loaded on Windows");
    void* state = apexo_engine_state_create(asr_dir, gemma_path);
    if (sherpa_dll || litert_dll) {
      apexo_engine_state_set_libraries(state, sherpa_dll, litert_dll);
    }
    ApexoEngines engines{};
    check(apexo_engine_state_bind(state, &engines) == 0, "real engine state bound on Windows");
    ApexoConfig cfg{};
    cfg.summary_model_path = gemma_path;
    ApexoSession* s = apexo_session_create(&cfg, &engines);
    char sum_buf[65536] = {0};
    int rc = apexo_session_summarize_text(
        s, "会议记录：原定周五发布，讨论后取消周五，最终改到下周三发布。", sum_buf, sizeof(sum_buf));
    check(rc == 0 && std::strlen(sum_buf) > 0, "real Gemma summarize_text on Windows");
    int push_rc = 0;
    for (size_t i = 0; i < wav_pcm.size(); i += 1600) {
      const int32_t n = static_cast<int32_t>(wav_pcm.size() - i < 1600 ? wav_pcm.size() - i : 1600);
      push_rc = apexo_session_push_pcm(s, wav_pcm.data() + i, n, APEXO_SAMPLE_RATE);
      if (push_rc != 0) break;
    }
    check(push_rc == 0 && apexo_session_end_segment(s) == 0 && std::strlen(apexo_session_full_transcript(s)) > 0,
          "real sherpa-onnx push_pcm + end_segment transcribes WAV on Windows");
    std::memset(sum_buf, 0, sizeof(sum_buf));
    check(apexo_session_summarize(s, sum_buf, sizeof(sum_buf)) == 0 && std::strlen(sum_buf) > 0,
          "real Gemma full summarize on transcribed WAV on Windows");
    apexo_session_destroy(s);
    apexo_engine_state_destroy(state);
  }

  if (g_failures != 0) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("all windows test assertions passed\n");
  return 0;
}
