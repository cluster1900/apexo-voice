#include <stdio.h>
#include <stdlib.h>

static int g_settings;
static int g_engine;
static int g_conversation;
static int g_response;
static const char* g_json = "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"适配器摘要\"}]}";

static void note(const char* line) {
  const char* path = getenv("APEXO_FAKE_LOG");
  if (path == 0 || path[0] == '\0') return;
  FILE* file = fopen(path, "a");
  if (file == 0) return;
  fputs(line, file);
  fputc('\n', file);
  fclose(file);
}

int litert_lm_engine_settings_create(const char* model_path, const char* backend, const char* vision, const char* audio,
                                     void** out_settings) {
  (void)backend;
  (void)vision;
  (void)audio;
  char line[1024];
  snprintf(line, sizeof(line), "settings %s", model_path != 0 ? model_path : "");
  note(line);
  if (out_settings != 0) *out_settings = &g_settings;
  return 0;
}

void litert_lm_engine_settings_delete(void* settings) { (void)settings; }

int litert_lm_engine_create(const void* settings, void** out_engine) {
  (void)settings;
  note("engine_create");
  if (out_engine != 0) *out_engine = &g_engine;
  return 0;
}

void litert_lm_engine_delete(void* engine) {
  (void)engine;
  note("engine_delete");
}

int litert_lm_conversation_create(void* engine, void* config, void** out_conversation) {
  (void)engine;
  (void)config;
  note("conversation_create");
  if (out_conversation != 0) *out_conversation = &g_conversation;
  return 0;
}

void litert_lm_conversation_delete(void* conversation) { (void)conversation; }

int litert_lm_conversation_send_message(void* conversation, const char* message_json, const char* extra,
                                        const void* optional_args, void** out_response) {
  (void)conversation;
  (void)extra;
  (void)optional_args;
  note("send");
  const char* path = getenv("APEXO_FAKE_LOG");
  if (path != 0 && path[0] != '\0') {
    FILE* file = fopen(path, "a");
    if (file != 0) {
      fputs(message_json != 0 ? message_json : "", file);
      fputc('\n', file);
      fclose(file);
    }
  }
  if (out_response != 0) *out_response = &g_response;
  return 0;
}

void litert_lm_json_response_delete(void* response) { (void)response; }

int litert_lm_json_response_get_string(const void* response, const char** out_json) {
  (void)response;
  if (out_json != 0) *out_json = g_json;
  return 0;
}
