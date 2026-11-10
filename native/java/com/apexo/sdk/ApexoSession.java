package com.apexo.sdk;

public final class ApexoSession {
  static {
    System.loadLibrary("apexo_session");
  }

  private long handle;

  public ApexoSession(String summaryModelPath) {
    handle = nativeCreate(summaryModelPath);
    if (handle == 0) {
      throw new IllegalStateException("apexo_session_create failed");
    }
  }

  public int pushPcm(float[] pcm, int sampleRate) {
    return nativePushPcm(handle, pcm, sampleRate);
  }

  public int endSegment() {
    return nativeEndSegment(handle);
  }

  public String summarize() {
    return nativeSummarize(handle);
  }

  public String displayText() {
    return nativeDisplay(handle);
  }

  public String fullTranscript() {
    return nativeFullTranscript(handle);
  }

  public boolean gemmaResident() {
    return nativeGemmaResident(handle) != 0;
  }

  public void release() {
    nativeRelease(handle);
  }

  public void close() {
    if (handle != 0) {
      nativeDestroy(handle);
      handle = 0;
    }
  }

  private static native long nativeCreate(String summaryModelPath);

  private static native void nativeDestroy(long handle);

  private static native int nativePushPcm(long handle, float[] pcm, int sampleRate);

  private static native int nativeEndSegment(long handle);

  private static native String nativeSummarize(long handle);

  private static native String nativeDisplay(long handle);

  private static native String nativeFullTranscript(long handle);

  private static native int nativeGemmaResident(long handle);

  private static native void nativeRelease(long handle);
}
