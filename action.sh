#!/system/bin/sh
# lockem action.sh — open the lockem WebUI config page.
#
# The root manager (Magisk/KernelSU/APatch) runs this when you tap the module's
# "Action" button. Magisk and APatch do not render a module's webroot themselves,
# so this hands off to a standalone WebUI app — KsuWebUI Standalone or WebUI X
# (MMRL). If neither is installed it fetches KsuWebUI Standalone, falling back to
# opening its releases page in a browser.
#
MODID=lockem
KSUWEBUI=io.github.a13e300.ksuwebui
WEBUIX=com.dergoogler.mmrl.wx
API=https://api.github.com/repos/KOWX712/KsuWebUIStandalone/releases/latest
RELEASES=https://github.com/KOWX712/KsuWebUIStandalone/releases/latest

open_ksuwebui() {
  am start -n "$KSUWEBUI/.WebUIActivity" -e id "$MODID" >/dev/null 2>&1
}

open_webuix() {
  am start -n "$WEBUIX/.ui.activity.webui.WebUIActivity" -e MOD_ID "$MODID" >/dev/null 2>&1
}

# Already have a renderer? Prefer KsuWebUI, then WebUI X.
if pm path "$KSUWEBUI" >/dev/null 2>&1; then
  echo "- opening lockem WebUI in KsuWebUI"
  open_ksuwebui
  exit 0
fi
if pm path "$WEBUIX" >/dev/null 2>&1; then
  echo "- opening lockem WebUI in WebUI X"
  open_webuix
  exit 0
fi

# None installed — fetch KsuWebUI Standalone (best-effort). Accept only official
# release assets: the URL is anchored to the repo's releases/download host, so a
# stray link elsewhere in the GitHub API JSON can't be picked. Stage in
# /data/local/tmp — the package installer runs in system_server and cannot read
# /data/adb, so an APK under the module dir would fail to install.
echo "- no WebUI app installed; fetching KsuWebUI Standalone..."
APK=/data/local/tmp/lockem-ksuwebui.apk
URL=$(busybox wget -qO- "$API" 2>/dev/null \
  | grep -oE 'https://github\.com/KOWX712/KsuWebUIStandalone/releases/download/[^"]+\.apk' \
  | head -n1)
ok=0
if [ -n "$URL" ] && busybox wget -qO "$APK" "$URL" 2>/dev/null; then
  pm install -r "$APK" >/dev/null 2>&1 && ok=1
fi
rm -f "$APK"

if [ "$ok" = 1 ] && pm path "$KSUWEBUI" >/dev/null 2>&1; then
  echo "- installed KsuWebUI; opening lockem WebUI"
  open_ksuwebui
else
  echo "- could not auto-install; opening the KsuWebUI releases page"
  echo "  install it, then tap Action again"
  am start -a android.intent.action.VIEW -d "$RELEASES" >/dev/null 2>&1
fi
