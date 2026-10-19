#!/system/bin/sh
# lockem installer. Runs in Magisk/KernelSU/APatch install context.
# Picks the right-ABI daemon, lays out /data/adb/lockem, sets perms.

ui_print "- lockem installer"
ui_print "- detected ABI: $ABI"

case "$ABI" in
  arm64-v8a) BIN=lockemd-arm64 ;;
  x86_64)    BIN=lockemd-x86_64 ;;
  *) abort "! Unsupported ABI '$ABI' (need arm64-v8a or x86_64)" ;;
esac

if [ ! -f "$MODPATH/bin/$BIN" ]; then
  abort "! Missing daemon binary $BIN — build it first (see build.sh)"
fi

mv -f "$MODPATH/bin/$BIN" "$MODPATH/bin/lockemd"
rm -f "$MODPATH/bin/"lockemd-arm64 "$MODPATH/bin/"lockemd-x86_64 2>/dev/null

LDIR=/data/adb/lockem
mkdir -p "$LDIR/ctl"
# do not clobber an existing config on upgrade
if [ ! -f "$LDIR/config" ]; then
  cp -f "$MODPATH/default-config" "$LDIR/config"
  ui_print "- wrote default config"
fi
[ -f "$LDIR/packages.list" ] || : > "$LDIR/packages.list"
chmod 0700 "$LDIR"

set_perm "$MODPATH/bin/lockemd"       0 0 0755
set_perm "$MODPATH/system/bin/lockem" 0 0 0755
set_perm "$MODPATH/action.sh"         0 0 0755
# optional app_process label/icon helper (ABI-independent dex); read-only is enough
[ -f "$MODPATH/bin/labeldump.dex" ] && set_perm "$MODPATH/bin/labeldump.dex" 0 0 0644

ui_print "- installed. Tap the module's Action button to open the WebUI"
ui_print "  (installs KsuWebUI if no WebUI app is present), then reboot."
ui_print "- recovery: hold Volume-Up during daemon startup to unlock-all."
