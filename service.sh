#!/system/bin/sh
# lockem late_start service. Starts (and supervises) the daemon AFTER boot +
# /data decrypt.
#
# IMPORTANT: we do NOT re-apply lock at boot — hidden state persists on its own via
# package-restrictions.xml. Reboot is neutral; the daemon only listens + does the
# boot-combo escape check at startup.
#
# Kill-switch: `touch /data/adb/lockem/disable` makes this exit and the supervisor
# stop (checked at boot and between restarts). Magisk's own
# `/data/adb/modules/lockem/disable` disables the whole module one level up.

MODDIR=${0%/*}
LDIR=/data/adb/lockem
DAEMON="$MODDIR/bin/lockemd"
LOG="$LDIR/lockem.log"

# wait for boot
until [ "$(getprop sys.boot_completed)" = "1" ]; do
  sleep 2
done

# /data must be decrypted (BFU: nothing to do until first unlock)
[ -d "$LDIR" ] || exit 0

# kill-switch honored at boot
[ -f "$LDIR/disable" ] && exit 0

# already running? (re-exec guard)
if pgrep -f "$DAEMON" >/dev/null 2>&1; then
  exit 0
fi

supervise() {
  # Restart the daemon with capped exponential backoff. If it keeps crashing
  # fast, give up so a persistently broken daemon can never burn CPU in a tight
  # loop (a crashed daemon is harmless — the feature just stops working).
  delay=2
  fails=0
  while :; do
    [ -f "$LDIR/disable" ] && { echo "[supervisor] disable flag set; exiting" >> "$LOG"; break; }
    [ -x "$DAEMON" ] || { echo "[supervisor] daemon missing; exiting" >> "$LOG"; break; }

    start=$(date +%s 2>/dev/null || echo 0)
    "$DAEMON" >> "$LOG" 2>&1
    end=$(date +%s 2>/dev/null || echo 0)

    [ -f "$LDIR/disable" ] && { echo "[supervisor] disable flag set; exiting" >> "$LOG"; break; }

    ran=$((end - start))
    if [ "$ran" -ge 60 ]; then
      # healthy run: reset backoff and the rapid-crash counter
      delay=2
      fails=0
    else
      fails=$((fails + 1))
      if [ "$fails" -ge 8 ]; then
        echo "[supervisor] daemon crashed $fails times rapidly; giving up" >> "$LOG"
        break
      fi
    fi
    echo "[supervisor] daemon exited (ran ${ran}s); restart in ${delay}s" >> "$LOG"
    sleep "$delay"
    delay=$((delay * 2))
    [ "$delay" -gt 60 ] && delay=60
  done
}

# run the supervisor in the background so boot is never blocked
supervise &
