#include "apexo_session.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#endif

#if defined(APEXO_STATIC_ENGINES)
/* Static mode (compile with -DAPEXO_STATIC_ENGINES): the engines are linked into the host program instead of being
 * found with dlopen. The host must link both the sherpa-onnx C API and the LiteRT-LM C API (1.0.0 or newer). */
#define APEXO_WEAK
extern "C" {
APEXO_WEAK const void* SherpaOnnxCreateOnlineRecognizer(const void* config);
APEXO_WEAK void SherpaOnnxDestroyOnlineRecognizer(const void* recognizer);
APEXO_WEAK const void* SherpaOnnxCreateOnlineStream(const void* recognizer);
APEXO_WEAK void SherpaOnnxDestroyOnlineStream(const void* stream);
APEXO_WEAK void SherpaOnnxOnlineStreamAcceptWaveform(const void* stream, int32_t sample_rate, const float* samples, int32_t n);
APEXO_WEAK int32_t SherpaOnnxIsOnlineStreamReady(const void* recognizer, const void* stream);
APEXO_WEAK void SherpaOnnxDecodeOnlineStream(const void* recognizer, const void* stream);
APEXO_WEAK const void* SherpaOnnxGetOnlineStreamResult(const void* recognizer, const void* stream);
APEXO_WEAK void SherpaOnnxDestroyOnlineRecognizerResult(const void* result);
APEXO_WEAK int32_t SherpaOnnxOnlineStreamIsEndpoint(const void* recognizer, const void* stream);
APEXO_WEAK void SherpaOnnxOnlineStreamReset(const void* recognizer, const void* stream);

APEXO_WEAK int litert_lm_engine_settings_create(const char* model, const char* backend, const char* vision, const char* audio, void** out);
APEXO_WEAK void litert_lm_engine_settings_delete(void* settings);
APEXO_WEAK int litert_lm_engine_create(const void* settings, void** out);
APEXO_WEAK void litert_lm_engine_delete(void* engine);
APEXO_WEAK int litert_lm_conversation_create(void* engine, void* config, void** out);
APEXO_WEAK void litert_lm_conversation_delete(void* conversation);
APEXO_WEAK int litert_lm_conversation_send_message(void* conversation, const char* message, const char* extra, const void* optional_args, void** out);
APEXO_WEAK void litert_lm_json_response_delete(void* response);
APEXO_WEAK int litert_lm_json_response_get_string(const void* response, const char** out);
}
#endif

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
  bool bound = false;  // symbols resolved
  bool ready = false;  // and all of them present
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
};

struct LitertApi {
  void* library = nullptr;
  bool bound = false;
  bool ready = false;
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

enum class AsrMode { kUnknown, kMissing, kLive, kFailed };

struct EngineState {
  std::string asr_dir;
  std::string sherpa_library;  // explicit path set by the host; empty means the default name or the env variable
  std::string litert_library;
  std::string tokens_path;
  std::string encoder_path;
  std::string decoder_path;
  std::string joiner_path;
  int64_t samples = 0;
  AsrMode asr_mode = AsrMode::kUnknown;
  SherpaApi sherpa;
  const SherpaOnlineRecognizer* recognizer = nullptr;
  const SherpaOnlineStream* stream = nullptr;
  LitertApi litert;
  LiteRtSettings* settings = nullptr;
  LiteRtEngine* engine = nullptr;
  LiteRtConversation* conversation = nullptr;
  int generated = 0;      // messages sent on the current conversation
  int64_t utterance_start_ms = -1;  // start of the words being decoded, -1 when none
};

#if defined(_WIN32)
std::wstring utf8_to_wide(const std::string& path) {
  if (path.empty()) return {};
  int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0);
  UINT cp = CP_UTF8;
  DWORD flags = MB_ERR_INVALID_CHARS;
  if (needed <= 0) {
    cp = CP_ACP;
    flags = 0;
    needed = MultiByteToWideChar(cp, flags, path.c_str(), -1, nullptr, 0);
  }
  if (needed <= 0) return {};
  std::wstring out(static_cast<size_t>(needed - 1), L'\0');
  MultiByteToWideChar(cp, flags, path.c_str(), -1, &out[0], needed);
  return out;
}

/* Converts a wide path to the ANSI code page. Fails instead of substituting characters that code page cannot
 * show, because a different path would silently point at a different (or no) file. */
bool wide_to_acp(const std::wstring& wide, std::string* out) {
  BOOL used_default = FALSE;
  const int needed =
      WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(), -1, nullptr, 0, nullptr, &used_default);
  if (needed <= 0 || used_default) return false;
  std::string converted(static_cast<size_t>(needed - 1), '\0');
  used_default = FALSE;
  WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(), -1, &converted[0], needed, nullptr, &used_default);
  if (used_default) return false;
  *out = converted;
  return true;
}

/* The engines take narrow paths. Gives the path they can open, or false when none exists: the path does not
 * fit the ANSI code page and the volume has no 8.3 short name for it (ReFS never does, NTFS may not). */
bool normalize_narrow_path(const std::string& path, std::string* out) {
  const std::wstring wpath = utf8_to_wide(path);
  if (wpath.empty()) return false;
  if (GetACP() == CP_UTF8) {
    *out = path;
    return true;
  }
  if (wide_to_acp(wpath, out)) return true;
  const DWORD needed = GetShortPathNameW(wpath.c_str(), nullptr, 0);
  if (needed == 0) return false;
  std::wstring short_path(needed, L'\0');
  const DWORD written = GetShortPathNameW(wpath.c_str(), &short_path[0], needed);
  if (written == 0 || written >= needed) return false;
  short_path.resize(written);
  return wide_to_acp(short_path, out);
}
#endif

#if !defined(APEXO_STATIC_ENGINES)
void* open_library(const char* path) {
  if (path == nullptr || path[0] == '\0') return nullptr;
#if defined(_WIN32)
  const std::wstring wpath = utf8_to_wide(path);
  if (wpath.empty()) return nullptr;
  if (wpath.find_first_of(L"/\\") != std::wstring::npos) {
    wchar_t full_buf[4096];
    const DWORD len = GetFullPathNameW(wpath.c_str(), 4096, full_buf, nullptr);
    const wchar_t* target = (len > 0 && len < 4096) ? full_buf : wpath.c_str();
    HMODULE mod =
        LoadLibraryExW(target, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (mod != nullptr) return reinterpret_cast<void*>(mod);
  }
  return reinterpret_cast<void*>(LoadLibraryW(wpath.c_str()));
#else
#if defined(RTLD_NODELETE)
  return dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
#else
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
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

#endif

void close_library(void* library) {
  /* Engine runtimes (LiteRT-LM / XNNPACK / ONNX Runtime) register pthread_key / FLS destructors pointing into
   * their own code segments without deleting the keys on engine destruction. Unmapping the shared library via
   * dlclose/FreeLibrary causes worker threads to jump to unmapped memory during pthread_exit / thread detach.
   * Model weights and engine instances are still freed by unload_gemma() and destroy_recognizer(). */
  (void)library;
}

bool path_exists(const std::string& path) {
  if (path.empty()) return false;
#if defined(_WIN32)
  const std::wstring wpath = utf8_to_wide(path);
  if (wpath.empty()) return false;
  return GetFileAttributesW(wpath.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
  struct stat st;
  return stat(path.c_str(), &st) == 0;
#endif
}

std::string join_path(const std::string& dir, const char* name) {
  if (dir.empty()) return name;
  if (dir.back() == '/' || dir.back() == '\\') return dir + name;
  return dir + "/" + name;
}

#if !defined(APEXO_STATIC_ENGINES)
void* open_first_available(const std::string& explicit_path, const char* env_key, const char* primary,
                           const char* secondary) {
  if (!explicit_path.empty()) return open_library(explicit_path.c_str());
  const char* env_val = std::getenv(env_key);
  if (env_val != nullptr && env_val[0] != '\0') return open_library(env_val);
  void* lib = open_library(primary);
  if (lib != nullptr) return lib;
  return open_library(secondary);
}

void* open_default_sherpa(const std::string& explicit_path) {
#if defined(_WIN32)
  return open_first_available(explicit_path, "APEXO_SHERPA_LIBRARY", "sherpa-onnx-c-api.dll", "sherpa-onnx.dll");
#elif defined(__APPLE__)
  return open_first_available(explicit_path, "APEXO_SHERPA_LIBRARY", "libsherpa-onnx-c-api.dylib",
                              "libsherpa-onnx.dylib");
#else
  return open_first_available(explicit_path, "APEXO_SHERPA_LIBRARY", "libsherpa-onnx-c-api.so", "libsherpa-onnx.so");
#endif
}

void* open_default_litert(const std::string& explicit_path) {
#if defined(_WIN32)
  return open_first_available(explicit_path, "APEXO_LITERT_LIBRARY", "litert-lm.dll", "litert_lm.dll");
#elif defined(__APPLE__)
  return open_first_available(explicit_path, "APEXO_LITERT_LIBRARY", "liblitert-lm.dylib", "liblitert_lm.dylib");
#else
  return open_first_available(explicit_path, "APEXO_LITERT_LIBRARY", "liblitert-lm.so", "liblitert_lm.so");
#endif
}
#endif

#if defined(APEXO_STATIC_ENGINES)
#define APEXO_BIND(api, field, type, symbol) ((api)->field = reinterpret_cast<type>(&symbol))
#else
#define APEXO_BIND(api, field, type, symbol) ((api)->field = reinterpret_cast<type>(lookup((api)->library, #symbol)))
#endif

/* True when every function the adapter needs was found. A failure is not cached when the library could not be
 * opened (it may be installed later) but is when the library was opened and lacks symbols. */
bool load_sherpa(SherpaApi* api, const std::string& explicit_path) {
  (void)explicit_path;
  if (api->bound) return api->ready;
#if !defined(APEXO_STATIC_ENGINES)
  api->library = open_default_sherpa(explicit_path);
  if (api->library == nullptr) return false;
#endif
  api->bound = true;
  APEXO_BIND(api, create_recognizer, CreateOnlineRecognizerFn, SherpaOnnxCreateOnlineRecognizer);
  APEXO_BIND(api, destroy_recognizer, DestroyOnlineRecognizerFn, SherpaOnnxDestroyOnlineRecognizer);
  APEXO_BIND(api, create_stream, CreateOnlineStreamFn, SherpaOnnxCreateOnlineStream);
  APEXO_BIND(api, destroy_stream, DestroyOnlineStreamFn, SherpaOnnxDestroyOnlineStream);
  APEXO_BIND(api, accept_waveform, AcceptWaveformFn, SherpaOnnxOnlineStreamAcceptWaveform);
  APEXO_BIND(api, is_ready, IsReadyFn, SherpaOnnxIsOnlineStreamReady);
  APEXO_BIND(api, decode, DecodeFn, SherpaOnnxDecodeOnlineStream);
  APEXO_BIND(api, get_result, GetResultFn, SherpaOnnxGetOnlineStreamResult);
  APEXO_BIND(api, destroy_result, DestroyResultFn, SherpaOnnxDestroyOnlineRecognizerResult);
  APEXO_BIND(api, is_endpoint, IsEndpointFn, SherpaOnnxOnlineStreamIsEndpoint);
  APEXO_BIND(api, reset_stream, ResetStreamFn, SherpaOnnxOnlineStreamReset);
  api->ready = api->create_recognizer && api->destroy_recognizer && api->create_stream && api->destroy_stream &&
               api->accept_waveform && api->is_ready && api->decode && api->get_result && api->destroy_result &&
               api->is_endpoint && api->reset_stream;
  return api->ready;
}

bool load_litert(LitertApi* api, const std::string& explicit_path) {
  (void)explicit_path;
  if (api->bound) return api->ready;
#if !defined(APEXO_STATIC_ENGINES)
  api->library = open_default_litert(explicit_path);
  if (api->library == nullptr) return false;
#endif
  api->bound = true;
  APEXO_BIND(api, settings_create, SettingsCreateFn, litert_lm_engine_settings_create);
  APEXO_BIND(api, settings_delete, SettingsDeleteFn, litert_lm_engine_settings_delete);
  APEXO_BIND(api, engine_create, EngineCreateFn, litert_lm_engine_create);
  APEXO_BIND(api, engine_delete, EngineDeleteFn, litert_lm_engine_delete);
  APEXO_BIND(api, conversation_create, ConversationCreateFn, litert_lm_conversation_create);
  APEXO_BIND(api, conversation_delete, ConversationDeleteFn, litert_lm_conversation_delete);
  APEXO_BIND(api, send_message, SendMessageFn, litert_lm_conversation_send_message);
  APEXO_BIND(api, json_delete, JsonDeleteFn, litert_lm_json_response_delete);
  APEXO_BIND(api, json_get, JsonGetFn, litert_lm_json_response_get_string);
  api->ready = api->settings_create && api->settings_delete && api->engine_create && api->engine_delete &&
               api->conversation_create && api->conversation_delete && api->send_message && api->json_delete &&
               api->json_get;
  return api->ready;
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

/* Copies at most sizeof(out->text) - 1 bytes (trimmed of ASCII whitespace) and never cuts a UTF-8 character in half. */
void copy_text(ApexoAsrEvent* out, const char* text) {
  if (text == nullptr) text = "";
  const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  while (*text != '\0' && is_space(*text)) ++text;
  size_t len = std::strlen(text);
  while (len > 0 && is_space(text[len - 1])) --len;
  const size_t cap = sizeof(out->text) - 1;
  if (len > cap) {
    len = cap;
    while (len > 0 && (static_cast<unsigned char>(text[len]) & 0xC0) == 0x80) --len;
    while (len > 0 && is_space(text[len - 1])) --len;
  }
  std::memcpy(out->text, text, len);
  out->text[len] = '\0';
}

bool asr_files_present(const EngineState* state) {
  if (state->asr_dir.empty()) return false;  // no directory given; never fall back to the working directory
  return path_exists(state->tokens_path) && path_exists(state->encoder_path) && path_exists(state->decoder_path) &&
         path_exists(state->joiner_path);
}

/* 0: recognizer is live. -3: model files are not all there (checked again on every call, so a model that is
 * copied in later is picked up). -5: the files are there but the engine could not be built. */
int ensure_asr(EngineState* state) {
  if (state->asr_mode == AsrMode::kLive) return 0;
  if (state->asr_mode == AsrMode::kFailed) return -5;
  if (!asr_files_present(state)) {
    state->asr_mode = AsrMode::kMissing;
    return -3;
  }
  if (!load_sherpa(&state->sherpa, state->sherpa_library)) {
    state->asr_mode = AsrMode::kFailed;
    return -5;
  }
  SherpaOnlineRecognizerConfig config;
  std::memset(&config, 0, sizeof(config));
  config.feat_config.sample_rate = APEXO_SAMPLE_RATE;
  config.feat_config.feature_dim = 80;
#if defined(_WIN32)
  std::string tokens, encoder, decoder, joiner;
  if (!normalize_narrow_path(state->tokens_path, &tokens) || !normalize_narrow_path(state->encoder_path, &encoder) ||
      !normalize_narrow_path(state->decoder_path, &decoder) || !normalize_narrow_path(state->joiner_path, &joiner)) {
    state->asr_mode = AsrMode::kFailed;
    return -5;
  }
  config.model_config.tokens = tokens.c_str();
  config.model_config.transducer.encoder = encoder.c_str();
  config.model_config.transducer.decoder = decoder.c_str();
  config.model_config.transducer.joiner = joiner.c_str();
#else
  config.model_config.tokens = state->tokens_path.c_str();
  config.model_config.transducer.encoder = state->encoder_path.c_str();
  config.model_config.transducer.decoder = state->decoder_path.c_str();
  config.model_config.transducer.joiner = state->joiner_path.c_str();
#endif
  config.model_config.num_threads = 1;
  config.model_config.provider = "cpu";
  config.decoding_method = "greedy_search";
  config.enable_endpoint = 1;
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
  const int64_t chunk_start_ms = state->samples * 1000 / APEXO_SAMPLE_RATE;
  state->sherpa.accept_waveform(state->stream, sample_rate, pcm, count);
  state->samples += count;
  const int64_t chunk_end_ms = state->samples * 1000 / APEXO_SAMPLE_RATE;
  /* Decode everything that is ready, so latency does not pile up behind large pushes. The bound only guards
   * against a runaway loop. */
  for (int steps = 0; steps < 1024 && state->sherpa.is_ready(state->recognizer, state->stream); ++steps) {
    state->sherpa.decode(state->recognizer, state->stream);
  }
  const SherpaOnlineRecognizerResult* result = state->sherpa.get_result(state->recognizer, state->stream);
  copy_text(out, result != nullptr ? result->text : "");
  const int endpoint = state->sherpa.is_endpoint(state->recognizer, state->stream);
  if (result != nullptr) state->sherpa.destroy_result(result);
  const bool has_text = out->text[0] != '\0';
  /* Times describe the words, not the audio chunk: an utterance starts at the first chunk that had text. */
  if (has_text && state->utterance_start_ms < 0) state->utterance_start_ms = chunk_start_ms;
  out->start_ms = static_cast<int32_t>(has_text ? state->utterance_start_ms : chunk_start_ms);
  out->end_ms = static_cast<int32_t>(chunk_end_ms);
  if (endpoint) {
    /* Silence can end an "utterance" that never had words; that is not an event. */
    out->is_final = has_text ? 1 : -1;
    state->sherpa.reset_stream(state->recognizer, state->stream);
    state->utterance_start_ms = -1;
  } else {
    out->is_final = has_text ? 0 : -1;
  }
  return 0;
}

void asr_reset(void* user) {
  EngineState* state = static_cast<EngineState*>(user);
  if (state == nullptr || state->asr_mode != AsrMode::kLive || state->recognizer == nullptr || state->stream == nullptr) return;
  state->sherpa.reset_stream(state->recognizer, state->stream);
  state->utterance_start_ms = -1;
}

int sum_is_loaded(void* user) {
  EngineState* state = static_cast<EngineState*>(user);
  return state != nullptr && state->engine != nullptr ? 1 : 0;
}

/* Builds a fresh conversation, so chunks of one transcript do not see each other as chat history. */
int new_conversation(EngineState* state) {
  if (state->conversation != nullptr) state->litert.conversation_delete(state->conversation);
  state->conversation = nullptr;
  state->generated = 0;
  LiteRtConversation* conversation = nullptr;
  if (state->litert.conversation_create(state->engine, nullptr, &conversation) != 0 || conversation == nullptr) return -5;
  state->conversation = conversation;
  return 0;
}

/* -3: the model file is not there. -5: the file is there but LiteRT-LM could not load it. */
int sum_load(void* user, const char* model_path) {
  EngineState* state = static_cast<EngineState*>(user);
  if (state == nullptr) return -1;
  unload_gemma(state);
  const char* path = model_path != nullptr ? model_path : "";
  if (!path_exists(path)) return -3;
  auto fail = [&]() {
    unload_gemma(state);  // never leave half-built engine state (and its memory) behind
    return -5;
  };
  if (!load_litert(&state->litert, state->litert_library)) return fail();
  LiteRtSettings* settings = nullptr;
#if defined(_WIN32)
  std::string normalized_path;
  if (!normalize_narrow_path(path, &normalized_path)) return fail();
  const char* settings_path = normalized_path.c_str();
#else
  const char* settings_path = path;
#endif
  if (state->litert.settings_create(settings_path, "cpu", nullptr, nullptr, &settings) != 0 || settings == nullptr) {
    return fail();
  }
  state->settings = settings;
  LiteRtEngine* engine = nullptr;
  if (state->litert.engine_create(settings, &engine) != 0 || engine == nullptr) return fail();
  state->engine = engine;
  if (new_conversation(state) != 0) return fail();
  return 0;
}

int sum_unload(void* user) {
  EngineState* state = static_cast<EngineState*>(user);
  if (state == nullptr) return -1;
  unload_gemma(state);
  return 0;
}

void append_hex4(std::string* out, unsigned value) {
  static const char digits[] = "0123456789abcdef";
  out->append("\\u00");
  out->push_back(digits[(value >> 4) & 0xF]);
  out->push_back(digits[value & 0xF]);
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
    } else if (c < 0x20 || c == 0x7F) {
      append_hex4(&out, c);
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  out += "\"}";
  return out;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool read_hex4(const char* p, unsigned* out) {
  unsigned value = 0;
  for (int i = 0; i < 4; ++i) {
    const int h = p[i] == '\0' ? -1 : hex_value(p[i]);
    if (h < 0) return false;
    value = (value << 4) | static_cast<unsigned>(h);
  }
  *out = value;
  return true;
}

void append_utf8(std::string* out, unsigned cp) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

bool parse_json_string(const char* start, std::string* out, const char** after = nullptr) {
  if (start == nullptr || *start != '"') return false;
  ++start;
  out->clear();
  while (*start != '\0') {
    if (*start == '\\') {
      ++start;
      if (*start == '\0') return false;
      switch (*start) {
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'u': {
          unsigned cp = 0;
          if (!read_hex4(start + 1, &cp)) return false;
          start += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && start[1] == '\\' && start[2] == 'u') {
            unsigned low = 0;
            if (read_hex4(start + 3, &low) && low >= 0xDC00 && low <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
              start += 6;
            }
          }
          append_utf8(out, cp);
          break;
        }
        default: out->push_back(*start); break;  // \" \\ \/
      }
      ++start;
      continue;
    }
    if (*start == '"') {
      if (after != nullptr) *after = start + 1;
      return true;
    }
    out->push_back(*start);
    ++start;
  }
  return false;
}

const char* skip_json_ws(const char* p) {
  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
  return p;
}

bool extract_response_text(const char* json, std::string* out) {
  if (json == nullptr) return false;
  const char* p = json;
  while (*p != '\0') {
    if (*p != '"') {
      ++p;
      continue;
    }
    std::string key;
    const char* after_key = nullptr;
    if (!parse_json_string(p, &key, &after_key)) return false;
    p = skip_json_ws(after_key);
    if (*p != ':') continue;  // string value (for example "type":"text"), not a field name
    p = skip_json_ws(p + 1);
    if ((key == "text" || key == "content") && *p == '"') {
      return parse_json_string(p, out);
    }
  }
  return false;
}

int sum_generate(void* user, const char* prompt, char* out, int32_t out_cap) {
  EngineState* state = static_cast<EngineState*>(user);
  if (state == nullptr || out == nullptr || out_cap < 1) return -1;
  out[0] = '\0';
  if (state->engine == nullptr || state->litert.send_message == nullptr) return -5;
  if (state->generated > 0 && new_conversation(state) != 0) return -5;
  if (state->conversation == nullptr) return -5;
  const std::string json = message_json(prompt);
  LiteRtJsonResponse* response = nullptr;
  if (state->litert.send_message(state->conversation, json.c_str(), nullptr, nullptr, &response) != 0 || response == nullptr) {
    return -5;
  }
  state->generated += 1;
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
  state->encoder_path = join_path(state->asr_dir, "encoder.onnx");
  state->decoder_path = join_path(state->asr_dir, "decoder.onnx");
  state->joiner_path = join_path(state->asr_dir, "joiner.onnx");
  (void)gemma_path;
  return state;
}

extern "C" void apexo_engine_state_set_libraries(void* user, const char* sherpa_library, const char* litert_library) {
  EngineState* state = static_cast<EngineState*>(user);
  if (state == nullptr) return;
  state->sherpa_library = sherpa_library != nullptr ? sherpa_library : "";
  state->litert_library = litert_library != nullptr ? litert_library : "";
  if (state->asr_mode != AsrMode::kLive) state->asr_mode = AsrMode::kUnknown;
}

extern "C" void apexo_engine_state_destroy(void* user) {
  EngineState* state = static_cast<EngineState*>(user);
  if (state == nullptr) return;
  unload_gemma(state);
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
