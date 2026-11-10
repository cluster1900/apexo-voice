#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
DIST="$ROOT/dist"
ASSETS="${APEXO_REAL_ASSETS:-$ROOT/native/tests/build/real-assets}"
BUILD_CACHE="${APEXO_BUILD_CACHE:-$ROOT/native/tests/build}"

if [ -z "${ZIG:-}" ]; then
  if command -v zig >/dev/null 2>&1; then
    ZIG="zig"
  elif [ -x "$ASSETS/zig-macos-aarch64-0.14.1/zig" ]; then
    ZIG="$ASSETS/zig-macos-aarch64-0.14.1/zig"
  else
    echo "zig is required for Windows x64 cross-compilation (set ZIG=...)." >&2
    exit 1
  fi
fi

if [ -z "${ANDROID_NDK_HOME:-}" ] && [ -z "${ANDROID_NDK:-}" ]; then
  if [ -d "$ASSETS/android-ndk-r29" ]; then
    ANDROID_NDK_HOME="$ASSETS/android-ndk-r29"
  fi
fi

if [ -z "${JAVA_HOME:-}" ]; then
  JAVA_HOME="$(/usr/libexec/java_home 2>/dev/null || true)"
fi
if [ -z "$JAVA_HOME" ] || [ ! -d "$JAVA_HOME/include" ]; then
  echo "JAVA_HOME must point to a JDK (it is needed for jni.h and javac)." >&2
  exit 1
fi
rm -rf "$DIST"
mkdir -p "$DIST/macos" "$DIST/ios" "$DIST/android" "$DIST/windows" "$DIST/web" "$DIST/obj"

# 1. macOS (arm64)
clang++ -std=c++17 -dynamiclib -fPIC -arch arm64 \
  -install_name @rpath/libapexo_session.dylib \
  -Wl,-rpath,@loader_path \
  -I "$ROOT/native/include" \
  -o "$DIST/macos/libapexo_session.dylib" \
  "$ROOT/native/src/session.cpp" \
  "$ROOT/native/src/engine_adapter.cpp"
cp "$ROOT/native/include/apexo_session.h" "$DIST/macos/apexo_session.h"

SHERPA_MACOS_DIR="$ASSETS/sherpa-onnx-v2.0.1-osx-arm64-shared-no-tts-lib/lib"
LITERT_MACOS_DIR="$ASSETS/litert-c-api/lib/macos_arm64"
if [ -d "$SHERPA_MACOS_DIR" ]; then
  cp "$SHERPA_MACOS_DIR/libsherpa-onnx-c-api.dylib" \
     "$SHERPA_MACOS_DIR/libsherpa-onnx-cxx-api.dylib" \
     "$SHERPA_MACOS_DIR/libonnxruntime.dylib" \
     "$DIST/macos/"
fi
if [ -f "$LITERT_MACOS_DIR/liblitert-lm.dylib" ]; then
  cp "$LITERT_MACOS_DIR/liblitert-lm.dylib" "$DIST/macos/"
fi
if [ -f "$ASSETS/sherpa-onnx-2.0.1/LICENSE" ]; then
  cp "$ASSETS/sherpa-onnx-2.0.1/LICENSE" "$DIST/macos/LICENSE-sherpa-onnx.txt"
fi
if [ -f "$ASSETS/litert-c-api/LICENSE" ]; then
  cp "$ASSETS/litert-c-api/LICENSE" "$DIST/macos/LICENSE-litert-lm.txt"
fi

# 2. iOS (arm64 device + arm64 simulator)
build_apple() {
  sdk="$1"
  target="$2"
  out="$3"
  tag="$4"
  sysroot="$(xcrun --sdk "$sdk" --show-sdk-path)"
  clang++ -std=c++17 -c -arch arm64 -isysroot "$sysroot" -target "$target" \
    -I "$ROOT/native/include" \
    -o "$DIST/obj/${tag}-session.o" \
    "$ROOT/native/src/session.cpp"
  clang++ -std=c++17 -c -arch arm64 -isysroot "$sysroot" -target "$target" \
    -I "$ROOT/native/include" \
    -o "$DIST/obj/${tag}-engine.o" \
    "$ROOT/native/src/engine_adapter.cpp"
  libtool -static -o "$out" "$DIST/obj/${tag}-session.o" "$DIST/obj/${tag}-engine.o"
}

build_apple iphoneos arm64-apple-ios15.0 "$DIST/obj/ios.a" ios
build_apple iphonesimulator arm64-apple-ios15.0-simulator "$DIST/obj/sim.a" sim
mkdir -p "$DIST/obj/headers"
cp "$ROOT/native/include/apexo_session.h" "$DIST/obj/headers/apexo_session.h"
cat > "$DIST/obj/headers/module.modulemap" <<'EOF'
module ApexoSession {
  header "apexo_session.h"
  export *
}
EOF
assemble_xcframework() {
  fw="$DIST/ios/ApexoSession.xcframework"
  rm -rf "$fw"
  mkdir -p "$fw/ios-arm64/Headers" "$fw/ios-arm64-simulator/Headers"
  cp "$DIST/obj/ios.a" "$fw/ios-arm64/libapexo_session.a"
  cp "$DIST/obj/sim.a" "$fw/ios-arm64-simulator/libapexo_session.a"
  cp "$DIST/obj/headers/apexo_session.h" "$DIST/obj/headers/module.modulemap" "$fw/ios-arm64/Headers/"
  cp "$DIST/obj/headers/apexo_session.h" "$DIST/obj/headers/module.modulemap" "$fw/ios-arm64-simulator/Headers/"
  cat > "$fw/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>AvailableLibraries</key>
  <array>
    <dict>
      <key>HeadersPath</key>
      <string>Headers</string>
      <key>LibraryIdentifier</key>
      <string>ios-arm64</string>
      <key>LibraryPath</key>
      <string>libapexo_session.a</string>
      <key>SupportedArchitectures</key>
      <array>
        <string>arm64</string>
      </array>
      <key>SupportedPlatform</key>
      <string>ios</string>
    </dict>
    <dict>
      <key>HeadersPath</key>
      <string>Headers</string>
      <key>LibraryIdentifier</key>
      <string>ios-arm64-simulator</string>
      <key>LibraryPath</key>
      <string>libapexo_session.a</string>
      <key>SupportedArchitectures</key>
      <array>
        <string>arm64</string>
      </array>
      <key>SupportedPlatform</key>
      <string>ios</string>
      <key>SupportedPlatformVariant</key>
      <string>simulator</string>
    </dict>
  </array>
  <key>CFBundlePackageType</key>
  <string>XFWK</string>
  <key>XCFrameworkFormatVersion</key>
  <string>1.0</string>
</dict>
</plist>
EOF
}

mkdir -p "$DIST/obj/ios-slice" "$DIST/obj/sim-slice"
cp "$DIST/obj/ios.a" "$DIST/obj/ios-slice/libapexo_session.a"
cp "$DIST/obj/sim.a" "$DIST/obj/sim-slice/libapexo_session.a"
rm -rf "$DIST/ios/ApexoSession.xcframework"
if ! xcodebuild -create-xcframework \
  -library "$DIST/obj/ios-slice/libapexo_session.a" -headers "$DIST/obj/headers" \
  -library "$DIST/obj/sim-slice/libapexo_session.a" -headers "$DIST/obj/headers" \
  -output "$DIST/ios/ApexoSession.xcframework"; then
  echo "xcodebuild -create-xcframework failed; assembling xcframework from the static libraries" >&2
  assemble_xcframework
fi

if [ -d "$ASSETS/sherpa-ios/SherpaOnnxC.xcframework" ]; then
  cp -R "$ASSETS/sherpa-ios/SherpaOnnxC.xcframework" "$DIST/ios/SherpaOnnxC.xcframework"
fi
if [ -d "$ASSETS/litert-ios/CLiteRTLM.xcframework" ]; then
  cp -R "$ASSETS/litert-ios/CLiteRTLM.xcframework" "$DIST/ios/CLiteRTLM.xcframework"
fi

# 3. Windows (x64)
"$ZIG" c++ -target x86_64-windows-gnu -shared -std=c++17 -DAPEXO_BUILDING \
  -I "$ROOT/native/include" \
  -Wl,--out-implib,"$DIST/windows/apexo_session.lib" \
  -o "$DIST/windows/apexo_session.dll" \
  "$ROOT/native/src/session.cpp" \
  "$ROOT/native/src/engine_adapter.cpp"
cp "$DIST/windows/apexo_session.lib" "$DIST/windows/session.lib"
cp "$ROOT/native/include/apexo_session.h" "$DIST/windows/apexo_session.h"
cat > "$DIST/windows/apexo_session.def" <<'EOF'
LIBRARY apexo_session.dll
EXPORTS
  apexo_session_create
  apexo_session_destroy
  apexo_session_set_transcript_callback
  apexo_session_push_pcm
  apexo_session_end_segment
  apexo_session_summarize
  apexo_session_summarize_new
  apexo_session_summarize_text
  apexo_session_reset
  apexo_session_release
  apexo_session_display_text
  apexo_session_full_transcript
  apexo_session_summary_text
  apexo_session_gemma_resident
  apexo_session_last_error
  apexo_engine_state_create
  apexo_engine_state_set_libraries
  apexo_engine_state_bind
  apexo_engine_state_destroy
EOF
rm -f "$DIST/windows/"*.pdb

SHERPA_WIN_DIR="$ASSETS/sherpa-windows/sherpa-onnx-v2.0.1-win-x64-shared-MD-Release-no-tts-lib/lib"
LITERT_WIN_DIR="$ASSETS/litert-c-api/lib/windows_x86_64"
if [ -d "$SHERPA_WIN_DIR" ]; then
  cp "$SHERPA_WIN_DIR/sherpa-onnx-c-api.dll" \
     "$SHERPA_WIN_DIR/sherpa-onnx-c-api.lib" \
     "$SHERPA_WIN_DIR/onnxruntime.dll" \
     "$SHERPA_WIN_DIR/onnxruntime_providers_shared.dll" \
     "$DIST/windows/"
fi
if [ -f "$LITERT_WIN_DIR/bin/litert-lm.dll" ]; then
  cp "$LITERT_WIN_DIR/bin/litert-lm.dll" "$DIST/windows/"
fi
if [ -f "$LITERT_WIN_DIR/lib/litert-lm.lib" ]; then
  cp "$LITERT_WIN_DIR/lib/litert-lm.lib" "$DIST/windows/"
fi
if [ -f "$ASSETS/sherpa-onnx-2.0.1/LICENSE" ]; then
  cp "$ASSETS/sherpa-onnx-2.0.1/LICENSE" "$DIST/windows/LICENSE-sherpa-onnx.txt"
fi
if [ -f "$ASSETS/litert-c-api/LICENSE" ]; then
  cp "$ASSETS/litert-c-api/LICENSE" "$DIST/windows/LICENSE-litert-lm.txt"
fi

# 4. Android (arm64-v8a + x86_64)
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK:-}}"
if [ -z "$NDK" ]; then
  echo "ANDROID_NDK_HOME is required. Zig 0.14 does not ship Android libc." >&2
  exit 1
fi
NDK_BIN=""
NDK_SYSROOT=""
for host in darwin-arm64 darwin-x86_64 linux-x86_64; do
  candidate="$NDK/toolchains/llvm/prebuilt/$host/bin"
  if [ -x "$candidate/aarch64-linux-android24-clang++" ]; then
    NDK_BIN="$candidate"
    NDK_SYSROOT="$NDK/toolchains/llvm/prebuilt/$host/sysroot"
    break
  fi
done
if [ -z "$NDK_BIN" ]; then
  echo "NDK clang++ not found under $NDK" >&2
  exit 1
fi

JNI_INCLUDE="$JAVA_HOME/include"
for pair in "aarch64-linux-android24:arm64-v8a:aarch64-linux-android:android_arm64" \
            "x86_64-linux-android24:x86_64:x86_64-linux-android:android_x86_64"; do
  IFS=":" read -r triple name sysarch litert_arch <<EOF
$pair
EOF
  mkdir -p "$DIST/obj/jni/$name"
  "$NDK_BIN/${triple}-clang++" -shared -fPIC -std=c++17 -static-libstdc++ \
    -I "$ROOT/native/include" \
    -Wl,-soname,libapexo_session.so \
    -o "$DIST/obj/jni/$name/libapexo_session.so" \
    "$ROOT/native/src/session.cpp" "$ROOT/native/src/engine_adapter.cpp" "$ROOT/native/src/jni_session.cpp"
  if [ -f "$NDK_SYSROOT/usr/lib/$sysarch/libc++_shared.so" ]; then
    cp "$NDK_SYSROOT/usr/lib/$sysarch/libc++_shared.so" "$DIST/obj/jni/$name/"
  fi
  if [ -f "$ASSETS/litert-c-api/lib/$litert_arch/liblitert-lm.so" ]; then
    cp "$ASSETS/litert-c-api/lib/$litert_arch/liblitert-lm.so" "$DIST/obj/jni/$name/"
  fi
done

if [ -f "$BUILD_CACHE/sherpa-android-arm64/lib/libsherpa-onnx-c-api.so" ]; then
  cp "$BUILD_CACHE/sherpa-android-arm64/lib/libsherpa-onnx-c-api.so" "$DIST/obj/jni/arm64-v8a/"
fi
if [ -f "$BUILD_CACHE/sherpa-android-x86_64/_deps/onnxruntime-src/jni/arm64-v8a/libonnxruntime.so" ]; then
  cp "$BUILD_CACHE/sherpa-android-x86_64/_deps/onnxruntime-src/jni/arm64-v8a/libonnxruntime.so" "$DIST/obj/jni/arm64-v8a/"
elif [ -f "$ASSETS/sherpa-onnx-v2.0.1-android-aarch64-termux-shared/lib/libonnxruntime.so" ]; then
  cp "$ASSETS/sherpa-onnx-v2.0.1-android-aarch64-termux-shared/lib/libonnxruntime.so" "$DIST/obj/jni/arm64-v8a/"
fi

if [ -f "$BUILD_CACHE/sherpa-android-x86_64/lib/libsherpa-onnx-c-api.so" ]; then
  cp "$BUILD_CACHE/sherpa-android-x86_64/lib/libsherpa-onnx-c-api.so" "$DIST/obj/jni/x86_64/"
fi
if [ -f "$BUILD_CACHE/sherpa-android-x86_64/_deps/onnxruntime-src/jni/x86_64/libonnxruntime.so" ]; then
  cp "$BUILD_CACHE/sherpa-android-x86_64/_deps/onnxruntime-src/jni/x86_64/libonnxruntime.so" "$DIST/obj/jni/x86_64/"
fi

CLASS_DIR="$DIST/obj/classes"
mkdir -p "$CLASS_DIR"
javac --release 17 -d "$CLASS_DIR" "$ROOT/native/java/com/apexo/sdk/ApexoSession.java"
jar cf "$DIST/obj/classes.jar" -C "$CLASS_DIR" .
AAR_STAGE="$DIST/obj/aar"
mkdir -p "$AAR_STAGE/jni/arm64-v8a" "$AAR_STAGE/jni/x86_64"
cp "$DIST/obj/jni/arm64-v8a/"*.so "$AAR_STAGE/jni/arm64-v8a/"
cp "$DIST/obj/jni/x86_64/"*.so "$AAR_STAGE/jni/x86_64/"
cp "$ROOT/native/AndroidManifest.xml" "$AAR_STAGE/AndroidManifest.xml"
cp "$DIST/obj/classes.jar" "$AAR_STAGE/classes.jar"
if [ -f "$ASSETS/sherpa-onnx-2.0.1/LICENSE" ]; then
  cp "$ASSETS/sherpa-onnx-2.0.1/LICENSE" "$AAR_STAGE/LICENSE-sherpa-onnx.txt"
fi
if [ -f "$ASSETS/litert-c-api/LICENSE" ]; then
  cp "$ASSETS/litert-c-api/LICENSE" "$AAR_STAGE/LICENSE-litert-lm.txt"
fi
( cd "$AAR_STAGE" && zip -qr "$DIST/android/apexo-session.aar" . )

# 5. Web (npm tarball)
( cd "$ROOT/web" && npm pack --pack-destination "$DIST/web" )

echo "packed $DIST"
