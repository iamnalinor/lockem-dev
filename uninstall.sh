#!/system/bin/sh
# Best-effort rollback when the module is removed.
# NOTE: Magisk runs this early (framework/pm may be unavailable), so the RELIABLE
# rollback is to run `lockem unlock` from a running system BEFORE removing the module.
# This script still tries, in case pm is reachable, and always restores dev options.

LDIR=/data/adb/lockem

if [ -f "$LDIR/packages.list" ] && command -v pm >/dev/null 2>&1; then
  while IFS= read -r pkg; do
    [ -z "$pkg" ] && continue
    pm unhide "$pkg" 2>/dev/null
    pm enable "$pkg" 2>/dev/null
    pm enable --user 0 "$pkg" 2>/dev/null
  done < "$LDIR/packages.list"
fi

if [ -f "$LDIR/state" ]; then
  . "$LDIR/state" 2>/dev/null
  if [ -n "$devopts_prev" ] && command -v settings >/dev/null 2>&1; then
    settings put global development_settings_enabled "$devopts_prev" 2>/dev/null
  fi
fi

pkill -f /data/adb/modules/lockem/bin/lockemd 2>/dev/null
rm -rf "$LDIR" 2>/dev/null
