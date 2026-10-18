# lockem

A Magisk / KernelSU / APatch module that **reversibly hides selected apps on a
hardware-button combo**, and restores them on a reverse combo. Built for a personal
Pixel 9a (Android 16); the daemon is portable C and uses only stock `pm` / `cmd` /
`am`, so it works across root managers.

> **lockem is concealment, not encryption.** It hides apps from a glance or a
> grabbed-unlocked phone — not from forensics or a knowledgeable adversary with your
> device. Read **[docs/RISKS.md](docs/RISKS.md)** before relying on it.

---

## How it works

A small native daemon (`lockemd`, started `late_start` by `service.sh`) reads the
power/volume evdev stream **passively** and watches for two combos:

- **lock combo** → for each selected package: `pm disable-user` (or strict
  `pm disable`), `pm hide`, `am force-stop` (the force-stop also clears the recents
  card). Optional extras: vibrate, lock Private Space, lockdown/lock screen, hide
  Developer Options.
- **unlock combo** → `pm unhide` + `pm enable` for every selected package, and
  restores Developer Options.

Hidden state lives in `package-restrictions.xml` and **persists across reboots on its
own** — `service.sh` never re-applies lock at boot (so it can't break booting). The
daemon only listens and performs the boot-combo escape check.

Exclusive input capture (`EVIOCGRAB`) is used **only** during combo recording, so the
phone doesn't react to your presses while you record. In everyday use the daemon never
holds a grab — it listens passively, so it can't "stick" on your buttons, and a daemon
crash is harmless (the feature just stops until restart).

### Combos are recipes

A combo is an **ordered recipe of tokens**:

- `click:KEY` — quick press-and-release
- `hold:KEY` — held past the threshold (`hold_ms`, default 500 ms)
- `press:KEY` … `unpress:KEY` — hold one key down while actuating others under it
  (for parallel presses; raises entropy)

Keys: `KEY_POWER`, `KEY_VOLUMEUP`, `KEY_VOLUMEDOWN`. Example:
`click:KEY_VOLUMEUP,click:KEY_VOLUMEUP,hold:KEY_VOLUMEDOWN`.

⚠️ Avoid **Power + Volume-Down** — Android screenshots on it (lands in your gallery).

---

## Install

1. Build the module zip (see **Build**) or download a release.
2. Install `lockem.zip` through your root manager's app (Magisk / KernelSU / APatch).
3. Reboot.
4. Open the module's **WebUI** (e.g. KsuWebUI), pick apps, and record both combos.

The installer lays out `/data/adb/lockem/` (config, `packages.list`, `ctl/`), picks the
right-ABI daemon for your device, and never clobbers an existing config on upgrade.

## Usage

### WebUI
Pick the apps to hide, record the lock/unlock combos (or build them by hand without
pressing anything), toggle options, then **Save & apply**.

### CLI (`lockem`, on PATH in any root shell)

```
lockem unlock                 # restore ALL packages — self-contained, no daemon needed
lockem lock                   # signal the daemon to lock now
lockem reload                 # re-read config
lockem status                 # daemon up? which packages?
lockem log [N]                # tail the daemon log (default 200 lines)
lockem record-lock-combo      # enter record mode (lock)
lockem record-unlock-combo    # enter record mode (unlock)
```

`lockem unlock` runs `pm unhide`/`pm enable` directly and does **not** need the daemon,
so it recovers from `adb su`, recovery, or a GSI.

## Options

| Option | Effect |
|---|---|
| `opt_vibrate` | Vibrate on lock/unlock |
| `opt_private_space` | Lock Private Space (`am stop-user`) on lock |
| `opt_lockdown` | Best-effort lockdown / lock screen on lock |
| `opt_disable_devopts` | Hide Developer Options on lock (**adb stays on** — recovery preserved) |
| `opt_autolock_screenoff` | Auto-lock when the screen turns off |
| `opt_autolock_timeout` | Auto-lock after N minutes of inactivity |
| `strict_disable` | Use global `pm disable` (riskier) instead of `pm disable-user` |

See `default-config` for every key and `docs/RISKS.md` for the risk of each.

## Recovery (forgot the combo?)

In order: reverse combo → **boot-combo escape** (hold Volume-Up while the daemon starts,
just after you unlock the lock screen on boot) → `adb shell su -c 'lockem unlock'` →
`lockem unlock` from any root shell → external root over `/data`. Full detail and the
threat model are in **[docs/RISKS.md](docs/RISKS.md)**.

Removing the module runs `uninstall.sh` (best-effort unhide+enable of all packages +
restore Developer Options). The reliable rollback is to run `lockem unlock` on a live
system **before** removing the module.

## Build

Needs an Android NDK (r26+). Point `ANDROID_NDK_HOME` at it (or set `ANDROID_SDK_ROOT`,
which has `ndk/<version>`), then:

```bash
bash build.sh    # -> bin/lockemd-arm64, bin/lockemd-x86_64
bash pack.sh     # -> dist/lockem.zip (+ .sha256)
```

Host unit tests (no device/NDK needed):

```bash
bash tests/run_tests.sh   # gcc + AddressSanitizer/UBSan
```

Testing on an emulator and on the phone is covered in **[docs/TESTING.md](docs/TESTING.md)**.

## Layout

```
module.prop customize.sh service.sh uninstall.sh sepolicy.rule default-config
bin/lockemd-{arm64,x86_64}      # built daemon (not committed)
system/bin/lockem              # CLI (POSIX sh)
webroot/                       # WebUI (index.html, app.js, style.css)
src/                           # daemon sources (C) + jni/ build files
tests/                         # host unit tests
build.sh pack.sh               # build + package
```

## License

MIT © iamnalinor. See [LICENSE](LICENSE).
