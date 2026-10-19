#!/usr/bin/env bash
# Build and run the host unit tests with ASan + UBSan. Pure host build (gcc),
# no NDK/device needed — this is what the CI `test` job runs.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
CC="${CC:-gcc}"
CFLAGS="-std=c11 -D_GNU_SOURCE -Wall -Wextra -g -fsanitize=address,undefined -fno-sanitize-recover=all"

echo "- compiler: $($CC --version | head -n1)"

build_and_run() {
  out="$1"; shift
  # CFLAGS is intentionally word-split into separate args
  # shellcheck disable=SC2086
  "$CC" $CFLAGS -I "$SRC" "$@" -o "$out"
  "$out"
}

build_and_run "$HERE/run_pattern" "$HERE/test_pattern.c" "$SRC/pattern.c" "$SRC/config.c"
build_and_run "$HERE/run_pkgname" "$HERE/test_pkgname.c" "$SRC/pkgname.c"
build_and_run "$HERE/run_config" "$HERE/test_config.c" "$SRC/config.c"
