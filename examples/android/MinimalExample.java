package com.apexo.example;

import com.apexo.sdk.ApexoSession;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Arrays;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.function.Consumer;

/**
 * Minimal Android integration example for Apexo 0.2.0.
 *
 * <p>Demonstrates:
 * <ul>
 *   <li>Copying ASR and Gemma model files into an accessible app directory</li>
 *   <li>Serial {@code audioExecutor} so the real-time audio capture callback only copies PCM frames
 *       and dispatches {@link ApexoSession#pushPcm(float[], int)} and {@link ApexoSession#endSegment()}
 *       in order</li>
 *   <li>Separate {@code summaryExecutor} for background summarization ({@link ApexoSession#summarize(String)},
 *       {@link ApexoSession#summarizeNew()}, and {@link ApexoSession#summarize()})</li>
 *   <li>Safe shutdown via {@link #close()}</li>
 * </ul>
 */
public final class MinimalExample implements AutoCloseable {
  private static final String[] ASR_FILES = {
    "encoder.onnx", "decoder.onnx", "joiner.onnx", "tokens.txt"
  };

  private final ApexoSession session;
  private final ExecutorService audioExecutor = Executors.newSingleThreadExecutor();
  private final ExecutorService summaryExecutor = Executors.newSingleThreadExecutor();

  public MinimalExample(File asrDir, File gemmaFile, ApexoSession.TranscriptListener listener) {
    this.session =
        new ApexoSession(
            asrDir != null ? asrDir.getAbsolutePath() : "",
            gemmaFile != null ? gemmaFile.getAbsolutePath() : "");
    if (listener != null) {
      this.session.setTranscriptListener(listener);
    }
  }

  /**
   * Copies the four streaming transducer ASR files into {@code targetAsrDir} using the provided
   * asset stream opener (for example {@code context.getAssets()::open}).
   */
  public interface AssetOpener {
    InputStream open(String assetPath) throws IOException;
  }

  public static File copyAsrModelIfMissing(File targetAsrDir, String assetPrefix, AssetOpener opener)
      throws IOException {
    if (!targetAsrDir.exists() && !targetAsrDir.mkdirs()) {
      throw new IOException("Could not create ASR model directory: " + targetAsrDir);
    }
    for (String name : ASR_FILES) {
      File dest = new File(targetAsrDir, name);
      if (dest.isFile() && dest.length() > 0) continue;
      try (InputStream in = opener.open(assetPrefix + "/" + name);
          FileOutputStream out = new FileOutputStream(dest)) {
        byte[] buf = new byte[65536];
        int n;
        while ((n = in.read(buf)) > 0) {
          out.write(buf, 0, n);
        }
      }
    }
    return targetAsrDir;
  }

  /**
   * Real-time audio capture callback entry point: copies the 16 kHz mono float32 buffer and queues
   * decoding onto {@code audioExecutor} so the audio capture thread never blocks on model loading
   * or recognition.
   */
  public void onAudioCaptured16k(
      float[] samples16k, int count, Consumer<String> onDisplayUpdated, Consumer<String> onError) {
    final float[] copy = Arrays.copyOf(samples16k, count);
    audioExecutor.execute(
        () -> {
          int rc = session.pushPcm(copy, ApexoSession.SAMPLE_RATE);
          if (rc == 0) {
            if (onDisplayUpdated != null) {
              onDisplayUpdated.accept(session.displayText());
            }
          } else if (onError != null) {
            onError.accept("pushPcm(" + rc + "): " + session.lastError());
          }
        });
  }

  /**
   * Waits for all already-captured audio chunks on {@code audioExecutor} to finish, then flushes
   * the recognizer tail via {@link ApexoSession#endSegment()}.
   */
  public Future<String> endSegmentAsync() {
    return audioExecutor.submit(
        () -> {
          int rc = session.endSegment();
          if (rc != 0) {
            throw new IllegalStateException("endSegment(" + rc + "): " + session.lastError());
          }
          return session.fullTranscript();
        });
  }

  /**
   * Scenario 3: Stage summary while recording continues on {@code audioExecutor}. Summarizes only
   * finalized lines since the last summary without stopping recording or promoting interim text.
   */
  public Future<String> summarizeNewAsync() {
    return summaryExecutor.submit(session::summarizeNew);
  }

  /**
   * Scenario 4: Drains queued audio and finishes the current segment on {@code audioExecutor}, then
   * runs full cross-chunk summarization on {@code summaryExecutor}.
   */
  public Future<String> endAndSummarizeAsync() {
    return summaryExecutor.submit(
        () -> {
          try {
            endSegmentAsync().get();
          } catch (ExecutionException e) {
            throw new RuntimeException(e.getCause());
          }
          return session.summarize();
        });
  }

  /**
   * Scenario 2: Summary-only on external text without requiring an ASR model or modifying the
   * session transcript.
   */
  public Future<String> summarizeTextAsync(String externalText) {
    return summaryExecutor.submit(() -> session.summarize(externalText));
  }

  public void resetAsync() {
    audioExecutor.execute(session::reset);
  }

  @Override
  public void close() {
    audioExecutor.shutdown();
    summaryExecutor.shutdown();
    try {
      audioExecutor.awaitTermination(10, java.util.concurrent.TimeUnit.SECONDS);
    } catch (InterruptedException ignored) {
      Thread.currentThread().interrupt();
    }
    // Queue session.close() behind any running summary.
    session.close();
  }
}
