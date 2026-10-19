#!/usr/bin/env bash
# Assemble the installable module zip from built binaries + scripts + webroot.
# Run build.sh first (needs bin/lockemd-arm64 and bin/lockemd-x86_64).
# Output: dist/lockem-<version>.zip (install via the Magisk/KernelSU/APatch manager app).
# The <version> comes from module.prop's `version=` field.
set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

if [ ! -f bin/lockemd-arm64 ] || [ ! -f bin/lockemd-x86_64 ]; then
  echo "error: missing bin/lockemd-arm64 / bin/lockemd-x86_64 — run build.sh first." >&2
  exit 1
fi

STAGE="dist/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/system/bin" "$STAGE/webroot"

# module root files
cp module.prop customize.sh service.sh uninstall.sh sepolicy.rule default-config action.sh "$STAGE/"
# daemon binaries (customize.sh renames the right one to lockemd on install)
cp bin/lockemd-arm64 bin/lockemd-x86_64 "$STAGE/bin/"
# CLI + WebUI
cp system/bin/lockem "$STAGE/system/bin/"
cp webroot/index.html webroot/app.js webroot/style.css "$STAGE/webroot/"

# version drives the archive name (e.g. v0.1.0 -> dist/lockem-v0.1.0.zip)
VERSION="$(sed -n 's/^version=//p' module.prop | head -n1)"
[ -n "$VERSION" ] || { echo "error: could not read version= from module.prop" >&2; exit 1; }

OUT="dist/lockem-$VERSION.zip"
rm -f "$OUT"
# -X strips extra attributes for a reproducible-ish archive
( cd "$STAGE" && zip -r -X "../lockem-$VERSION.zip" . >/dev/null )
rm -rf "$STAGE"

echo "- packaged: $OUT"
ls -l "$OUT"
