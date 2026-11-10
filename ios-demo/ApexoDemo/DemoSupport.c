#include "DemoSupport.h"

#include <string.h>

/* Each session owns its engine state, so a session still finishing a summary keeps its engines even when a new
 * session is opened meanwhile. */
ApexoSession* DemoCreateSessionWithLibraries(const char* asr_model_dir, const char* model_path,
                                             const char* sherpa_library_path, const char* litert_library_path,
                                             void** engines_out) {
  ApexoConfig config;
  memset(&config, 0, sizeof(config));
  config.merge_gap_ms = APEXO_MERGE_GAP_MS;
  config.summary_chunk_chars = APEXO_SUMMARY_CHUNK_CHARS;
  config.display_lines = APEXO_DISPLAY_LINES;
  config.summary_model_path = model_path;
  config.web_summary = 0;
  *engines_out = 0;
  void* state = apexo_engine_state_create(asr_model_dir, model_path);
  apexo_engine_state_set_libraries(state, sherpa_library_path, litert_library_path);
  ApexoEngines engines;
  if (apexo_engine_state_bind(state, &engines) != 0) {
    apexo_engine_state_destroy(state);
    return 0;
  }
  ApexoSession* session = apexo_session_create(&config, &engines);
  if (session == 0) {
    apexo_engine_state_destroy(state);
    return 0;
  }
  *engines_out = state;
  return session;
}

ApexoSession* DemoCreateSession(const char* asr_model_dir, const char* model_path, void** engines_out) {
  return DemoCreateSessionWithLibraries(asr_model_dir, model_path, 0, 0, engines_out);
}

void DemoDestroyEngines(void* engines) {
  if (engines != 0) apexo_engine_state_destroy(engines);
}
