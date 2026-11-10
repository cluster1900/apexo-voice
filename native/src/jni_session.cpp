#include "apexo_session.h"

#include <jni.h>

#include <cstring>
#include <string>

struct Boxed {
  ApexoSession* session;
  void* engines;
};

static Boxed* box_from(jlong handle) { return reinterpret_cast<Boxed*>(handle); }

extern "C" JNIEXPORT jlong JNICALL Java_com_apexo_sdk_ApexoSession_nativeCreate(JNIEnv* env, jclass,
                                                                                         jstring model_path) {
  const char* path = model_path != nullptr ? env->GetStringUTFChars(model_path, nullptr) : "";
  Boxed* box = new Boxed();
  box->session = nullptr;
  box->engines = apexo_engine_state_create(nullptr, path);
  ApexoConfig config;
  std::memset(&config, 0, sizeof(config));
  config.merge_gap_ms = APEXO_MERGE_GAP_MS;
  config.summary_chunk_chars = APEXO_SUMMARY_CHUNK_CHARS;
  config.display_lines = APEXO_DISPLAY_LINES;
  config.summary_model_path = path;
  config.web_summary = 0;
  ApexoEngines engines;
  if (apexo_engine_state_bind(box->engines, &engines) != 0) {
    apexo_engine_state_destroy(box->engines);
    delete box;
    if (model_path != nullptr) env->ReleaseStringUTFChars(model_path, path);
    return 0;
  }
  box->session = apexo_session_create(&config, &engines);
  if (model_path != nullptr) env->ReleaseStringUTFChars(model_path, path);
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
  const int rc = apexo_session_push_pcm(box->session, samples, count, sample_rate);
  env->ReleaseFloatArrayElements(pcm, samples, JNI_ABORT);
  return rc;
}

extern "C" JNIEXPORT jint JNICALL Java_com_apexo_sdk_ApexoSession_nativeEndSegment(JNIEnv*, jclass, jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return -1;
  return apexo_session_end_segment(box->session);
}

extern "C" JNIEXPORT jstring JNICALL Java_com_apexo_sdk_ApexoSession_nativeSummarize(JNIEnv* env, jclass,
                                                                                              jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return env->NewStringUTF("");
  std::string out(16384, '\0');
  const int rc = apexo_session_summarize(box->session, out.data(), static_cast<int32_t>(out.size()));
  if (rc != 0) return env->NewStringUTF(apexo_session_last_error(box->session));
  return env->NewStringUTF(out.c_str());
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
