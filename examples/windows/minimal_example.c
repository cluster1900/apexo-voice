/* Minimal runnable C example for Windows (x64).
 *
 * Place the following runtime DLLs in the same directory as minimal_example.exe
 * (or pass explicit UTF-8 paths to apexo_engine_state_set_libraries):
 *   - apexo_session.dll
 *   - sherpa-onnx-c-api.dll
 *   - onnxruntime.dll
 *   - onnxruntime_providers_shared.dll
 *   - litert-lm.dll
 *
 * Demonstrates all four 0.2.0 scenarios:
 *   1. Summary-only (apexo_session_summarize_text on external text without an ASR model)
 *   2. ASR-only (speech recognition via apexo_session_push_pcm + apexo_session_end_segment)
 *   3. Rolling stage summary (apexo_session_summarize_new on finalized text)
 *   4. Full post-recording summary (apexo_session_summarize)
 *
 * Usage:
 *   minimal_example.exe <asr_model_dir> <gemma_model.litertlm> <speech_16k_mono.wav> [sherpa_dll_path] [litert_dll_path]
 */
#include "apexo_session.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

static FILE* open_wav_rb(const char* path) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  UINT cp = CP_UTF8;
  DWORD flags = MB_ERR_INVALID_CHARS;
  if (n <= 0) {
    cp = CP_ACP;
    flags = 0;
    n = MultiByteToWideChar(cp, flags, path, -1, NULL, 0);
  }
  if (n <= 0) return NULL;
  wchar_t* wpath = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
  if (!wpath) return NULL;
  MultiByteToWideChar(cp, flags, path, -1, wpath, n);
  FILE* fp = _wfopen(wpath, L"rb");
  free(wpath);
  return fp;
}
#else
static FILE* open_wav_rb(const char* path) { return fopen(path, "rb"); }
#endif

static float* read_wav_16k_mono(const char* path, int32_t* out_samples) {
  *out_samples = 0;
  FILE* fp = open_wav_rb(path);
  if (!fp) return NULL;
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return NULL;
  }
  long file_len = ftell(fp);
  if (file_len < 44 || fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return NULL;
  }
  unsigned char* bytes = (unsigned char*)malloc((size_t)file_len);
  if (!bytes || fread(bytes, 1, (size_t)file_len, fp) != (size_t)file_len) {
    free(bytes);
    fclose(fp);
    return NULL;
  }
  fclose(fp);
  if (memcmp(bytes, "RIFF", 4) != 0) {
    free(bytes);
    return NULL;
  }
  size_t pos = 12;
  while (pos + 8 <= (size_t)file_len) {
    uint32_t chunk_len = 0;
    memcpy(&chunk_len, bytes + pos + 4, 4);
    if (memcmp(bytes + pos, "data", 4) == 0) {
      size_t end = pos + 8 + chunk_len;
      if (end > (size_t)file_len) end = (size_t)file_len;
      int32_t count = (int32_t)((end - (pos + 8)) / 2);
      float* pcm = (float*)malloc((size_t)count * sizeof(float));
      if (!pcm) {
        free(bytes);
        return NULL;
      }
      for (int32_t i = 0; i < count; ++i) {
        int16_t s = 0;
        memcpy(&s, bytes + pos + 8 + (size_t)i * 2, 2);
        pcm[i] = (float)s / 32768.0f;
      }
      free(bytes);
      *out_samples = count;
      return pcm;
    }
    pos += 8 + chunk_len + (chunk_len & 1u);
  }
  free(bytes);
  return NULL;
}

static void on_transcript_event(void* user, const ApexoAsrEvent* event) {
  int* counter = (int*)user;
  if (counter && event) {
    *counter += 1;
  }
}

static int run_example(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr,
            "Usage: %s <asr_model_dir> <gemma_model.litertlm> <speech_16k_mono.wav> [sherpa_dll] [litert_dll]\n",
            argv[0]);
    return 2;
  }
  const char* asr_dir = argv[1];
  const char* gemma_path = argv[2];
  const char* wav_path = argv[3];
  const char* sherpa_dll = argc >= 5 ? argv[4] : NULL;
  const char* litert_dll = argc >= 6 ? argv[5] : NULL;

  int32_t total_samples = 0;
  float* pcm = read_wav_16k_mono(wav_path, &total_samples);
  if (!pcm || total_samples <= 0) {
    fprintf(stderr, "Failed to read 16 kHz mono WAV: %s\n", wav_path);
    free(pcm);
    return 1;
  }

  /* 1. Summary-only scenario (no ASR model needed). */
  {
    void* sum_only_state = apexo_engine_state_create("", gemma_path);
    if (sherpa_dll || litert_dll) {
      apexo_engine_state_set_libraries(sum_only_state, sherpa_dll, litert_dll);
    }
    ApexoEngines sum_only_engines;
    apexo_engine_state_bind(sum_only_state, &sum_only_engines);
    ApexoConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.summary_model_path = gemma_path;
    ApexoSession* sum_session = apexo_session_create(&cfg, &sum_only_engines);
    char sum_buf[65536];
    int rc = apexo_session_summarize_text(
        sum_session,
        "会议记录：原定周五上线2.0版本，经压测讨论决定取消周五上线，最终改到下周三下午发布。",
        sum_buf, (int32_t)sizeof(sum_buf));
    if (rc != 0) {
      fprintf(stderr, "[Summary-only] failed (%d): %s\n", rc, apexo_session_last_error(sum_session));
      apexo_session_destroy(sum_session);
      apexo_engine_state_destroy(sum_only_state);
      free(pcm);
      return 1;
    }
    printf("[1. Summary-only] %s\n", sum_buf);
    apexo_session_destroy(sum_session);
    apexo_engine_state_destroy(sum_only_state);
  }

  /* 2. ASR-only + 3. Rolling stage summary + 4. Full post-recording summary. */
  void* state = apexo_engine_state_create(asr_dir, gemma_path);
  if (sherpa_dll || litert_dll) {
    apexo_engine_state_set_libraries(state, sherpa_dll, litert_dll);
  }
  ApexoEngines engines;
  apexo_engine_state_bind(state, &engines);
  ApexoConfig config;
  memset(&config, 0, sizeof(config));
  config.merge_gap_ms = APEXO_MERGE_GAP_MS;
  config.summary_chunk_chars = APEXO_SUMMARY_CHUNK_CHARS;
  config.display_lines = APEXO_DISPLAY_LINES;
  config.summary_model_path = gemma_path;

  ApexoSession* session = apexo_session_create(&config, &engines);
  int event_count = 0;
  apexo_session_set_transcript_callback(session, on_transcript_event, &event_count);

  for (int32_t i = 0; i < total_samples; i += 1600) {
    int32_t n = total_samples - i < 1600 ? total_samples - i : 1600;
    int rc = apexo_session_push_pcm(session, pcm + i, n, APEXO_SAMPLE_RATE);
    if (rc != 0) {
      fprintf(stderr, "push_pcm failed (%d): %s\n", rc, apexo_session_last_error(session));
      apexo_session_destroy(session);
      apexo_engine_state_destroy(state);
      free(pcm);
      return 1;
    }
  }

  int end_rc = apexo_session_end_segment(session);
  if (end_rc != 0) {
    fprintf(stderr, "end_segment failed (%d): %s\n", end_rc, apexo_session_last_error(session));
    apexo_session_destroy(session);
    apexo_engine_state_destroy(state);
    free(pcm);
    return 1;
  }
  printf("[2. ASR-only] events=%d transcript=%s\n", event_count, apexo_session_full_transcript(session));

  char stage_buf[65536];
  int stage_rc = apexo_session_summarize_new(session, stage_buf, (int32_t)sizeof(stage_buf));
  if (stage_rc != 0) {
    fprintf(stderr, "summarize_new failed (%d): %s\n", stage_rc, apexo_session_last_error(session));
    apexo_session_destroy(session);
    apexo_engine_state_destroy(state);
    free(pcm);
    return 1;
  }
  printf("[3. Stage summary] %s\n", stage_buf);

  char full_buf[65536];
  int full_rc = apexo_session_summarize(session, full_buf, (int32_t)sizeof(full_buf));
  if (full_rc != 0) {
    fprintf(stderr, "summarize failed (%d): %s\n", full_rc, apexo_session_last_error(session));
    apexo_session_destroy(session);
    apexo_engine_state_destroy(state);
    free(pcm);
    return 1;
  }
  printf("[4. Full summary] %s\n", full_buf);

  apexo_session_reset(session);
  apexo_session_destroy(session);
  apexo_engine_state_destroy(state);
  free(pcm);
  return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  int wargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  if (wargv && wargc > 0) {
    char** u8argv = (char**)calloc((size_t)wargc + 1, sizeof(char*));
    if (u8argv) {
      int ok = 1;
      for (int i = 0; i < wargc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        if (n <= 0 || !(u8argv[i] = (char*)malloc((size_t)n))) {
          ok = 0;
          break;
        }
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, u8argv[i], n, NULL, NULL);
      }
      LocalFree(wargv);
      int rc = ok ? run_example(wargc, u8argv) : run_example(argc, argv);
      for (int i = 0; i < wargc; ++i) free(u8argv[i]);
      free(u8argv);
      return rc;
    }
    LocalFree(wargv);
  }
#endif
  return run_example(argc, argv);
}
