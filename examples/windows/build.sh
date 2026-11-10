#!/bin/sh
set -eu
DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$DIR/../.." && pwd)"
DIST="$ROOT/dist/windows"
ZIG="${ZIG:-$ROOT/native/tests/build/real-assets/zig-macos-aarch64-0.14.1/zig}"
LLVM_READOBJ="${LLVM_READOBJ:-$ROOT/native/tests/build/real-assets/android-ndk-r29/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readobj}"

mkdir -p "$DIR/build"

# 1. Cross-compile minimal_example.exe and windows_test.exe against dist/windows/apexo_session.lib
"$ZIG" cc -target x86_64-windows-gnu -std=c11 -O2 \
  -I "$DIST" \
  "$DIR/minimal_example.c" \
  "$DIST/apexo_session.lib" -lshell32 \
  -o "$DIR/build/minimal_example.exe"

"$ZIG" c++ -target x86_64-windows-gnu -std=c++17 -O2 \
  -I "$DIST" \
  "$DIR/windows_test.cpp" \
  "$DIST/apexo_session.lib" -lshell32 \
  -o "$DIR/build/windows_test.exe"

# Verify static engine mode compiles cleanly for Windows x64 without dllimport conflicts
"$ZIG" c++ -target x86_64-windows-gnu -std=c++17 -O2 -DAPEXO_STATIC_ENGINES \
  -I "$ROOT/native/include" \
  -c "$ROOT/native/src/session.cpp" \
  -o "$DIR/build/static_session_win.obj"
"$ZIG" c++ -target x86_64-windows-gnu -std=c++17 -O2 -DAPEXO_STATIC_ENGINES \
  -I "$ROOT/native/include" \
  -c "$ROOT/native/src/engine_adapter.cpp" \
  -o "$DIR/build/static_adapter_win.obj"

# 2. Build and run host-side Windows path & ABI verification + windows_test contract runner
clang++ -std=c++17 -O2 "$DIR/windows_host_verify.cpp" -o "$DIR/build/windows_host_verify"
"$DIR/build/windows_host_verify"

clang++ -std=c++17 -O2 -I "$DIST" "$DIR/windows_test.cpp" \
  "$ROOT/dist/macos/libapexo_session.dylib" \
  -Wl,-rpath,"$ROOT/dist/macos" \
  -o "$DIR/build/windows_test_host"
"$DIR/build/windows_test_host"

# 3. Verify PE/COFF exports & imports of dist/windows binaries and examples
if [ -x "$LLVM_READOBJ" ]; then
  EXPORTS="$("$LLVM_READOBJ" --coff-exports "$DIST/apexo_session.dll")"
  for sym in \
    apexo_session_create \
    apexo_session_destroy \
    apexo_session_set_transcript_callback \
    apexo_session_push_pcm \
    apexo_session_end_segment \
    apexo_session_summarize \
    apexo_session_summarize_new \
    apexo_session_summarize_text \
    apexo_session_reset \
    apexo_session_release \
    apexo_session_display_text \
    apexo_session_full_transcript \
    apexo_session_summary_text \
    apexo_session_gemma_resident \
    apexo_session_last_error \
    apexo_engine_state_create \
    apexo_engine_state_set_libraries \
    apexo_engine_state_bind \
    apexo_engine_state_destroy; do
    echo "$EXPORTS" | grep -q "Name: $sym$" || {
      echo "FAIL Windows DLL missing export: $sym" >&2
      exit 1
    }
  done
  echo "PASS dist/windows/apexo_session.dll exports all 19 Apexo 0.2.0 C API symbols"

  IMPORTS="$("$LLVM_READOBJ" --coff-imports "$DIR/build/windows_test.exe")"
  echo "$IMPORTS" | grep -q "apexo_session_summarize_text" && \
    echo "$IMPORTS" | grep -q "apexo_session_reset" && \
    echo "$IMPORTS" | grep -q "apexo_session_set_transcript_callback" || {
      echo "FAIL windows_test.exe missing 0.2.0 imports from apexo_session.lib" >&2
      exit 1
    }
  echo "PASS minimal_example.exe and windows_test.exe link cleanly against dist/windows/apexo_session.lib"

  if [ -f "$DIST/sherpa-onnx-c-api.dll" ] && [ -f "$DIST/litert-lm.dll" ]; then
    "$LLVM_READOBJ" --coff-exports "$DIST/sherpa-onnx-c-api.dll" | grep -q "SherpaOnnxOnlineStreamInputFinished" || {
      echo "FAIL sherpa-onnx-c-api.dll missing SherpaOnnxOnlineStreamInputFinished" >&2
      exit 1
    }
    "$LLVM_READOBJ" --coff-exports "$DIST/litert-lm.dll" | grep -q "litert_lm_engine_create" || {
      echo "FAIL litert-lm.dll missing litert_lm_engine_create" >&2
      exit 1
    }
    echo "PASS dist/windows runtime DLLs (sherpa-onnx-c-api.dll, litert-lm.dll) verified"
  fi
fi
