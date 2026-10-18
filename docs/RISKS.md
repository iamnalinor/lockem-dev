# lockem — risks, threat model, and recovery

Read this before relying on lockem. It is deliberately blunt.

## 1. What lockem is (and is not)

lockem is **concealment / obfuscation**, not a cryptographic lock. It defends against:

- a shoulder-surfer or someone casually poking around your phone,
- a person who grabs your already-unlocked phone,
- a quick glance through the app drawer / Settings / recents.

It does **not** defend against forensics or a knowledgeable adversary who has your
device. Here is why this is fundamental, not a bug:

1. The bootloader is unlocked (a prerequisite for root at all).
2. The combo is stored in `/data/adb/lockem/config` **in plaintext** (a deliberate
   decision — we do not encrypt it).
3. There is no at-rest encryption against root.

So anyone who can get root over `/data` (pre-authorized `adb su`; or booting a
root-capable GSI/recovery via `fastboot boot`) can read the plaintext combo **or** just
remove the hiding directly. Hidden packages are enumerable:
`pm list packages -d` / `-a`, or `dumpsys package` → `hidden=true`.

**The core trade-off is unavoidable here:** any recovery path available to you is also a
rollback path available to whoever holds the phone. You cannot simultaneously have "the
owner can always recover" and "a root-capable adversary cannot undo it" while the secret
is plaintext and the bootloader is open.

## 2. Reliability invariants (must never be violated)

- **No bootloops.** `service.sh` only ever runs in the background and never blocks boot.
  The supervisor restarts the daemon with capped backoff and gives up after repeated
  rapid crashes, so a broken daemon can't burn CPU. A kill-switch
  (`touch /data/adb/lockem/disable`) stops it entirely. There are no post-fs-data
  blocking steps. `sepolicy.rule` only adds `allow` rules (safe).
- **No grab in everyday use.** The daemon never holds `EVIOCGRAB` except during the
  short recording window — it cannot "stick" on your buttons. If it crashes mid-record,
  the kernel drops the grab when the fd closes. A crashed daemon is harmless.
- **Critical-package blocklist** (in `actions.c`): the daemon refuses to lock
  `android`, `com.android.systemui`, `com.android.settings`, the launcher, GMS, phone,
  and shell — hiding/force-stopping these could destabilize the system. Package names
  are also validated (`[A-Za-z0-9._]` only) in both the daemon and the CLI, as a guard
  against junk and shell injection.
- **force-stop does not touch data** (it is not clear-data) — it is safe, and is how we
  clear the recents card.
- **Logs are bounded** (256 KB + one rotated backup) — they cannot fill storage.
- **Reboot is neutral.** Hidden flags persist in `package-restrictions.xml`; boot
  neither re-hides nor un-hides. Only the boot-combo escape can change state at boot.

## 3. Per-option risks

| Option | Risk / caveat |
|---|---|
| `strict_disable` | `pm disable` is **global** `DISABLED` (not per-user). From root it may succeed, but `enable` returns the app to *enabled* (not its prior state), and on system/dependent packages it can destabilize the system. Default is `disable-user` (per-user, safe). We fall back to `disable-user` if `disable` is rejected. |
| `opt_lockdown` | No clean shell trigger for true biometric lockdown. Degrades to "lock the screen" (`input keyevent 26` when awake) + a best-effort strong-auth attempt. The shakiest option — verify on device. |
| `opt_private_space` | Uses `am stop-user <ps_id>`. Private Space usually auto-locks on device lock anyway; verify on a real Private Space. |
| recents clearing | A side-effect of `am force-stop`. Whether it removes the exact card must be verified on A16; no guaranteed per-task delete from shell. Harmless if it doesn't. |
| `opt_disable_devopts` | Hides the whole Developer Options section via `development_settings_enabled=0`, restored from `state`. We deliberately **do not** touch `adb_enabled`, so the adb-based recovery path stays alive. |

## 4. Recovery, in order

1. **Reverse combo** — the normal path. The daemon is alive and root-capable
   independently of whether the Magisk app / KsuWebUI is hidden (root was granted to it
   by the manager daemon at boot). `unlock` restores **all** locked packages, including
   the manager app and KsuWebUI if you hid them, so the UI comes back.
2. **Boot-combo escape** — the one fallback we build and that is always on / not
   disableable. Reboot → unlock the lock screen with your PIN → during the daemon's
   startup window (first `escape_window_sec` seconds) **hold the escape key**
   (default Volume-Up) → the daemon unlocks everything. It's checked at daemon start
   (`late_start`, i.e. after the first unlock, since `/data` isn't mounted before —
   BFU). No conflict with bootloader/recovery: those use Power+Vol-Down from a powered-
   off state and are handled by firmware *before* Android/our daemon start.
3. **Pre-authorized adb** — `adb shell su -c 'lockem unlock'` if su was granted to adb
   ahead of time and adb is on.
4. **`lockem unlock` from any root shell** — self-contained, no daemon needed.
5. **External root over `/data`** — if the daemon won't start at all, boot a
   root-capable environment (userdebug GSI / recovery with adb-root) via
   `fastboot boot <img>` — **not** the same patched `init_boot` (that reproduces the
   same state) — and read the plaintext combo, run `lockem unlock`, or remove the module.
   The exact image/method for Pixel 9a is a device-specific TODO.

## 5. Hiding the root manager

The manager daemon keeps granting root to already-granted callers (lockem,
pre-granted adb) from saved policy, so lockem itself won't lose root if you hide the
manager app. But a **new interactive su prompt** can't show its confirmation UI
(the manager Activity is disabled) → it times out → deny. KsuWebUI lives inside the
manager, so hiding it kills the GUI config until unlock (the CLI still works).

**Recommendation:** hide the manager with its own built-in hide (Magisk repackage /
random name), which hides it without breaking su grants; hide ordinary target apps with
lockem; treat hiding the manager/WebUI as opt-in, knowing you fall back to headless CLI.

## 6. Anti-brick hygiene

- Back up `init_boot` before experimenting (standard Pixel/Magisk hygiene). The module
  itself never touches boot.
- Magisk escape: disable the module with
  `touch /data/adb/modules/lockem/disable` (or delete the dir) from adb/recovery.
- Fail-safe daemon: invalid/empty config → it does nothing. No grab is held in everyday
  use, so a crash can never leave your buttons captured.
- `uninstall.sh` is a full best-effort rollback (unhide+enable every package, restore
  settings, stop the daemon), so removing the module returns the phone to normal even
  from a locked state. The *reliable* rollback is `lockem unlock` on a live system first.
