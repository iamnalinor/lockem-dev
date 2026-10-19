#!/system/bin/sh
# Best-effort rollback when the module is removed.
# NOTE: Magisk runs this early (framework/pm may be unavailable), so the RELIABLE
# rollback is to run `lockem unlock` from a running system BEFORE removing the module.
# This script still tries, in case pm is reachable, and always restores dev options.

LDIR=/data/adb/lockem

if [ -f "$LDIR/packages.list" ] && command -v pm >/dev/null 2>&1; then
  while IFS= read -r pkg; do
    [ -z "$pkg" ] && continue
    # </dev/null is load-bearing: pm/cmd run app_process, which otherwise swallows
    # this loop's stdin (packages.list) and the unhide/enable silently no-op.
    pm unhide "$pkg" </dev/null 2>/dev/null
    pm enable "$pkg" </dev/null 2>/dev/null
    pm enable --user 0 "$pkg" </dev/null 2>/dev/null
  done < "$LDIR/packages.list"
fi

if [ -f "$LDIR/state" ] && command -v settings >/dev/null 2>&1; then
  # parse (don't source) — state is data, not code
  devopts_prev=$(grep -E '^devopts_prev=' "$LDIR/state" 2>/dev/null | head -n1 | cut -d= -f2-)
  if [ -n "$devopts_prev" ]; then
    if [ "$devopts_prev" = "null" ]; then
      settings delete global development_settings_enabled 2>/dev/null
    else
      settings put global development_settings_enabled "$devopts_prev" 2>/dev/null
    fi
  fi
fi

pkill -f /data/adb/modules/lockem/bin/lockemd 2>/dev/null
rm -rf "$LDIR" 2>/dev/null
