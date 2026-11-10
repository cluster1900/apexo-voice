#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
DIST="$ROOT/dist"
ZIG="${ZIG:-zig}"
if [ -z "${JAVA_HOME:-}" ]; then
  JAVA_HOME="$(/usr/libexec/java_home 2>/dev/null || true)"
fi
if [ -z "$JAVA_HOME" ] || [ ! -d "$JAVA_HOME/include" ]; then
  echo "JAVA_HOME must point to a JDK (it is needed for jni.h and javac)." >&2
  exit 1
fi
rm -rf "$DIST"
mkdir -p "$DIST/macos" "$DIST/ios" "$DIST/android" "$DIST/windows" "$DIST/web" "$DIST/obj"

clang++ -std=c++17 -dynamiclib -fPIC -arch arm64 \
  -install_name @rpath/libapexo_session.dylib \
  -I "$ROOT/native/include" \
  -o "$DIST/macos/libapexo_session.dylib" \
  "$ROOT/native/src/session.cpp" \
  "$ROOT/native/src/engine_adapter.cpp"
cp "$ROOT/native/include/apexo_session.h" "$DIST/macos/apexo_session.h"

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

"$ZIG" c++ -target x86_64-windows-gnu -shared -std=c++17 -DAPEXO_BUILDING \
  -I "$ROOT/native/include" \
  -o "$DIST/windows/apexo_session.dll" \
  "$ROOT/native/src/session.cpp" \
  "$ROOT/native/src/engine_adapter.cpp"
cp "$ROOT/native/include/apexo_session.h" "$DIST/windows/apexo_session.h"
rm -f "$DIST/windows/"*.pdb

NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK:-}}"
if [ -z "$NDK" ]; then
  echo "ANDROID_NDK_HOME is required. Zig 0.14 does not ship Android libc." >&2
  exit 1
fi
NDK_BIN=""
for host in darwin-arm64 darwin-x86_64 linux-x86_64; do
  candidate="$NDK/toolchains/llvm/prebuilt/$host/bin"
  if [ -x "$candidate/aarch64-linux-android24-clang++" ]; then
    NDK_BIN="$candidate"
    break
  fi
done
if [ -z "$NDK_BIN" ]; then
  echo "NDK clang++ not found under $NDK" >&2
  exit 1
fi

JNI_INCLUDE="$JAVA_HOME/include"
for pair in "aarch64-linux-android24:arm64-v8a" "x86_64-linux-android24:x86_64"; do
  triple="${pair%%:*}"
  name="${pair##*:}"
  mkdir -p "$DIST/obj/jni/$name"
  "$NDK_BIN/${triple}-clang++" -shared -fPIC -std=c++17 -static-libstdc++ \
    -I "$ROOT/native/include" -I "$JNI_INCLUDE" -I "$JNI_INCLUDE/darwin" \
    -Wl,-soname,libapexo_session.so \
    -o "$DIST/obj/jni/$name/libapexo_session.so" \
    "$ROOT/native/src/session.cpp" "$ROOT/native/src/engine_adapter.cpp" "$ROOT/native/src/jni_session.cpp"
done

CLASS_DIR="$DIST/obj/classes"
mkdir -p "$CLASS_DIR"
javac --release 17 -d "$CLASS_DIR" "$ROOT/native/java/com/apexo/sdk/ApexoSession.java"
jar cf "$DIST/obj/classes.jar" -C "$CLASS_DIR" .
AAR_STAGE="$DIST/obj/aar"
mkdir -p "$AAR_STAGE/jni/arm64-v8a" "$AAR_STAGE/jni/x86_64"
cp "$DIST/obj/jni/arm64-v8a/libapexo_session.so" "$AAR_STAGE/jni/arm64-v8a/"
cp "$DIST/obj/jni/x86_64/libapexo_session.so" "$AAR_STAGE/jni/x86_64/"
cp "$ROOT/native/AndroidManifest.xml" "$AAR_STAGE/AndroidManifest.xml"
cp "$DIST/obj/classes.jar" "$AAR_STAGE/classes.jar"
( cd "$AAR_STAGE" && zip -qr "$DIST/android/apexo-session.aar" . )

( cd "$ROOT/web" && npm pack --pack-destination "$DIST/web" )

echo "packed $DIST"
