#include "apexo_session.h"

#include <jni.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct Boxed {
  ApexoSession* session = nullptr;
  void* engines = nullptr;
  JavaVM* jvm = nullptr;
  jobject callback_target = nullptr;
  jmethodID dispatch_mid = nullptr;
};

static Boxed* box_from(jlong handle) { return reinterpret_cast<Boxed*>(handle); }

namespace {

std::string jstring_to_std_utf8(JNIEnv* env, jstring s) {
  if (env == nullptr || s == nullptr) return {};
  const jsize len = env->GetStringLength(s);
  if (len <= 0) return {};
  const jchar* u16 = env->GetStringChars(s, nullptr);
  if (u16 == nullptr) return {};
  std::string out;
  out.reserve(static_cast<size_t>(len) * 3);
  for (jsize i = 0; i < len; ++i) {
    uint32_t cp = u16[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && u16[i + 1] >= 0xDC00 && u16[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (u16[i + 1] - 0xDC00);
      ++i;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  env->ReleaseStringChars(s, u16);
  return out;
}

jstring std_utf8_to_jstring(JNIEnv* env, const char* s) {
  if (env == nullptr) return nullptr;
  const jchar empty_ch = 0;
  if (s == nullptr || s[0] == '\0') return env->NewString(&empty_ch, 0);
  std::vector<jchar> u16;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  while (*p != 0) {
    uint32_t cp = 0;
    if (*p < 0x80) {
      cp = *p++;
    } else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
      p += 2;
    } else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
      p += 3;
    } else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
      cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
      p += 4;
    } else {
      cp = 0xFFFD;
      ++p;
    }
    if (cp <= 0xFFFF) {
      u16.push_back(static_cast<jchar>(cp));
    } else if (cp <= 0x10FFFF) {
      cp -= 0x10000;
      u16.push_back(static_cast<jchar>(0xD800 + (cp >> 10)));
      u16.push_back(static_cast<jchar>(0xDC00 + (cp & 0x3FF)));
    } else {
      u16.push_back(0xFFFD);
    }
  }
  return env->NewString(u16.data(), static_cast<jsize>(u16.size()));
}

/* Owns a standard RFC 3629 UTF-8 copy of a Java string. Null stays "" for get(), or nullptr for nullable_get(). */
struct Utf8 {
  bool is_null;
  std::string text;
  Utf8(JNIEnv* e, jstring s) : is_null(s == nullptr), text(jstring_to_std_utf8(e, s)) {}
  const char* get() const { return text.c_str(); }
  const char* nullable_get() const { return is_null ? nullptr : text.c_str(); }
};

void write_rc(JNIEnv* env, jintArray rc_out, int rc) {
  if (rc_out == nullptr) return;
  const jint val = static_cast<jint>(rc);
  env->SetIntArrayRegion(rc_out, 0, 1, &val);
}

void jni_transcript_callback(void* user, const ApexoAsrEvent* event) {
  Boxed* box = static_cast<Boxed*>(user);
  if (box == nullptr || event == nullptr || box->jvm == nullptr || box->callback_target == nullptr ||
      box->dispatch_mid == nullptr) {
    return;
  }
  JNIEnv* env = nullptr;
  bool attached = false;
  const jint get_rc = box->jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (get_rc == JNI_EDETACHED) {
#if defined(__ANDROID__)
    if (box->jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
#else
    if (box->jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) return;
#endif
    attached = true;
  } else if (get_rc != JNI_OK || env == nullptr) {
    return;
  }
  jstring jtext = std_utf8_to_jstring(env, event->text);
  if (jtext != nullptr) {
    env->CallVoidMethod(box->callback_target, box->dispatch_mid, jtext, static_cast<jboolean>(event->is_final == 1),
                        static_cast<jint>(event->start_ms), static_cast<jint>(event->end_ms));
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    env->DeleteLocalRef(jtext);
  } else if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  if (attached) box->jvm->DetachCurrentThread();
}

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

extern "C" JNIEXPORT void JNICALL Java_com_apexo_sdk_ApexoSession_nativeDestroy(JNIEnv* env, jclass, jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return;
  apexo_session_set_transcript_callback(box->session, nullptr, nullptr);
  if (box->callback_target != nullptr && env != nullptr) {
    env->DeleteGlobalRef(box->callback_target);
    box->callback_target = nullptr;
  }
  apexo_session_reset(box->session);
  apexo_session_destroy(box->session);
  apexo_engine_state_destroy(box->engines);
  delete box;
}

extern "C" JNIEXPORT void JNICALL Java_com_apexo_sdk_ApexoSession_nativeSetTranscriptCallback(JNIEnv* env, jclass,
                                                                                              jlong handle,
                                                                                              jobject target) {
  Boxed* box = box_from(handle);
  if (box == nullptr || env == nullptr) return;
  apexo_session_set_transcript_callback(box->session, nullptr, nullptr);
  if (box->callback_target != nullptr) {
    env->DeleteGlobalRef(box->callback_target);
    box->callback_target = nullptr;
  }
  box->dispatch_mid = nullptr;
  if (target == nullptr) return;
  if (env->GetJavaVM(&box->jvm) != JNI_OK || box->jvm == nullptr) return;
  jclass cls = env->GetObjectClass(target);
  if (cls == nullptr) return;
  jmethodID mid = env->GetMethodID(cls, "dispatchTranscript", "(Ljava/lang/String;ZII)V");
  env->DeleteLocalRef(cls);
  if (mid == nullptr) return;
  box->callback_target = env->NewGlobalRef(target);
  if (box->callback_target == nullptr) return;
  box->dispatch_mid = mid;
  apexo_session_set_transcript_callback(box->session, jni_transcript_callback, box);
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

/* Returns the summary, or null when summarize failed. The return code is written to rc_out[0] and the reason is in nativeLastError. */
extern "C" JNIEXPORT jstring JNICALL Java_com_apexo_sdk_ApexoSession_nativeSummarize(JNIEnv* env, jclass,
                                                                                              jlong handle,
                                                                                              jintArray rc_out) {
  Boxed* box = box_from(handle);
  if (box == nullptr) {
    write_rc(env, rc_out, -1);
    return nullptr;
  }
  std::string out(262144, '\0');
  const int rc = apexo_session_summarize(box->session, &out[0], static_cast<int32_t>(out.size()));
  write_rc(env, rc_out, rc);
  if (rc != 0) return nullptr;
  return std_utf8_to_jstring(env, out.c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_apexo_sdk_ApexoSession_nativeSummarizeText(JNIEnv* env, jclass,
                                                                                                  jlong handle,
                                                                                                  jstring text,
                                                                                                  jintArray rc_out) {
  Boxed* box = box_from(handle);
  if (box == nullptr) {
    write_rc(env, rc_out, -1);
    return nullptr;
  }
  Utf8 utf8_text(env, text);
  std::string out(262144, '\0');
  const int rc =
      apexo_session_summarize_text(box->session, utf8_text.nullable_get(), &out[0], static_cast<int32_t>(out.size()));
  write_rc(env, rc_out, rc);
  if (rc != 0) return nullptr;
  return std_utf8_to_jstring(env, out.c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_apexo_sdk_ApexoSession_nativeLastError(JNIEnv* env, jclass,
                                                                                               jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return std_utf8_to_jstring(env, "session is closed");
  return std_utf8_to_jstring(env, apexo_session_last_error(box->session));
}

extern "C" JNIEXPORT jstring JNICALL Java_com_apexo_sdk_ApexoSession_nativeDisplay(JNIEnv* env, jclass,
                                                                                            jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return std_utf8_to_jstring(env, "");
  return std_utf8_to_jstring(env, apexo_session_display_text(box->session));
}

extern "C" JNIEXPORT jstring JNICALL Java_com_apexo_sdk_ApexoSession_nativeFullTranscript(JNIEnv* env, jclass,
                                                                                                   jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return std_utf8_to_jstring(env, "");
  return std_utf8_to_jstring(env, apexo_session_full_transcript(box->session));
}

extern "C" JNIEXPORT jint JNICALL Java_com_apexo_sdk_ApexoSession_nativeGemmaResident(JNIEnv*, jclass,
                                                                                               jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return 0;
  return apexo_session_gemma_resident(box->session);
}

extern "C" JNIEXPORT void JNICALL Java_com_apexo_sdk_ApexoSession_nativeReset(JNIEnv*, jclass, jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return;
  apexo_session_reset(box->session);
}

extern "C" JNIEXPORT void JNICALL Java_com_apexo_sdk_ApexoSession_nativeRelease(JNIEnv*, jclass, jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return;
  apexo_session_reset(box->session);
}

extern "C" JNIEXPORT jstring JNICALL Java_com_apexo_sdk_ApexoSession_nativeSummarizeNew(JNIEnv* env, jclass,
                                                                                                  jlong handle,
                                                                                                  jintArray rc_out) {
  Boxed* box = box_from(handle);
  if (box == nullptr) {
    write_rc(env, rc_out, -1);
    return nullptr;
  }
  std::string out(262144, '\0');
  const int rc = apexo_session_summarize_new(box->session, &out[0], static_cast<int32_t>(out.size()));
  write_rc(env, rc_out, rc);
  if (rc != 0) return nullptr;
  return std_utf8_to_jstring(env, out.c_str());
}

extern "C" JNIEXPORT void JNICALL Java_com_apexo_sdk_ApexoSession_nativeSetLibraries(JNIEnv* env, jclass, jlong handle,
                                                                                     jstring sherpa_library,
                                                                                     jstring litert_library) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return;
  Utf8 sherpa(env, sherpa_library);
  Utf8 litert(env, litert_library);
  apexo_engine_state_set_libraries(box->engines, sherpa.get(), litert.get());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_apexo_sdk_ApexoSession_nativeSummary(JNIEnv* env, jclass,
                                                                                             jlong handle) {
  Boxed* box = box_from(handle);
  if (box == nullptr) return std_utf8_to_jstring(env, "");
  return std_utf8_to_jstring(env, apexo_session_summary_text(box->session));
}
