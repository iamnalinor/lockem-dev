#!/usr/bin/env bash
# Build the lockemd native daemon for both ABIs via the Android NDK, then stage
# the binaries under bin/ with ABI-suffixed names for customize.sh to pick from.
#
# Requirements: an Android NDK (r26+). Point ANDROID_NDK_HOME at it, or let this
# script derive it from $ANDROID_SDK_ROOT/ndk/<version> (highest version wins).
# On Windows, ndk-build is ndk-build.cmd — this script handles both.
set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# --- locate ndk-build -------------------------------------------------------
find_ndk_build() {
  # 1) explicit NDK home
  if [ -n "${ANDROID_NDK_HOME:-}" ]; then
    echo "$ANDROID_NDK_HOME"; return 0
  fi
  # 2) ndk dir inside the SDK (pick the highest version)
  if [ -n "${ANDROID_SDK_ROOT:-}" ] && [ -d "$ANDROID_SDK_ROOT/ndk" ]; then
    # version dirs are plain version numbers; ls+sort -V picks the highest
    # shellcheck disable=SC2012
    ver="$(ls -1 "$ANDROID_SDK_ROOT/ndk" 2>/dev/null | sort -V | tail -n1)"
    if [ -n "$ver" ]; then echo "$ANDROID_SDK_ROOT/ndk/$ver"; return 0; fi
  fi
  return 1
}

NDK="$(find_ndk_build || true)"
NDK_BUILD=""
if [ -n "$NDK" ]; then
  if [ -x "$NDK/ndk-build" ]; then NDK_BUILD="$NDK/ndk-build"
  elif [ -f "$NDK/ndk-build.cmd" ]; then NDK_BUILD="$NDK/ndk-build.cmd"
  fi
fi
# fall back to ndk-build on PATH
if [ -z "$NDK_BUILD" ] && command -v ndk-build >/dev/null 2>&1; then
  NDK_BUILD="$(command -v ndk-build)"
fi

if [ -z "$NDK_BUILD" ]; then
  echo "error: ndk-build not found." >&2
  echo "  Install the NDK (Android Studio > SDK Manager > SDK Tools > NDK, or a" >&2
  echo "  standalone NDK r26+), then set one of:" >&2
  echo "    export ANDROID_NDK_HOME=/path/to/ndk/<version>" >&2
  echo "    export ANDROID_SDK_ROOT=/path/to/Android/Sdk   # has ndk/<version>" >&2
  exit 1
fi

echo "- using ndk-build: $NDK_BUILD"

# --- build ------------------------------------------------------------------
rm -rf src/obj src/libs
"$NDK_BUILD" \
  NDK_PROJECT_PATH=src \
  NDK_APPLICATION_MK=src/jni/Application.mk \
  APP_BUILD_SCRIPT=src/jni/Android.mk

# --- stage binaries ---------------------------------------------------------
mkdir -p bin
cp -f src/libs/arm64-v8a/lockemd bin/lockemd-arm64
cp -f src/libs/x86_64/lockemd   bin/lockemd-x86_64
chmod 0755 bin/lockemd-arm64 bin/lockemd-x86_64

echo "- built: bin/lockemd-arm64, bin/lockemd-x86_64"
ls -l bin/lockemd-arm64 bin/lockemd-x86_64
