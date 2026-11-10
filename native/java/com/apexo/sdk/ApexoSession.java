package com.apexo.sdk;

public final class ApexoSession implements AutoCloseable {
  public static final int SAMPLE_RATE = 16000;
  public static final int ERR_BUSY = -6;

  public static final class TranscriptEvent {
    public final String text;
    public final boolean isFinal;
    public final int startMs;
    public final int endMs;

    public TranscriptEvent(String text, boolean isFinal, int startMs, int endMs) {
      this.text = text;
      this.isFinal = isFinal;
      this.startMs = startMs;
      this.endMs = endMs;
    }
  }

  public interface TranscriptListener {
    void onTranscript(TranscriptEvent event);
  }

  static {
    System.loadLibrary("apexo_session");
  }

  private long handle;
  private volatile TranscriptListener transcriptListener;

  /**
   * @param asrModelDir directory holding encoder.onnx, decoder.onnx, joiner.onnx and tokens.txt, or null/empty for
   *     summary-only usage. When a file is missing at transcription time, {@link #pushPcm} returns -3 and no text
   *     is produced.
   * @param summaryModelPath Gemma 4 E2B .litertlm file for {@link #summarize}, {@link #summarize(String)} and
   *     {@link #summarizeNew()}.
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

  public void setLibraries(String sherpaLibrary, String litertLibrary) {
    long h = enter();
    try {
      nativeSetLibraries(h, sherpaLibrary, litertLibrary);
    } finally {
      exit();
    }
  }

  public void setTranscriptListener(TranscriptListener listener) {
    long h = enter();
    try {
      this.transcriptListener = listener;
      nativeSetTranscriptCallback(h, listener != null ? this : null);
    } finally {
      exit();
    }
  }

  // Invoked from JNI on the caller thread outside the native session mutex.
  @SuppressWarnings("unused")
  private void dispatchTranscript(String text, boolean isFinal, int startMs, int endMs) {
    TranscriptListener listener = this.transcriptListener;
    if (listener != null) {
      listener.onTranscript(new TranscriptEvent(text, isFinal, startMs, endMs));
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

  private static IllegalStateException summarizeException(long h, int rc) {
    String reason = nativeLastError(h);
    if (reason == null || reason.isEmpty()) {
      reason = "summarize failed";
    }
    return new IllegalStateException(reason + " (code " + rc + ")");
  }

  /**
   * Loads Gemma, summarizes the finalized transcript snapshot ({@link #fullTranscript()}) and unloads Gemma.
   * Blocks until done, so call it off the main thread. Does not modify interim text or block {@link #pushPcm}.
   *
   * @return the summary; empty when nothing has been finalized.
   * @throws IllegalStateException when the summary could not be produced; message includes reason and return code.
   */
  public String summarize() {
    long h = enter();
    try {
      int[] rc = new int[1];
      String summary = nativeSummarize(h, rc);
      if (summary == null) {
        throw summarizeException(h, rc[0]);
      }
      return summary;
    } finally {
      exit();
    }
  }

  /**
   * Summarizes caller-supplied text without reading or modifying {@link #fullTranscript()}, {@link #summaryText()},
   * incremental cursor, or ASR state. Empty string returns {@code ""} without loading the model.
   *
   * @param text UTF-8 text to summarize (must not be null).
   * @return the generated summary.
   * @throws IllegalStateException when the summary could not be produced; message includes reason and return code.
   */
  public String summarize(String text) {
    long h = enter();
    try {
      int[] rc = new int[1];
      String summary = nativeSummarizeText(h, text, rc);
      if (summary == null) {
        throw summarizeException(h, rc[0]);
      }
      return summary;
    } finally {
      exit();
    }
  }

  /**
   * Summarizes the finalized speech added since the last summary snapshot and appends to {@link #summaryText()}.
   * Does not modify interim text, reset ASR, or block live {@link #pushPcm} calls while running.
   */
  public String summarizeNew() {
    long h = enter();
    try {
      int[] rc = new int[1];
      String summary = nativeSummarizeNew(h, rc);
      if (summary == null) {
        throw summarizeException(h, rc[0]);
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

  /**
   * Clears transcript, summary, incremental cursor and interim text, and resets the ASR timeline to 0 while keeping
   * any loaded recognizer ready for the next recording.
   */
  public void reset() {
    long h = enter();
    try {
      nativeReset(h);
    } finally {
      exit();
    }
  }

  /**
   * Compatibility alias for {@link #reset()}.
   */
  public void release() {
    reset();
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
      this.transcriptListener = null;
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

  private static native void nativeSetLibraries(long handle, String sherpaLibrary, String litertLibrary);

  private static native void nativeSetTranscriptCallback(long handle, Object target);

  private static native int nativePushPcm(long handle, float[] pcm, int sampleRate);

  private static native int nativeEndSegment(long handle);

  private static native String nativeSummarize(long handle, int[] rcOut);

  private static native String nativeSummarizeText(long handle, String text, int[] rcOut);

  private static native String nativeSummarizeNew(long handle, int[] rcOut);

  private static native String nativeSummary(long handle);

  private static native String nativeLastError(long handle);

  private static native String nativeDisplay(long handle);

  private static native String nativeFullTranscript(long handle);

  private static native int nativeGemmaResident(long handle);

  private static native void nativeReset(long handle);

  private static native void nativeRelease(long handle);
}
