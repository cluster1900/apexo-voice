#ifndef APEXO_SESSION_H
#define APEXO_SESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(APEXO_BUILDING)
#define APEXO_API __declspec(dllexport)
#elif defined(APEXO_STATIC_ENGINES) || defined(APEXO_STATIC)
#define APEXO_API
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

#define APEXO_ERR_BUSY (-6)

typedef struct ApexoSession ApexoSession;

typedef struct ApexoAsrEvent {
  char text[4096];
  int32_t is_final; /* 1 final, 0 interim, -1 no event */
  int32_t start_ms;
  int32_t end_ms;
} ApexoAsrEvent;

typedef void (*ApexoTranscriptCallback)(void* user, const ApexoAsrEvent* event);

typedef struct ApexoEngines {
  void* asr_user;
  int (*asr_push)(void* user, const float* pcm, int32_t count, int32_t sample_rate, ApexoAsrEvent* out);
  void (*asr_reset)(void* user);
  void* sum_user;
  int (*sum_is_loaded)(void* user);
  int (*sum_load)(void* user, const char* model_path);
  int (*sum_unload)(void* user);
  int (*sum_generate)(void* user, const char* prompt, char* out, int32_t out_cap);
  int (*asr_finish)(void* user, ApexoAsrEvent* out);
} ApexoEngines;

typedef struct ApexoConfig {
  int32_t merge_gap_ms;
  int32_t summary_chunk_chars;
  int32_t display_lines;
  const char* summary_model_path;
  int32_t web_summary; /* 1: the model is supplied by a browser runtime, so a model name is required (a path
                        * that is only a label is fine). 0: native; an empty path is reported by summarize as -3.
                        * Either way a non-empty path must end in .litertlm. */
} ApexoConfig;

APEXO_API ApexoSession* apexo_session_create(const ApexoConfig* config, const ApexoEngines* engines);
APEXO_API void apexo_session_destroy(ApexoSession* session);
APEXO_API void apexo_session_set_transcript_callback(ApexoSession* session, ApexoTranscriptCallback callback,
                                                     void* user);
APEXO_API int apexo_session_push_pcm(ApexoSession* session, const float* pcm, int32_t count, int32_t sample_rate);
APEXO_API int apexo_session_end_segment(ApexoSession* session);
APEXO_API int apexo_session_gemma_resident(const ApexoSession* session);
APEXO_API const char* apexo_session_display_text(const ApexoSession* session);
APEXO_API const char* apexo_session_full_transcript(const ApexoSession* session);
APEXO_API const char* apexo_session_summary_text(const ApexoSession* session);
/* Summarizes the snapshot of finalized transcript (full_transcript) without modifying interim text, ASR state,
 * or blocking push_pcm. Replaces summary_text and advances the incremental cursor to the snapshot end. */
APEXO_API int apexo_session_summarize(ApexoSession* session, char* out, int32_t out_cap);
/* Summarizes caller-supplied UTF-8 text without reading or modifying full_transcript, summary_text, incremental
 * cursor, interim text, or ASR state. Empty string writes "" and returns 0 without loading the model. */
APEXO_API int apexo_session_summarize_text(ApexoSession* session, const char* text, char* out, int32_t out_cap);
/* Summarizes the finalized transcript added since the last summary snapshot and appends to summary_text.
 * Does not modify interim text, reset ASR, or block live push_pcm calls while running. */
APEXO_API int apexo_session_summarize_new(ApexoSession* session, char* out, int32_t out_cap);
/* Clears transcript, summary, incremental cursor and interim state, and resets ASR timeline to 0 while keeping
 * any loaded recognizer ready for the next recording. */
APEXO_API void apexo_session_reset(ApexoSession* session);
/* Compatibility alias for apexo_session_reset. */
APEXO_API void apexo_session_release(ApexoSession* session);
APEXO_API const char* apexo_session_last_error(const ApexoSession* session);

/* Default engines dlopen sherpa-onnx and LiteRT-LM (LiteRT-LM C API 1.0.0 or newer).
 *
 * asr_model_dir must hold encoder.onnx, decoder.onnx, joiner.onnx and tokens.txt when ASR is used; pass NULL or ""
 * for summary-only usage. If any ASR file is missing when push_pcm is called, push_pcm returns -3 and the transcript
 * stays empty. Nothing is invented. A missing summary model file makes summarize return -3 as well.
 *
 * Threading: one session may be used from several threads (for example capture on one, summarize on another).
 * Live push_pcm continues while Gemma runs; only one summary call (summarize, summarize_text, or summarize_new)
 * runs at a time, and concurrent summary calls return APEXO_ERR_BUSY (-6). summarize blocks its caller while the
 * model runs, so do not call it on a UI thread. Strings returned by display_text, full_transcript, summary_text
 * and last_error stay valid until the next call that changes the session. */
APEXO_API void* apexo_engine_state_create(const char* asr_model_dir, const char* gemma_path);
APEXO_API void apexo_engine_state_destroy(void* state);
/* Where to dlopen the engines from, instead of the default names or APEXO_SHERPA_LIBRARY and
 * APEXO_LITERT_LIBRARY. Call it right after apexo_engine_state_create, before the first push_pcm or summarize.
 * Pass NULL or "" to keep the default for one of them. On iOS pass the path of the embedded, signed framework
 * binary (for example <app>/Frameworks/sherpa-onnx.framework/sherpa-onnx). Ignored when the library was built with
 * -DAPEXO_STATIC_ENGINES, which links the engines into the host program instead. */
APEXO_API void apexo_engine_state_set_libraries(void* state, const char* sherpa_library, const char* litert_library);
APEXO_API int apexo_engine_state_bind(void* state, ApexoEngines* out);

#ifdef __cplusplus
}
#endif

#endif
