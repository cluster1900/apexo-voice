#ifndef APEXO_SESSION_H
#define APEXO_SESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(APEXO_BUILDING)
#define APEXO_API __declspec(dllexport)
#else
#define APEXO_API __declspec(dllimport)
#endif
#else
#define APEXO_API __attribute__((visibility("default")))
#endif

#define APEXO_SAMPLE_RATE 16000
#define APEXO_MERGE_GAP_MS 2000
#define APEXO_SUMMARY_CHUNK_CHARS 1500
#define APEXO_DISPLAY_LINES 4
#define APEXO_DISPLAY_WORDS 200

typedef struct ApexoSession ApexoSession;

typedef struct ApexoAsrEvent {
  char text[4096];
  int32_t is_final; /* 1 final, 0 interim, -1 no event */
  int32_t start_ms;
  int32_t end_ms;
} ApexoAsrEvent;

typedef struct ApexoEngines {
  void* asr_user;
  int (*asr_push)(void* user, const float* pcm, int32_t count, int32_t sample_rate, ApexoAsrEvent* out);
  void (*asr_reset)(void* user);
  void* sum_user;
  int (*sum_is_loaded)(void* user);
  int (*sum_load)(void* user, const char* model_path);
  int (*sum_unload)(void* user);
  int (*sum_generate)(void* user, const char* prompt, char* out, int32_t out_cap);
} ApexoEngines;

typedef struct ApexoConfig {
  int32_t merge_gap_ms;
  int32_t summary_chunk_chars;
  int32_t display_lines;
  const char* summary_model_path;
  int32_t web_summary; /* 1: require a web Gemma file; 0: reject a web Gemma file */
} ApexoConfig;

APEXO_API ApexoSession* apexo_session_create(const ApexoConfig* config, const ApexoEngines* engines);
APEXO_API void apexo_session_destroy(ApexoSession* session);
APEXO_API int apexo_session_push_pcm(ApexoSession* session, const float* pcm, int32_t count, int32_t sample_rate);
APEXO_API int apexo_session_end_segment(ApexoSession* session);
APEXO_API int apexo_session_gemma_resident(const ApexoSession* session);
APEXO_API const char* apexo_session_display_text(const ApexoSession* session);
APEXO_API const char* apexo_session_full_transcript(const ApexoSession* session);
APEXO_API int apexo_session_summarize(ApexoSession* session, char* out, int32_t out_cap);
APEXO_API void apexo_session_release(ApexoSession* session);
APEXO_API const char* apexo_session_last_error(const ApexoSession* session);

/* Default engines dlopen sherpa-onnx and LiteRT-LM. A missing model file may
 * emit canned text. A present model file is never replaced with canned text. */
APEXO_API void* apexo_engine_state_create(const char* asr_model_dir, const char* gemma_path);
APEXO_API void apexo_engine_state_destroy(void* state);
APEXO_API int apexo_engine_state_bind(void* state, ApexoEngines* out);

#ifdef __cplusplus
}
#endif

#endif
