# lockem — testing guide

Two tracks: a rooted emulator (functional logic) and a real rooted device (things the
VM can't be trusted for). Plus host unit tests that need neither.

> lockem is meant to run on any rooted Android device. So far it has only been tested
> on a **Pixel 9a / Android 17**, which is the reference device named under Track B —
> substitute your own device there.

## 0. Host unit tests (no device, no NDK)

```bash
bash tests/run_tests.sh
```

Compiles `pattern.c` + `config.c` with gcc and AddressSanitizer/UBSan and runs the
suite: pattern parse/serialize round-trip, the matcher (click vs hold timing, reset,
parallel press/unpress), and the recording normalizer. This is also the CI `test` job.

## 1. Emulator (Track A) — functional logic

### ⚠️ Caveats before you trust the VM

- **A Play Store (Google Play) image gives no root** — `adb root` is refused, and the
  rooting tools' su-grant step needs it. Use a **Google APIs (not Play Store)** image.
- Many emulator images run **SELinux permissive** → AVC denials will **not** surface on
  the VM but **will** on a real enforcing phone. So the VM does not validate
  `sepolicy.rule` or real evdev access by the daemon.
- Vibration and real biometric lockdown have no physical effect on a VM.

### Suggested image

To mirror the Pixel 9a (16 KB pages), use the **`google_apis_ps16k`** image — it is
both 16 KB-page (`ps16k`) and non-Play-Store (so it can be rooted). The 16 KB page is
the one thing the VM uniquely exercises about the build: that the daemon, linked with
`-Wl,-z,max-page-size=16384` (already in `Application.mk`), actually loads under 16 KB
pages. Everything else (hide/unhide, the matcher, config, WebUI) is page-size-agnostic,
so a plain 4 KB `google_apis` image is a fine fallback if a 16 KB AVD won't root.

> Do **not** use a `..._playstore_...` image — it blocks root. The non-Play-Store
> `google_apis_ps16k` variant is the one you want.

### Setup

**1. Create a non-Play-Store 16 KB AVD** (needs the Android SDK cmdline-tools on PATH;
confirm the exact tag with `sdkmanager --list | grep ps16k` — in Android Studio, 16 KB
images are under the **Other Images** tab):
```bash
sdkmanager "system-images;android-36;google_apis_ps16k;x86_64"
avdmanager create avd -n lockem16k -k "system-images;android-36;google_apis_ps16k;x86_64"
```

**2. Root it with avdroot** (<https://github.com/ejfkdev/avdroot>) — a pure-Go tool that
patches the AVD ramdisk with Magisk on the host. Prefer it over
[rootAVD](https://github.com/newbit1/rootAVD): rootAVD relies on a bundled busybox that
fails to run on recent / 16 KB-page guest images (`Can not find any working Busybox
Version`), whereas avdroot does the patch host-side with no busybox and no in-guest code.

Install it either from a release binary or with Go:
```bash
go install github.com/ejfkdev/avdroot@latest   # or download a prebuilt binary from Releases
```
Then root the AVD. Download a **stable** `Magisk-vXX.apk` from
<https://github.com/topjohnwu/Magisk/releases> and pass it with `--magisk` — avdroot
otherwise fetches the newest release (possibly a pre-release) and does not verify the
APK signature, so supplying the file yourself is both a version pin and a supply-chain
guard:
```bash
avdroot doctor                         # confirm it found the SDK/AVD (else pass --sdk PATH)
avdroot list                           # AVD name + patch status
avdroot patch --dry-run lockem16k      # preview; writes nothing
avdroot root lockem16k --magisk /path/to/Magisk-v28.1.apk
avdroot verify lockem16k               # expects su -> uid=0
```
`avdroot restore lockem16k` rolls the ramdisk back. On Windows the emulator is restarted
as `-avd NAME`; pass `--emulator-args "…"` to keep extra flags. Note avdroot is young and
its author verified Android 17 end-to-end only on arm64 macOS — if it can't root a 16 KB
x86_64 AVD on your host, fall back to a plain `google_apis;x86_64` (4 KB) image for the
functional pass and leave the 16 KB-load check to the real device (Track B).

**3. Build the x86_64 daemon and package** (on your build host, needs an NDK r26+):
```bash
export ANDROID_NDK_HOME="$ANDROID_SDK_ROOT/ndk/<version>"   # or set ANDROID_SDK_ROOT
bash build.sh && bash pack.sh                               # -> dist/lockem-<version>.zip
```
`build.sh` also compiles the app-label/icon helper to `bin/labeldump.dex` when a JDK plus
an SDK platform (`android.jar`) and build-tools (`d8`) are found (set `ANDROID_SDK_ROOT`);
if they aren't, it skips the dex and the WebUI shows package names (no labels/icons).

**4. Install the module and reboot.** Push the zip, then install it through the root
manager app (Magisk/KernelSU/APatch → Modules → Install from storage), and reboot:
```bash
adb push dist/lockem-*.zip /sdcard/Download/
# install the lockem-<version>.zip from the manager app UI, then:
adb reboot
```
After reboot, verify from a root shell: `adb shell su -c 'lockem status'` and
`adb shell su -c 'lockem log'`.

### What the VM validates (trust these)

`pm hide/unhide/disable/enable`, the full lock/unlock sequences, config parsing, the
matcher against a live event stream, the temporary record grab, `am force-stop` /
recents behavior (approximately), Private Space `am stop-user`, auto-lock by
screen-off/timeout, and `uninstall.sh` rollback. Also confirm power/volume arrive as
evdev events (`getevent -lp`) and are read passively.

### What the VM does **not** validate (defer to the phone)

SELinux (permissive), real vibration / biometric lockdown, real recents-card removal and
Private Space behavior on hardware, and whether `pm hide` really removes an app from
Settings → Apps → See all on A17.

## 2. Smoke run (after installing the zip)

1. Daemon came up: `lockem status` (or `ps -A | grep lockemd`), and `lockem log`.
   Ordinary power/volume still work (no grab held in everyday use).
2. In the WebUI pick 1–2 test apps and record the lock and unlock combos.
3. Lock combo → target apps vanish from the drawer / Settings / recents; options fired.
4. Unlock combo → everything returns.
5. Reboot while locked → apps stay hidden **on their own** (flags persist); the daemon
   starts; lock is **not** re-applied.
6. Boot-escape → hold Volume-Up during daemon startup → everything unlocks.
7. Remove the module (or `lockem unlock`) → full rollback.
8. Fail-safe: `kill` the daemon while locked → buttons keep working (no grab was held);
   apps recoverable via `pm enable` / reinstall.
9. App list shows **real labels + icons** (resolved by `bin/labeldump.dex` via
   `app_process`); an app with no label falls back to its package name, a missing icon
   simply shows none. The list is three blocks in order **Apps / Magisk-related / System
   apps**; "Show system apps" reveals the last. (If `labeldump.dex` wasn't built — no SDK
   build-tools at build time — the list degrades to package names with no icons.)
10. **Custom command** (Advanced): set a lock and/or unlock command, try both `shell` and
    `root`, and both `before`/`after` — trigger lock/unlock and confirm it ran (right
    order) and that its output + exit code show in the log (`lockem log` / WebUI viewer).
11. **Stop recording**: start a record, press the button again → the grab ends at once and
    the previous combo is kept (`status=cancelled`).
12. Command output (all actions) is now logged — `lockem log` shows `pm hide … rc=0` etc.
13. Unlock restores **every** listed package unconditionally (`pm unhide`/`enable`),
    regardless of its state when locked — by design.

## 3. Real device (Track B) — enforcing hardware

Reference device tested: **Pixel 9a / Android 17** — substitute your own. Repeat the
smoke run, then close what the VM couldn't:

- **SELinux:** collect real denials — `dmesg | grep 'avc: denied'` — for reading
  `/dev/input/*` (and the `EVIOCGRAB` ioctl during recording), and for the binder calls
  behind `cmd vibrator_manager` and `cmd lock_settings`. Extend `sepolicy.rule` with the
  exact types/classes from the denials (don't guess types).
- **Passive match on hardware buttons:** the daemon catches the combo from the live
  stream; incidental system reactions are tolerated. The temporary record grab really
  silences button reactions while recording.
- **Record handshake:** a single Record press runs exactly one grab (one
  `record: start` in the log), and a timed-out/empty capture must NOT wipe a good
  pattern (log shows `kept previous pattern`, config unchanged).
- **Vibration:** confirm a tight double buzz and check the log for `vibrate: ok via '…'`
  (the chain tries a single `cmd vibrator_manager synced -f waveform -a …` first, then
  falls back to two `oneshot` pulses).
- **Lockdown:** `opt_lockdown` runs `cmd lock_settings require-strong-auth
  STRONG_AUTH_REQUIRED_AFTER_USER_LOCKDOWN`; verify the next unlock is forced to
  PIN/pattern (biometrics rejected) and that an already-off screen is not woken.
- Resolved on-device: `pm hide` does **not** remove the app from Settings → Apps (it
  stays listed as "Disabled" and re-enablable), so lockem now always uses global
  `pm disable`, which removes it. Still verify real recents-card removal and Private
  Space `am stop-user`.
- `pm disable` passes from the root daemon; the `disable-user` fallback is only hit when
  a build rejects the global disable.
- Boot-escape timing: which keys the firmware grabs on early boot, and how long the
  startup window needs to be to reliably catch a held Volume-Up.
- Which combos the system intercepts (screenshot / power-menu / SOS), to confirm the
  WebUI warnings.
