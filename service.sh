#!/system/bin/sh
# lockem late_start service. Starts the daemon AFTER boot + /data decrypt.
# IMPORTANT: we do NOT re-apply lock at boot (hidden state persists on its own via
# package-restrictions.xml). Reboot is neutral; the daemon only listens + does the
# boot-combo escape check at startup.

MODDIR=${0%/*}
LDIR=/data/adb/lockem

# wait for boot
until [ "$(getprop sys.boot_completed)" = "1" ]; do
  sleep 2
done

# /data must be decrypted (BFU: nothing to do until first unlock)
[ -d "$LDIR" ] || exit 0

# already running?
if pgrep -f "$MODDIR/bin/lockemd" >/dev/null 2>&1; then
  exit 0
fi

# launch daemon (it performs the boot-combo escape check first, then listens)
"$MODDIR/bin/lockemd" >> "$LDIR/lockem.log" 2>&1 &
