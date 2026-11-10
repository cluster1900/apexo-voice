#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
OUT="${1:-$ROOT/dist/macos/session_test}"
mkdir -p "$(dirname "$OUT")" "$ROOT/dist/macos"
if [ ! -f "$ROOT/dist/macos/libapexo_session.dylib" ]; then
  clang++ -std=c++17 -dynamiclib -fPIC -arch "$(uname -m)" \
    -install_name @rpath/libapexo_session.dylib \
    -I "$ROOT/native/include" \
    -o "$ROOT/dist/macos/libapexo_session.dylib" \
    "$ROOT/native/src/session.cpp" \
    "$ROOT/native/src/engine_adapter.cpp"
fi
clang++ -std=c++17 "$ROOT/native/tests/session_test.cpp" \
  -I "$ROOT/native/include" \
  -L "$ROOT/dist/macos" -lapexo_session \
  -Wl,-rpath,"$ROOT/dist/macos" \
  -o "$OUT"
"$OUT"
FAKES="$ROOT/native/tests/build"
mkdir -p "$FAKES"
clang -shared -fPIC -arch "$(uname -m)" \
  -o "$FAKES/fake_sherpa.dylib" \
  "$ROOT/native/tests/fake_sherpa.c"
clang -shared -fPIC -arch "$(uname -m)" \
  -o "$FAKES/fake_litert.dylib" \
  "$ROOT/native/tests/fake_litert.c"
clang++ -std=c++17 "$ROOT/native/tests/adapter_test.cpp" \
  -I "$ROOT/native/include" \
  -L "$ROOT/dist/macos" -lapexo_session \
  -Wl,-rpath,"$ROOT/dist/macos" \
  -o "$FAKES/adapter_test"
"$FAKES/adapter_test" "$FAKES/fake_sherpa.dylib" "$FAKES/fake_litert.dylib"
# Static engine mode: the fakes are linked in as if they were the real engines.
STATIC="$FAKES/static"
mkdir -p "$STATIC"
clang -c -fPIC "$ROOT/native/tests/fake_sherpa.c" -o "$STATIC/fake_sherpa.o"
clang -c -fPIC "$ROOT/native/tests/fake_litert.c" -o "$STATIC/fake_litert.o"
clang++ -std=c++17 -DAPEXO_STATIC_ENGINES -I "$ROOT/native/include" -c "$ROOT/native/src/session.cpp" -o "$STATIC/session.o"
clang++ -std=c++17 -DAPEXO_STATIC_ENGINES -I "$ROOT/native/include" -c "$ROOT/native/src/engine_adapter.cpp" -o "$STATIC/adapter.o"
clang++ -std=c++17 -I "$ROOT/native/include" -c "$ROOT/native/tests/static_engines_test.cpp" -o "$STATIC/test.o"
clang++ "$STATIC/test.o" "$STATIC/session.o" "$STATIC/adapter.o" "$STATIC/fake_sherpa.o" "$STATIC/fake_litert.o" -o "$STATIC/static_engines_test"
( cd "$STATIC" && ./static_engines_test )
node "$ROOT/web/test/session.test.js"
node --import "$ROOT/web/test/register-fakes.js" "$ROOT/web/test/adapter.test.js"
node "$ROOT/web/test/real-sherpa-config.test.js"
node "$ROOT/web/test/asr-presence.test.js"
node "$ROOT/web/test/capture.test.js"
node "$ROOT/web/test/demo-flow.test.js"
node "$ROOT/web/test/asr-pipeline.test.js"
# Opt-in: real recognizer and real model. APEXO_TEST_ASR_DIR holds encoder.onnx, decoder.onnx, joiner.onnx,
# tokens.txt and test_wavs/0.wav; APEXO_SHERPA_LIBRARY is the sherpa-onnx C library (for the native part).
node "$ROOT/web/test/browser-asr.test.js"
if [ -n "${APEXO_TEST_ASR_DIR:-}" ] && [ -n "${APEXO_SHERPA_LIBRARY:-}" ]; then
  clang++ -std=c++17 "$ROOT/native/tests/real_engine_test.cpp" \
    -I "$ROOT/native/include" \
    -L "$ROOT/dist/macos" -lapexo_session \
    -Wl,-rpath,"$ROOT/dist/macos" \
    -o "$FAKES/real_engine_test"
  "$FAKES/real_engine_test" "$APEXO_TEST_ASR_DIR" "$APEXO_TEST_ASR_DIR/test_wavs/0.wav" "${APEXO_TEST_ASR_EXPECT:-研究感兴趣}"
fi
