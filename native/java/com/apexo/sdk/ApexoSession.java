package com.apexo.sdk;

public final class ApexoSession implements AutoCloseable {
  static {
    System.loadLibrary("apexo_session");
  }

  private long handle;

  /**
   * @param asrModelDir directory holding encoder.onnx, decoder.onnx, joiner.onnx and tokens.txt. When a file is
   *     missing, {@link #pushPcm} returns -3 and no text is produced.
   * @param summaryModelPath Gemma 4 E2B .litertlm file for {@link #summarize}.
   */
  public ApexoSession(String asrModelDir, String summaryModelPath) {
    handle = nativeCreate(asrModelDir, summaryModelPath);
    if (handle == 0) {
      throw new IllegalStateException("apexo_session_create failed");
    }
  }

  // Calls in flight. close() while a call (for example summarize on a worker thread) is running only marks the
  // session closed; the last call to return destroys the native session, so nothing runs on freed memory.
  private int active;
  private boolean closing;

  private long enter() {
    synchronized (this) {
      if (closing || handle == 0) {
        throw new IllegalStateException("session is closed");
      }
      active += 1;
      return handle;
    }
  }

  private void exit() {
    long toDestroy = 0;
    synchronized (this) {
      active -= 1;
      if (closing && active == 0 && handle != 0) {
        toDestroy = handle;
        handle = 0;
      }
    }
    if (toDestroy != 0) {
      nativeDestroy(toDestroy);
    }
  }

  public int pushPcm(float[] pcm, int sampleRate) {
    long h = enter();
    try {
      return nativePushPcm(h, pcm, sampleRate);
    } finally {
      exit();
    }
  }

  public int endSegment() {
    long h = enter();
    try {
      return nativeEndSegment(h);
    } finally {
      exit();
    }
  }

  /**
   * Loads Gemma, writes the summary and unloads it. Blocks until done, so call it off the main thread.
   *
   * @return the summary; empty when nothing has been transcribed.
   * @throws IllegalStateException when the summary could not be produced; the message is the reason.
   */
  public String summarize() {
    long h = enter();
    try {
      String summary = nativeSummarize(h);
      if (summary == null) {
        throw new IllegalStateException(nativeLastError(h));
      }
      return summary;
    } finally {
      exit();
    }
  }

  /**
   * Summarizes the speech added since the last summary, including the line still being recognized (it is closed
   * first, as {@link #endSegment()} does), and appends to {@link #summaryText()}.
   * Does not block live {@link #pushPcm} calls while running.
   */
  public String summarizeNew() {
    long h = enter();
    try {
      String summary = nativeSummarizeNew(h);
      if (summary == null) {
        throw new IllegalStateException(nativeLastError(h));
      }
      return summary;
    } finally {
      exit();
    }
  }

  public String summaryText() {
    long h = enter();
    try {
      return nativeSummary(h);
    } finally {
      exit();
    }
  }

  public String displayText() {
    long h = enter();
    try {
      return nativeDisplay(h);
    } finally {
      exit();
    }
  }

  public String fullTranscript() {
    long h = enter();
    try {
      return nativeFullTranscript(h);
    } finally {
      exit();
    }
  }

  public String lastError() {
    long h = enter();
    try {
      return nativeLastError(h);
    } finally {
      exit();
    }
  }

  public boolean gemmaResident() {
    long h = enter();
    try {
      return nativeGemmaResident(h) != 0;
    } finally {
      exit();
    }
  }

  public void release() {
    long h = enter();
    try {
      nativeRelease(h);
    } finally {
      exit();
    }
  }

  /**
   * Closes the session. When a summary is still running on another thread this returns at once and the session
   * is destroyed as soon as that call returns; calls started after close() throw IllegalStateException.
   */
  public void close() {
    long toDestroy = 0;
    synchronized (this) {
      if (closing || handle == 0) {
        return;
      }
      closing = true;
      if (active == 0) {
        toDestroy = handle;
        handle = 0;
      }
    }
    if (toDestroy != 0) {
      nativeDestroy(toDestroy);
    }
  }

  private static native long nativeCreate(String asrModelDir, String summaryModelPath);

  private static native void nativeDestroy(long handle);

  private static native int nativePushPcm(long handle, float[] pcm, int sampleRate);

  private static native int nativeEndSegment(long handle);

  private static native String nativeSummarize(long handle);

  private static native String nativeSummarizeNew(long handle);

  private static native String nativeSummary(long handle);

  private static native String nativeLastError(long handle);

  private static native String nativeDisplay(long handle);

  private static native String nativeFullTranscript(long handle);

  private static native int nativeGemmaResident(long handle);

  private static native void nativeRelease(long handle);
}
