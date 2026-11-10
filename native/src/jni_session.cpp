#include "apexo_session.h"

#include <jni.h>

#include <cstring>
#include <string>

struct Boxed {
  ApexoSession* session;
  void* engines;
};

static Boxed* box_from(jlong handle) { return reinterpret_cast<Boxed*>(handle); }

namespace {

/* Owns a UTF-8 copy of a Java string. Null stays an empty string. */
struct Utf8 {
  JNIEnv* env;
  jstring str;
  const char* chars;
  Utf8(JNIEnv* e, jstring s) : env(e), str(s), chars(nullptr) {
    if (s != nullptr) chars = e->GetStringUTFChars(s, nullptr);
  }
  ~Utf8() {
    if (chars != nullptr) env->ReleaseStringUTFChars(str, chars);
  }
  const char* get() const { return chars != nullptr ? chars : ""; }
};

}  // namespace

extern "C" JNIEXPORT jlong JNICALL Java_com_apexo_sdk_ApexoSession_nativeCreate(JNIEnv* env, jclass,
                                                                                         jstring asr_model_dir,
                                                                                         jstring model_path) {
  Utf8 asr(env, asr_model_dir);
  Utf8 gemma(env, model_path);
  Boxed* box = new Boxed();
  box->session = nullptr;
  box->engines = apexo_engine_state_create(asr.get(), gemma.get());
  ApexoConfig config;
  std::memset(&config, 0, sizeof(config));
  config.merge_gap_ms = APEXO_MERGE_GAP_MS;
  config.summary_chunk_chars = APEXO_SUMMARY_CHUNK_CHARS;
  config.display_lines = APEXO_DISPLAY_LINES;
  config.summary_model_path = gemma.get();
  config.web_summary = 0;
  ApexoEngines engines;
  if (apexo_engine_state_bind(box->engines, &engines) != 0) {
    apexo_engine_state_destroy(box->engines);
    delete box;
    return 0;
  }
  // The session copies the model path, so releasing the Java string on return is safe.
  box->session = apexo_session_create(&config, &engines);
  if (box->session == nullptr) {
    apexo_engine_state_destroy(box->engines);
    delete box;
    return 0;
  }
  return reinterpret_cast<jlong>(box);
}

extern "C" JNIEXPORT void JNICALL Java_com_apexo_sdk_ApexoSession_nativeDestroy(JNIEnv*, jclass, jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return;
  apexo_session_release(box->session);
  apexo_session_destroy(box->session);
  apexo_engine_state_destroy(box->engines);
  delete box;
}

extern "C" JNIEXPORT jint JNICALL Java_com_apexo_sdk_ApexoSession_nativePushPcm(JNIEnv* env, jclass, jlong handle,
                                                                                         jfloatArray pcm, jint sample_rate) {
  Boxed* box = box_from(handle);
  if (box == nullptr || pcm == nullptr) return -1;
  jsize count = env->GetArrayLength(pcm);
  jfloat* samples = env->GetFloatArrayElements(pcm, nullptr);
  if (samples == nullptr) return -1;
  const int rc = apexo_session_push_pcm(box->session, samples, count, sample_rate);
  env->ReleaseFloatArrayElements(pcm, samples, JNI_ABORT);
  return rc;
}

extern "C" JNIEXPORT jint JNICALL Java_com_apexo_sdk_ApexoSession_nativeEndSegment(JNIEnv*, jclass, jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return -1;
  return apexo_session_end_segment(box->session);
}

/* Returns the summary, or null when summarize failed. The reason is in nativeLastError. */
extern "C" JNIEXPORT jstring JNICALL Java_com_apexo_sdk_ApexoSession_nativeSummarize(JNIEnv* env, jclass,
                                                                                              jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return nullptr;
  std::string out(262144, '\0');
  const int rc = apexo_session_summarize(box->session, &out[0], static_cast<int32_t>(out.size()));
  if (rc != 0) return nullptr;
  return env->NewStringUTF(out.c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_apexo_sdk_ApexoSession_nativeLastError(JNIEnv* env, jclass,
                                                                                               jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return env->NewStringUTF("session is closed");
  return env->NewStringUTF(apexo_session_last_error(box->session));
}

extern "C" JNIEXPORT jstring JNICALL Java_com_apexo_sdk_ApexoSession_nativeDisplay(JNIEnv* env, jclass,
                                                                                            jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return env->NewStringUTF("");
  return env->NewStringUTF(apexo_session_display_text(box->session));
}

extern "C" JNIEXPORT jstring JNICALL Java_com_apexo_sdk_ApexoSession_nativeFullTranscript(JNIEnv* env, jclass,
                                                                                                   jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return env->NewStringUTF("");
  return env->NewStringUTF(apexo_session_full_transcript(box->session));
}

extern "C" JNIEXPORT jint JNICALL Java_com_apexo_sdk_ApexoSession_nativeGemmaResident(JNIEnv*, jclass,
                                                                                               jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return 0;
  return apexo_session_gemma_resident(box->session);
}

extern "C" JNIEXPORT void JNICALL Java_com_apexo_sdk_ApexoSession_nativeRelease(JNIEnv*, jclass, jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return;
  apexo_session_release(box->session);
}

extern "C" JNIEXPORT jstring JNICALL Java_com_apexo_sdk_ApexoSession_nativeSummarizeNew(JNIEnv* env, jclass,
                                                                                                  jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return nullptr;
  std::string out(262144, '\0');
  const int rc = apexo_session_summarize_new(box->session, &out[0], static_cast<int32_t>(out.size()));
  if (rc != 0) return nullptr;
  return env->NewStringUTF(out.c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_apexo_sdk_ApexoSession_nativeSummary(JNIEnv* env, jclass,
                                                                                             jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return env->NewStringUTF("");
  return env->NewStringUTF(apexo_session_summary_text(box->session));
}
