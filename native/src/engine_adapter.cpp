#include "apexo_session.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <sys/stat.h>

namespace {

struct SherpaOnlineTransducer {
  const char* encoder;
  const char* decoder;
  const char* joiner;
};

struct SherpaOnlineParaformer {
  const char* encoder;
  const char* decoder;
};

struct SherpaOnlineZipformer2Ctc {
  const char* model;
};

struct SherpaOnlineNemoCtc {
  const char* model;
};

struct SherpaOnlineToneCtc {
  const char* model;
};

struct SherpaOnlineModelConfig {
  SherpaOnlineTransducer transducer;
  SherpaOnlineParaformer paraformer;
  SherpaOnlineZipformer2Ctc zipformer2_ctc;
  const char* tokens;
  int32_t num_threads;
  const char* provider;
  int32_t debug;
  const char* model_type;
  const char* modeling_unit;
  const char* bpe_vocab;
  const char* tokens_buf;
  int32_t tokens_buf_size;
  SherpaOnlineNemoCtc nemo_ctc;
  SherpaOnlineToneCtc t_one_ctc;
};

struct SherpaFeatureConfig {
  int32_t sample_rate;
  int32_t feature_dim;
};

struct SherpaOnlineCtcFstDecoderConfig {
  const char* graph;
  int32_t max_active;
};

struct SherpaHomophoneReplacerConfig {
  const char* dict_dir;
  const char* lexicon;
  const char* rule_fsts;
};

/* Layout matches sherpa-onnx v2.0.1 SherpaOnnxOnlineRecognizerConfig. */
struct SherpaOnlineRecognizerConfig {
  SherpaFeatureConfig feat_config;
  SherpaOnlineModelConfig model_config;
  const char* decoding_method;
  int32_t max_active_paths;
  int32_t enable_endpoint;
  float rule1_min_trailing_silence;
  float rule2_min_trailing_silence;
  float rule3_min_utterance_length;
  const char* hotwords_file;
  float hotwords_score;
  SherpaOnlineCtcFstDecoderConfig ctc_fst_decoder_config;
  const char* rule_fsts;
  const char* rule_fars;
  float blank_penalty;
  const char* hotwords_buf;
  int32_t hotwords_buf_size;
  SherpaHomophoneReplacerConfig hr;
};

struct SherpaOnlineRecognizerResult {
  const char* text;
  const char* tokens;
  const char* const* tokens_arr;
  float* timestamps;
  int32_t count;
  const char* json;
};

struct SherpaOnlineRecognizer;
struct SherpaOnlineStream;

struct SherpaSileroVadModelConfig {
  const char* model;
  float threshold;
  float min_silence_duration;
  float min_speech_duration;
  int32_t window_size;
  float max_speech_duration;
};

struct SherpaTenVadModelConfig {
  const char* model;
  float threshold;
  float min_silence_duration;
  float min_speech_duration;
  int32_t window_size;
  float max_speech_duration;
};

struct SherpaVadModelConfig {
  SherpaSileroVadModelConfig silero_vad;
  int32_t sample_rate;
  int32_t num_threads;
  const char* provider;
  int32_t debug;
  SherpaTenVadModelConfig ten_vad;
};

struct SherpaVoiceActivityDetector;

using CreateOnlineRecognizerFn = const SherpaOnlineRecognizer* (*)(const SherpaOnlineRecognizerConfig*);
using DestroyOnlineRecognizerFn = void (*)(const SherpaOnlineRecognizer*);
using CreateOnlineStreamFn = const SherpaOnlineStream* (*)(const SherpaOnlineRecognizer*);
using DestroyOnlineStreamFn = void (*)(const SherpaOnlineStream*);
using AcceptWaveformFn = void (*)(const SherpaOnlineStream*, int32_t, const float*, int32_t);
using IsReadyFn = int32_t (*)(const SherpaOnlineRecognizer*, const SherpaOnlineStream*);
using DecodeFn = void (*)(const SherpaOnlineRecognizer*, const SherpaOnlineStream*);
using GetResultFn = const SherpaOnlineRecognizerResult* (*)(const SherpaOnlineRecognizer*, const SherpaOnlineStream*);
using DestroyResultFn = void (*)(const SherpaOnlineRecognizerResult*);
using IsEndpointFn = int32_t (*)(const SherpaOnlineRecognizer*, const SherpaOnlineStream*);
using ResetStreamFn = void (*)(const SherpaOnlineRecognizer*, const SherpaOnlineStream*);
using CreateVadFn = const SherpaVoiceActivityDetector* (*)(const SherpaVadModelConfig*, float);
using DestroyVadFn = void (*)(const SherpaVoiceActivityDetector*);
using VadAcceptFn = void (*)(const SherpaVoiceActivityDetector*, const float*, int32_t);

struct LiteRtSettings;
struct LiteRtEngine;
struct LiteRtConversation;
struct LiteRtJsonResponse;

using SettingsCreateFn = int (*)(const char*, const char*, const char*, const char*, LiteRtSettings**);
using SettingsDeleteFn = void (*)(LiteRtSettings*);
using EngineCreateFn = int (*)(const LiteRtSettings*, LiteRtEngine**);
using EngineDeleteFn = void (*)(LiteRtEngine*);
using ConversationCreateFn = int (*)(LiteRtEngine*, void*, LiteRtConversation**);
using ConversationDeleteFn = void (*)(LiteRtConversation*);
using SendMessageFn = int (*)(LiteRtConversation*, const char*, const char*, const void*, LiteRtJsonResponse**);
using JsonDeleteFn = void (*)(LiteRtJsonResponse*);
using JsonGetFn = int (*)(const LiteRtJsonResponse*, const char**);

struct SherpaApi {
  void* library = nullptr;
  CreateOnlineRecognizerFn create_recognizer = nullptr;
  DestroyOnlineRecognizerFn destroy_recognizer = nullptr;
  CreateOnlineStreamFn create_stream = nullptr;
  DestroyOnlineStreamFn destroy_stream = nullptr;
  AcceptWaveformFn accept_waveform = nullptr;
  IsReadyFn is_ready = nullptr;
  DecodeFn decode = nullptr;
  GetResultFn get_result = nullptr;
  DestroyResultFn destroy_result = nullptr;
  IsEndpointFn is_endpoint = nullptr;
  ResetStreamFn reset_stream = nullptr;
  CreateVadFn create_vad = nullptr;
  DestroyVadFn destroy_vad = nullptr;
  VadAcceptFn vad_accept = nullptr;
};

struct LitertApi {
  void* library = nullptr;
  SettingsCreateFn settings_create = nullptr;
  SettingsDeleteFn settings_delete = nullptr;
  EngineCreateFn engine_create = nullptr;
  EngineDeleteFn engine_delete = nullptr;
  ConversationCreateFn conversation_create = nullptr;
  ConversationDeleteFn conversation_delete = nullptr;
  SendMessageFn send_message = nullptr;
  JsonDeleteFn json_delete = nullptr;
  JsonGetFn json_get = nullptr;
};

enum class AsrMode { kUnknown, kCanned, kLive, kFailed };

struct EngineState {
  std::string asr_dir;
  std::string tokens_path;
  std::string silero_path;
  std::string encoder_path;
  std::string decoder_path;
  std::string joiner_path;
  int asr_calls = 0;
  int64_t samples = 0;
  AsrMode asr_mode = AsrMode::kUnknown;
  SherpaApi sherpa;
  const SherpaOnlineRecognizer* recognizer = nullptr;
  const SherpaOnlineStream* stream = nullptr;
  const SherpaVoiceActivityDetector* vad = nullptr;
  LitertApi litert;
  LiteRtSettings* settings = nullptr;
  LiteRtEngine* engine = nullptr;
  LiteRtConversation* conversation = nullptr;
  bool gemma_file_present = false;
};

void* open_library(const char* path) {
  if (path == nullptr || path[0] == '\0') return nullptr;
#if defined(_WIN32)
  return reinterpret_cast<void*>(LoadLibraryA(path));
#else
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* lookup(void* library, const char* name) {
  if (library == nullptr || name == nullptr) return nullptr;
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
  return dlsym(library, name);
#endif
}

void close_library(void* library) {
  if (library == nullptr) return;
#if defined(_WIN32)
  FreeLibrary(reinterpret_cast<HMODULE>(library));
#else
  dlclose(library);
#endif
}

bool path_exists(const std::string& path) {
  if (path.empty()) return false;
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

std::string join_path(const std::string& dir, const char* name) {
  if (dir.empty()) return name;
  if (dir.back() == '/' || dir.back() == '\\') return dir + name;
  return dir + "/" + name;
}

const char* env_or(const char* key, const char* fallback) {
  const char* value = std::getenv(key);
  if (value != nullptr && value[0] != '\0') return value;
  return fallback;
}

const char* sherpa_library_name() {
#if defined(_WIN32)
  return env_or("APEXO_SHERPA_LIBRARY", "sherpa-onnx.dll");
#elif defined(__APPLE__)
  return env_or("APEXO_SHERPA_LIBRARY", "libsherpa-onnx.dylib");
#else
  return env_or("APEXO_SHERPA_LIBRARY", "libsherpa-onnx.so");
#endif
}

const char* litert_library_name() {
#if defined(_WIN32)
  return env_or("APEXO_LITERT_LIBRARY", "litert_lm.dll");
#elif defined(__APPLE__)
  return env_or("APEXO_LITERT_LIBRARY", "liblitert_lm.dylib");
#else
  return env_or("APEXO_LITERT_LIBRARY", "liblitert_lm.so");
#endif
}

bool load_sherpa(SherpaApi* api, bool need_vad) {
  api->library = open_library(sherpa_library_name());
  if (api->library == nullptr) return false;
  api->create_recognizer = reinterpret_cast<CreateOnlineRecognizerFn>(lookup(api->library, "SherpaOnnxCreateOnlineRecognizer"));
  api->destroy_recognizer = reinterpret_cast<DestroyOnlineRecognizerFn>(lookup(api->library, "SherpaOnnxDestroyOnlineRecognizer"));
  api->create_stream = reinterpret_cast<CreateOnlineStreamFn>(lookup(api->library, "SherpaOnnxCreateOnlineStream"));
  api->destroy_stream = reinterpret_cast<DestroyOnlineStreamFn>(lookup(api->library, "SherpaOnnxDestroyOnlineStream"));
  api->accept_waveform = reinterpret_cast<AcceptWaveformFn>(lookup(api->library, "SherpaOnnxOnlineStreamAcceptWaveform"));
  api->is_ready = reinterpret_cast<IsReadyFn>(lookup(api->library, "SherpaOnnxIsOnlineStreamReady"));
  api->decode = reinterpret_cast<DecodeFn>(lookup(api->library, "SherpaOnnxDecodeOnlineStream"));
  api->get_result = reinterpret_cast<GetResultFn>(lookup(api->library, "SherpaOnnxGetOnlineStreamResult"));
  api->destroy_result = reinterpret_cast<DestroyResultFn>(lookup(api->library, "SherpaOnnxDestroyOnlineRecognizerResult"));
  api->is_endpoint = reinterpret_cast<IsEndpointFn>(lookup(api->library, "SherpaOnnxOnlineStreamIsEndpoint"));
  api->reset_stream = reinterpret_cast<ResetStreamFn>(lookup(api->library, "SherpaOnnxOnlineStreamReset"));
  api->create_vad = reinterpret_cast<CreateVadFn>(lookup(api->library, "SherpaOnnxCreateVoiceActivityDetector"));
  api->destroy_vad = reinterpret_cast<DestroyVadFn>(lookup(api->library, "SherpaOnnxDestroyVoiceActivityDetector"));
  api->vad_accept = reinterpret_cast<VadAcceptFn>(lookup(api->library, "SherpaOnnxVoiceActivityDetectorAcceptWaveform"));
  const bool asr_ok = api->create_recognizer && api->destroy_recognizer && api->create_stream && api->destroy_stream &&
                      api->accept_waveform && api->is_ready && api->decode && api->get_result && api->destroy_result &&
                      api->is_endpoint && api->reset_stream;
  if (!asr_ok) return false;
  if (need_vad && (api->create_vad == nullptr || api->destroy_vad == nullptr || api->vad_accept == nullptr)) return false;
  return true;
}

bool load_litert(LitertApi* api) {
  api->library = open_library(litert_library_name());
  if (api->library == nullptr) return false;
  api->settings_create = reinterpret_cast<SettingsCreateFn>(lookup(api->library, "litert_lm_engine_settings_create"));
  api->settings_delete = reinterpret_cast<SettingsDeleteFn>(lookup(api->library, "litert_lm_engine_settings_delete"));
  api->engine_create = reinterpret_cast<EngineCreateFn>(lookup(api->library, "litert_lm_engine_create"));
  api->engine_delete = reinterpret_cast<EngineDeleteFn>(lookup(api->library, "litert_lm_engine_delete"));
  api->conversation_create = reinterpret_cast<ConversationCreateFn>(lookup(api->library, "litert_lm_conversation_create"));
  api->conversation_delete = reinterpret_cast<ConversationDeleteFn>(lookup(api->library, "litert_lm_conversation_delete"));
  api->send_message = reinterpret_cast<SendMessageFn>(lookup(api->library, "litert_lm_conversation_send_message"));
  api->json_delete = reinterpret_cast<JsonDeleteFn>(lookup(api->library, "litert_lm_json_response_delete"));
  api->json_get = reinterpret_cast<JsonGetFn>(lookup(api->library, "litert_lm_json_response_get_string"));
  return api->settings_create && api->settings_delete && api->engine_create && api->engine_delete &&
         api->conversation_create && api->conversation_delete && api->send_message && api->json_delete && api->json_get;
}

void unload_gemma(EngineState* state) {
  if (state->conversation != nullptr && state->litert.conversation_delete != nullptr) {
    state->litert.conversation_delete(state->conversation);
  }
  state->conversation = nullptr;
  if (state->engine != nullptr && state->litert.engine_delete != nullptr) state->litert.engine_delete(state->engine);
  state->engine = nullptr;
  if (state->settings != nullptr && state->litert.settings_delete != nullptr) state->litert.settings_delete(state->settings);
  state->settings = nullptr;
}

void copy_text(ApexoAsrEvent* out, const char* text) {
  if (text == nullptr) text = "";
  std::strncpy(out->text, text, sizeof(out->text) - 1);
  out->text[sizeof(out->text) - 1] = '\0';
}

void canned_asr(EngineState* state, ApexoAsrEvent* out) {
  state->asr_calls += 1;
  if (state->asr_calls == 1) {
    copy_text(out, "你好，这是已经成稿的一句。");
    out->is_final = 1;
    out->start_ms = 0;
    out->end_ms = 800;
    return;
  }
  copy_text(out, "下一句还在识别");
  out->is_final = 0;
  out->start_ms = 4000;
  out->end_ms = 4400;
}

int ensure_asr(EngineState* state) {
  if (state->asr_mode == AsrMode::kLive || state->asr_mode == AsrMode::kCanned) return 0;
  if (state->asr_mode == AsrMode::kFailed) return -5;
  if (!path_exists(state->asr_dir)) {
    state->asr_mode = AsrMode::kCanned;
    return 0;
  }
  const bool need_vad = path_exists(state->silero_path);
  if (!load_sherpa(&state->sherpa, need_vad)) {
    state->asr_mode = AsrMode::kFailed;
    return -5;
  }
  SherpaOnlineRecognizerConfig config;
  std::memset(&config, 0, sizeof(config));
  config.feat_config.sample_rate = APEXO_SAMPLE_RATE;
  config.feat_config.feature_dim = 80;
  config.model_config.tokens = state->tokens_path.c_str();
  config.model_config.num_threads = 1;
  config.model_config.provider = "cpu";
  config.decoding_method = "greedy_search";
  config.enable_endpoint = 1;
  if (path_exists(state->encoder_path) && path_exists(state->decoder_path) && path_exists(state->joiner_path)) {
    config.model_config.transducer.encoder = state->encoder_path.c_str();
    config.model_config.transducer.decoder = state->decoder_path.c_str();
    config.model_config.transducer.joiner = state->joiner_path.c_str();
  }
  state->recognizer = state->sherpa.create_recognizer(&config);
  if (state->recognizer == nullptr) {
    state->asr_mode = AsrMode::kFailed;
    return -5;
  }
  state->stream = state->sherpa.create_stream(state->recognizer);
  if (state->stream == nullptr) {
    state->asr_mode = AsrMode::kFailed;
    return -5;
  }
  if (need_vad) {
    SherpaVadModelConfig vad_config;
    std::memset(&vad_config, 0, sizeof(vad_config));
    vad_config.silero_vad.model = state->silero_path.c_str();
    vad_config.silero_vad.threshold = 0.5f;
    vad_config.silero_vad.min_silence_duration = 0.5f;
    vad_config.silero_vad.min_speech_duration = 0.25f;
    vad_config.silero_vad.window_size = 512;
    vad_config.silero_vad.max_speech_duration = 20.0f;
    vad_config.sample_rate = APEXO_SAMPLE_RATE;
    vad_config.num_threads = 1;
    vad_config.provider = "cpu";
    state->vad = state->sherpa.create_vad(&vad_config, 30.0f);
    if (state->vad == nullptr) {
      state->asr_mode = AsrMode::kFailed;
      return -5;
    }
  }
  state->asr_mode = AsrMode::kLive;
  return 0;
}

int asr_push(void* user, const float* pcm, int32_t count, int32_t sample_rate, ApexoAsrEvent* out) {
  EngineState* state = static_cast<EngineState*>(user);
  if (state == nullptr || out == nullptr || pcm == nullptr) return -1;
  std::memset(out, 0, sizeof(*out));
  out->is_final = -1;
  if (sample_rate != APEXO_SAMPLE_RATE) return -2;
  const int ready = ensure_asr(state);
  if (ready != 0) return ready;
  if (state->asr_mode == AsrMode::kCanned) {
    canned_asr(state, out);
    return 0;
  }
  if (state->vad != nullptr) state->sherpa.vad_accept(state->vad, pcm, count);
  state->sherpa.accept_waveform(state->stream, sample_rate, pcm, count);
  int steps = 0;
  while (state->sherpa.is_ready(state->recognizer, state->stream) && steps < 8) {
    state->sherpa.decode(state->recognizer, state->stream);
    steps += 1;
  }
  const SherpaOnlineRecognizerResult* result = state->sherpa.get_result(state->recognizer, state->stream);
  const char* text = result != nullptr ? result->text : "";
  copy_text(out, text);
  const int endpoint = state->sherpa.is_endpoint(state->recognizer, state->stream);
  if (result != nullptr) state->sherpa.destroy_result(result);
  state->samples += count;
  out->end_ms = static_cast<int32_t>(state->samples * 1000 / APEXO_SAMPLE_RATE);
  out->start_ms = out->end_ms - static_cast<int32_t>(static_cast<int64_t>(count) * 1000 / APEXO_SAMPLE_RATE);
  if (endpoint) {
    out->is_final = 1;
    state->sherpa.reset_stream(state->recognizer, state->stream);
  } else if (out->text[0] == '\0') {
    out->is_final = -1;
  } else {
    out->is_final = 0;
  }
  return 0;
}

void asr_reset(void* user) {
  EngineState* state = static_cast<EngineState*>(user);
  if (state == nullptr || state->asr_mode != AsrMode::kLive || state->recognizer == nullptr || state->stream == nullptr) return;
  state->sherpa.reset_stream(state->recognizer, state->stream);
}

int sum_is_loaded(void* user) {
  EngineState* state = static_cast<EngineState*>(user);
  return state != nullptr && state->engine != nullptr ? 1 : 0;
}

int sum_load(void* user, const char* model_path) {
  EngineState* state = static_cast<EngineState*>(user);
  if (state == nullptr) return -1;
  unload_gemma(state);
  const char* path = model_path != nullptr ? model_path : "";
  state->gemma_file_present = path_exists(path);
  if (!state->gemma_file_present) return 0;
  if (!load_litert(&state->litert)) return -5;
  LiteRtSettings* settings = nullptr;
  if (state->litert.settings_create(path, "cpu", nullptr, nullptr, &settings) != 0 || settings == nullptr) return -5;
  state->settings = settings;
  LiteRtEngine* engine = nullptr;
  if (state->litert.engine_create(settings, &engine) != 0 || engine == nullptr) return -5;
  state->engine = engine;
  LiteRtConversation* conversation = nullptr;
  if (state->litert.conversation_create(engine, nullptr, &conversation) != 0 || conversation == nullptr) return -5;
  state->conversation = conversation;
  return 0;
}

int sum_unload(void* user) {
  EngineState* state = static_cast<EngineState*>(user);
  if (state == nullptr) return -1;
  unload_gemma(state);
  return 0;
}

std::string message_json(const char* prompt) {
  std::string out = "{\"role\":\"user\",\"content\":\"";
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(prompt != nullptr ? prompt : "");
  for (; *cursor != 0; ++cursor) {
    const unsigned char c = *cursor;
    if (c == '\\' || c == '"') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  out += "\"}";
  return out;
}

bool parse_json_string(const char* start, std::string* out) {
  if (start == nullptr || *start != '"') return false;
  ++start;
  out->clear();
  while (*start != '\0') {
    if (*start == '\\') {
      ++start;
      if (*start == 'n') out->push_back('\n');
      else if (*start == 'r') out->push_back('\r');
      else if (*start == 't') out->push_back('\t');
      else if (*start != '\0') out->push_back(*start);
      if (*start != '\0') ++start;
      continue;
    }
    if (*start == '"') return true;
    out->push_back(*start);
    ++start;
  }
  return false;
}

bool extract_response_text(const char* json, std::string* out) {
  if (json == nullptr) return false;
  const char* key = std::strstr(json, "\"text\"");
  if (key == nullptr) key = std::strstr(json, "\"content\"");
  if (key == nullptr) return false;
  const char* colon = std::strchr(key, ':');
  if (colon == nullptr) return false;
  ++colon;
  while (*colon == ' ' || *colon == '\n' || *colon == '\t') ++colon;
  return parse_json_string(colon, out);
}

int sum_generate(void* user, const char* prompt, char* out, int32_t out_cap) {
  EngineState* state = static_cast<EngineState*>(user);
  if (state == nullptr || out == nullptr || out_cap < 1) return -1;
  out[0] = '\0';
  if (!state->gemma_file_present) {
    const char* canned = "模型文件不在，未调用 LiteRT-LM。";
    if (static_cast<int32_t>(std::strlen(canned)) >= out_cap) return -1;
    std::strcpy(out, canned);
    return 0;
  }
  if (state->conversation == nullptr || state->litert.send_message == nullptr) return -5;
  const std::string json = message_json(prompt);
  LiteRtJsonResponse* response = nullptr;
  if (state->litert.send_message(state->conversation, json.c_str(), nullptr, nullptr, &response) != 0 || response == nullptr) {
    return -5;
  }
  const char* response_json = nullptr;
  const int got = state->litert.json_get(response, &response_json);
  std::string text;
  const bool parsed = got == 0 && extract_response_text(response_json, &text);
  if (state->litert.json_delete != nullptr) state->litert.json_delete(response);
  if (!parsed) return -5;
  if (static_cast<int32_t>(text.size()) >= out_cap) return -1;
  std::memcpy(out, text.c_str(), text.size() + 1);
  return 0;
}

}  // namespace

extern "C" void* apexo_engine_state_create(const char* asr_model_dir, const char* gemma_path) {
  EngineState* state = new EngineState();
  state->asr_dir = asr_model_dir != nullptr ? asr_model_dir : "";
  state->tokens_path = join_path(state->asr_dir, "tokens.txt");
  state->silero_path = join_path(state->asr_dir, "silero_vad.onnx");
  state->encoder_path = join_path(state->asr_dir, "encoder.onnx");
  state->decoder_path = join_path(state->asr_dir, "decoder.onnx");
  state->joiner_path = join_path(state->asr_dir, "joiner.onnx");
  (void)gemma_path;
  return state;
}

extern "C" void apexo_engine_state_destroy(void* user) {
  EngineState* state = static_cast<EngineState*>(user);
  if (state == nullptr) return;
  unload_gemma(state);
  if (state->vad != nullptr && state->sherpa.destroy_vad != nullptr) state->sherpa.destroy_vad(state->vad);
  if (state->stream != nullptr && state->sherpa.destroy_stream != nullptr) state->sherpa.destroy_stream(state->stream);
  if (state->recognizer != nullptr && state->sherpa.destroy_recognizer != nullptr) {
    state->sherpa.destroy_recognizer(state->recognizer);
  }
  close_library(state->sherpa.library);
  close_library(state->litert.library);
  delete state;
}

extern "C" int apexo_engine_state_bind(void* user, ApexoEngines* out) {
  if (user == nullptr || out == nullptr) return -1;
  std::memset(out, 0, sizeof(*out));
  out->asr_user = user;
  out->sum_user = user;
  out->asr_push = asr_push;
  out->asr_reset = asr_reset;
  out->sum_is_loaded = sum_is_loaded;
  out->sum_load = sum_load;
  out->sum_unload = sum_unload;
  out->sum_generate = sum_generate;
  return 0;
}
