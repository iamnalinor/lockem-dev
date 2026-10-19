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

# --- app_process label/icon helper (optional) -------------------------------
# Compile src/labeldump/LabelDump.java to a single ABI-independent dex that the
# WebUI runs via app_process to resolve real app labels + icons. Best-effort: if
# the SDK platform / build-tools (d8) / a JDK aren't all present, skip it — the
# WebUI then falls back to package names and no icons.
winpath() { if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi; }

build_labeldump() {
  local sdk jar d8
  sdk="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
  if [ -z "$sdk" ] && [ -n "${NDK:-}" ]; then sdk="${NDK%/ndk/*}"; fi
  if [ -z "$sdk" ] || [ ! -d "$sdk" ]; then
    echo "- labeldump: no Android SDK found; skipping (WebUI will show package names)"; return 0
  fi
  # shellcheck disable=SC2012
  jar="$(ls -1 "$sdk/platforms"/*/android.jar 2>/dev/null | sort -V | tail -n1)"
  # shellcheck disable=SC2012
  d8="$(ls -1 "$sdk/build-tools"/*/d8 "$sdk/build-tools"/*/d8.bat 2>/dev/null | sort -V | tail -n1)"
  if ! command -v javac >/dev/null 2>&1 || [ -z "$jar" ] || [ -z "$d8" ]; then
    echo "- labeldump: javac / android.jar / d8 not all present; skipping"; return 0
  fi

  echo "- building labeldump.dex (javac + d8)"
  rm -rf src/labeldump/classes src/labeldump/dexout
  mkdir -p src/labeldump/classes src/labeldump/dexout

  javac -source 8 -target 8 -bootclasspath "$(winpath "$jar")" \
    -d "$(winpath src/labeldump/classes)" "$(winpath src/labeldump/LabelDump.java)"

  "$d8" --min-api 26 --lib "$(winpath "$jar")" --output "$(winpath src/labeldump/dexout)" \
    "$(winpath src/labeldump/classes/com/lockem/LabelDump.class)"

  cp -f src/labeldump/dexout/classes.dex bin/labeldump.dex
  rm -rf src/labeldump/classes src/labeldump/dexout
  echo "- built: bin/labeldump.dex"
  ls -l bin/labeldump.dex
}

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

# --- app_process helper (best-effort) ---------------------------------------
build_labeldump
