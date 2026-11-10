#include <stdio.h>
#include <stdlib.h>

typedef struct SherpaOnnxOnlineRecognizer { int unused; } SherpaOnnxOnlineRecognizer;
typedef struct SherpaOnnxOnlineStream { int unused; } SherpaOnnxOnlineStream;
typedef struct SherpaOnnxVoiceActivityDetector { int unused; } SherpaOnnxVoiceActivityDetector;

typedef struct SherpaOnnxOnlineRecognizerResult {
  const char* text;
  const char* tokens;
  const char* const* tokens_arr;
  float* timestamps;
  int count;
  const char* json;
} SherpaOnnxOnlineRecognizerResult;

static SherpaOnnxOnlineRecognizer g_recognizer;
static SherpaOnnxOnlineStream g_stream;
static SherpaOnnxVoiceActivityDetector g_vad;
static SherpaOnnxOnlineRecognizerResult g_result = {"适配器转写", 0, 0, 0, 0, 0};
static int g_ready = 0;

static void note(const char* line) {
  const char* path = getenv("APEXO_FAKE_LOG");
  if (path == 0 || path[0] == '\0') return;
  FILE* file = fopen(path, "a");
  if (file == 0) return;
  fputs(line, file);
  fputc('\n', file);
  fclose(file);
}

const SherpaOnnxOnlineRecognizer* SherpaOnnxCreateOnlineRecognizer(const void* config) {
  (void)config;
  note("create_recognizer");
  return &g_recognizer;
}

void SherpaOnnxDestroyOnlineRecognizer(const SherpaOnnxOnlineRecognizer* recognizer) { (void)recognizer; }

const SherpaOnnxOnlineStream* SherpaOnnxCreateOnlineStream(const SherpaOnnxOnlineRecognizer* recognizer) {
  (void)recognizer;
  note("create_stream");
  return &g_stream;
}

void SherpaOnnxDestroyOnlineStream(const SherpaOnnxOnlineStream* stream) { (void)stream; }

void SherpaOnnxOnlineStreamAcceptWaveform(const SherpaOnnxOnlineStream* stream, int sample_rate, const float* samples,
                                           int n) {
  (void)stream;
  (void)samples;
  char line[128];
  snprintf(line, sizeof(line), "accept %d %d", sample_rate, n);
  note(line);
  g_ready = 1;
}

int SherpaOnnxIsOnlineStreamReady(const SherpaOnnxOnlineRecognizer* recognizer, const SherpaOnnxOnlineStream* stream) {
  (void)recognizer;
  (void)stream;
  return g_ready;
}

void SherpaOnnxDecodeOnlineStream(const SherpaOnnxOnlineRecognizer* recognizer, const SherpaOnnxOnlineStream* stream) {
  (void)recognizer;
  (void)stream;
  g_ready = 0;
  note("decode");
}

const SherpaOnnxOnlineRecognizerResult* SherpaOnnxGetOnlineStreamResult(const SherpaOnnxOnlineRecognizer* recognizer,
                                                                         const SherpaOnnxOnlineStream* stream) {
  (void)recognizer;
  (void)stream;
  return &g_result;
}

void SherpaOnnxDestroyOnlineRecognizerResult(const SherpaOnnxOnlineRecognizerResult* result) { (void)result; }

int SherpaOnnxOnlineStreamIsEndpoint(const SherpaOnnxOnlineRecognizer* recognizer, const SherpaOnnxOnlineStream* stream) {
  (void)recognizer;
  (void)stream;
  return 1;
}

void SherpaOnnxOnlineStreamReset(const SherpaOnnxOnlineRecognizer* recognizer, const SherpaOnnxOnlineStream* stream) {
  (void)recognizer;
  (void)stream;
  note("reset");
}

const SherpaOnnxVoiceActivityDetector* SherpaOnnxCreateVoiceActivityDetector(const void* config, float seconds) {
  (void)config;
  (void)seconds;
  note("create_vad");
  return &g_vad;
}

void SherpaOnnxDestroyVoiceActivityDetector(const SherpaOnnxVoiceActivityDetector* vad) { (void)vad; }

void SherpaOnnxVoiceActivityDetectorAcceptWaveform(const SherpaOnnxVoiceActivityDetector* vad, const float* samples,
                                                    int n) {
  (void)vad;
  (void)samples;
  char line[64];
  snprintf(line, sizeof(line), "vad %d", n);
  note(line);
}
