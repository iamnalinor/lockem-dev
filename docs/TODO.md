# TODO

## Disable-mode selector

Replace the removed `strict_disable` boolean with a per-config picker for how a
locked app is hidden. Each mode needs a matching restore on unlock. Two modes:

1. **hide + disable** *(current behavior / default)* — `pm hide` + global
   `pm disable <pkg>` (fall back to `pm disable-user --user 0 <pkg>` if rejected).
   App disappears from Settings → Apps entirely. Restore: `pm unhide` + `pm enable`.

2. **uninstall, keep apk + data** — `pm uninstall -k --user 0 <pkg>` removes the
   app for user 0 but **keeps** its data and cache. Strongest hiding (looks truly
   not-installed). Restore on unlock: `pm install-existing --user 0 <pkg>`.
   To validate on-device: that data/cache actually survive the uninstall, that
   `install-existing` brings the app back for user 0, and behavior for apps that
   are not system/preloaded (whether the APK is still resolvable for
   `install-existing`).

Notes:
- `hide + disable-user` was considered and dropped: it leaves the app visible and
  re-enablable in Settings, which defeats the lock.
- Keep the package blocklist + the WebUI system/Magisk confirm as guards for every
  mode, since uninstall/global-disable on the wrong package can destabilize the
  system.
