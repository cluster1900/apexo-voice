#include "DemoSupport.h"

#include <string.h>

static void* g_state = 0;

ApexoSession* DemoCreateSession(const char* model_path) {
  ApexoConfig config;
  memset(&config, 0, sizeof(config));
  config.merge_gap_ms = APEXO_MERGE_GAP_MS;
  config.summary_chunk_chars = APEXO_SUMMARY_CHUNK_CHARS;
  config.display_lines = APEXO_DISPLAY_LINES;
  config.summary_model_path = model_path;
  config.web_summary = 0;
  if (g_state != 0) {
    apexo_engine_state_destroy(g_state);
    g_state = 0;
  }
  g_state = apexo_engine_state_create(0, model_path);
  ApexoEngines engines;
  if (apexo_engine_state_bind(g_state, &engines) != 0) return 0;
  return apexo_session_create(&config, &engines);
}

void DemoDestroyEngines(void) {
  if (g_state == 0) return;
  apexo_engine_state_destroy(g_state);
  g_state = 0;
}
