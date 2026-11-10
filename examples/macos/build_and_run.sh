#!/bin/sh
set -eu
DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$DIR/../.." && pwd)"
DIST="$ROOT/dist/macos"

clang -std=c11 -O2 -arch arm64 \
  -I "$DIST" \
  -L "$DIST" -lapexo_session \
  -Wl,-rpath,"$DIST" \
  -o "$DIR/minimal_example" \
  "$DIR/minimal_example.c"

if [ $# -ge 3 ]; then
  exec "$DIR/minimal_example" "$@"
fi
