#!/bin/sh
set -eu
DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$DIR/../.." && pwd)"
AAR="$ROOT/dist/android/apexo-session.aar"

if [ -z "${JAVA_HOME:-}" ]; then
  JAVA_HOME="$(/usr/libexec/java_home 2>/dev/null || true)"
fi

mkdir -p "$DIR/build/aar" "$DIR/build/classes"
unzip -qo "$AAR" classes.jar -d "$DIR/build/aar"
"$JAVA_HOME/bin/javac" --release 17 \
  -cp "$DIR/build/aar/classes.jar" \
  -d "$DIR/build/classes" \
  "$DIR/MinimalExample.java"
echo "PASS examples/android/MinimalExample.java compiled cleanly against dist/android/apexo-session.aar"
