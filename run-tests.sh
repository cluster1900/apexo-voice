#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
OUT="${1:-$ROOT/dist/macos/session_test}"
mkdir -p "$(dirname "$OUT")"
clang++ -std=c++17 "$ROOT/native/tests/session_test.cpp" \
  -I "$ROOT/native/include" \
  -L "$ROOT/dist/macos" -lapexo_session \
  -Wl,-rpath,"$ROOT/dist/macos" \
  -o "$OUT"
"$OUT"
FAKES="$ROOT/native/tests/build"
mkdir -p "$FAKES"
clang -shared -fPIC -arch arm64 \
  -o "$FAKES/fake_sherpa.dylib" \
  "$ROOT/native/tests/fake_sherpa.c"
clang -shared -fPIC -arch arm64 \
  -o "$FAKES/fake_litert.dylib" \
  "$ROOT/native/tests/fake_litert.c"
clang++ -std=c++17 "$ROOT/native/tests/adapter_test.cpp" \
  -I "$ROOT/native/include" \
  -L "$ROOT/dist/macos" -lapexo_session \
  -Wl,-rpath,"$ROOT/dist/macos" \
  -o "$FAKES/adapter_test"
"$FAKES/adapter_test" "$FAKES/fake_sherpa.dylib" "$FAKES/fake_litert.dylib"
node "$ROOT/web/test/session.test.js"
node --import "$ROOT/web/test/register-fakes.js" "$ROOT/web/test/adapter.test.js"
node "$ROOT/web/test/real-sherpa-config.test.js"
node "$ROOT/web/test/asr-presence.test.js"
node "$ROOT/web/test/capture.test.js"
node "$ROOT/web/test/demo-flow.test.js"
