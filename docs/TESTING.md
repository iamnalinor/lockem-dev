# lockem — testing guide

Two tracks: a rooted emulator (functional logic) and a real rooted device (things the
VM can't be trusted for). Plus host unit tests that need neither.

> lockem is meant to run on any rooted Android device. So far it has only been tested
> on a **Pixel 9a / Android 16**, which is the reference device named under Track B —
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

- **A Play Store (Google Play) image gives no root** — `adb root` is refused. For root
  you need a **Google APIs (not Play Store)** x86_64 image, rooted with **rootAVD**
  (<https://github.com/newbit1/rootAVD>), which patches the AVD ramdisk with Magisk.
- Many emulator images run **SELinux permissive** → AVC denials will **not** surface on
  the VM but **will** on a real enforcing phone. So the VM does not validate
  `sepolicy.rule` or real evdev access by the daemon.
- Vibration and real biometric lockdown have no physical effect on a VM.

### Suggested image

An x86_64 image close to the target: API 36/37, 16 KB page size (the `ps16k` variant),
**Google APIs** (so it can be rooted). The daemon's `-Wl,-z,max-page-size=16384` flag
(already in `Application.mk`) is required for 16 KB-page images.

### Setup

**1. Create a Google APIs x86_64 AVD** (needs the Android SDK cmdline-tools on PATH).
Pick an image close to the target; adjust the API level to taste:
```bash
sdkmanager "system-images;android-36;google_apis;x86_64"
avdmanager create avd -n lockem-test -k "system-images;android-36;google_apis;x86_64"
emulator -avd lockem-test        # first boot, so rootAVD can see the image
```

**2. Root it with rootAVD** (patches the AVD ramdisk with Magisk) —
<https://github.com/newbit1/rootAVD>:
```bash
git clone https://github.com/newbit1/rootAVD.git && cd rootAVD
./rootAVD.sh ListAllAVDs                                   # find your ramdisk path
./rootAVD.sh system-images/android-36/google_apis/x86_64/ramdisk.img
# it reboots the AVD; finish Magisk setup in the Magisk app (Install → Direct)
```
(On Windows use `rootAVD.bat` instead of `./rootAVD.sh`.)

**3. Build the x86_64 daemon and package** (on your build host, needs an NDK r26+):
```bash
export ANDROID_NDK_HOME="$ANDROID_SDK_ROOT/ndk/<version>"   # or set ANDROID_SDK_ROOT
bash build.sh && bash pack.sh                               # -> dist/lockem.zip
```

**4. Install the module and reboot.** Push the zip, then install it through the root
manager app (Magisk/KernelSU/APatch → Modules → Install from storage), and reboot:
```bash
adb push dist/lockem.zip /sdcard/Download/
# install dist/lockem.zip from the manager app UI, then:
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
Settings → Apps → See all on A16.

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

## 3. Real device (Track B) — enforcing hardware

Reference device tested: **Pixel 9a / Android 16** — substitute your own. Repeat the
smoke run, then close what the VM couldn't:

- **SELinux:** collect real denials — `dmesg | grep 'avc: denied'` — for reading
  `/dev/input/*` (and the `EVIOCGRAB` ioctl during recording). Extend `sepolicy.rule`
  with the exact types/classes from the denials (don't guess types).
- **Passive match on hardware buttons:** the daemon catches the combo from the live
  stream; incidental system reactions are tolerated. The temporary record grab really
  silences button reactions while recording.
- Physical vibration; real lockdown/biometric behavior; `pm hide` visibility in
  Settings → Apps on A16; real recents-card removal; Private Space `am stop-user`.
- `strict_disable`: does `pm disable` pass from root on the phone, or does it fall back
  to `disable-user`?
- Boot-escape timing: which keys the firmware grabs on early boot, and how long the
  startup window needs to be to reliably catch a held Volume-Up.
- Which combos the system intercepts (screenshot / power-menu / SOS), to confirm the
  WebUI warnings.
