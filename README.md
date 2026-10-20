# lockem

A Magisk / KernelSU / APatch module that **hides and disables selected apps on a
hardware-button combo**, and restores them on an unlock combo. The daemon is
portable C that uses only stock `pm` / `cmd` / `am` and reads evdev generically, so
it is meant to run on any rooted Android device, across root managers.

> **Compatibility.** lockem targets rooted Android generally (arm64 and x86_64).
> So far it has been **tested on a Pixel 9a running Android 17** (physical device) and
> on a **rooted Android emulator (Google APIs, 16 KB pages)** for the functional logic —
> other devices and Android versions *should* work but are unverified. A few behaviors are
> inherently device-dependent (see [docs/RISKS.md](docs/RISKS.md) §3 and
> [docs/TESTING.md](docs/TESTING.md)): biometric lockdown, recents-card removal,
> Private Space, vibration path, and which keys the firmware grabs on early boot.
> Reports and PRs from other devices are welcome.

> **It's privacy, not a vault.** lockem hides apps from a glance or a
> grabbed-unlocked phone — not from forensics or someone technical with your device.

---

## What it protects against

lockem is about **privacy from a glance, not security from an attacker.** It helps when:

- someone picks up your unlocked phone and scrolls around,
- a person looks over your shoulder,
- you hand your phone over and don't want certain apps noticed in the app drawer,
  Settings, or recents.

It does **not** keep your apps safe from someone technical who has your device, or from
forensic tools. Your phone is rooted with an unlocked bootloader, and lockem stores your
combo and hidden-apps list in plain text — so anyone who can get deep enough into the
phone can simply undo the hiding or read the combo.

There's an honest trade-off built in: **every way you can get back in is also a way
someone else holding your phone could undo lockem.** You can't have both. lockem buys
everyday privacy, not a vault.

The full threat model and the recovery routes are in **[docs/RISKS.md](docs/RISKS.md)** —
read it before you rely on lockem.

## How it works

You pick which apps to hide and record two button combos — one to **lock** (hide the
apps) and one to **unlock** (bring them back). A small background service watches your
Power and Volume buttons and acts when it sees a combo:

- **Lock** disables each chosen app so it disappears from the launcher, from Settings,
  and from recents. Optional extras: vibrate, lock Private Space, force a PIN on the next
  unlock (lockdown), hide Developer Options, and run a **custom command** you supply.
- **Unlock** brings every chosen app back.

What keeps it safe to run:

- **It can't boot-loop your phone** — lock is never re-applied automatically at startup,
  so a bad state can't trap you out.
- **It only listens** to your buttons in normal use (it takes them over only while you're
  recording a combo), so it can't get stuck intercepting your presses; if the service
  crashes, the feature just stops until the next restart.
- **It refuses to hide core system apps** (Android system, Settings, your launcher,
  Phone, Google Play services).

> Under the hood: lock runs `pm disable` (falling back to `pm disable-user`) + `pm hide` +
> `am force-stop` per app; unlock runs `pm unhide` + `pm enable`. Hidden state lives in
> `package-restrictions.xml` and persists across reboots on its own. More detail is in
> [docs/RISKS.md](docs/RISKS.md) and [docs/TESTING.md](docs/TESTING.md).

### Combos are recipes

A combo is an **ordered recipe of tokens**:

- `click:KEY` — quick press-and-release
- `hold:KEY` — held past the threshold (`hold_ms`, default 500 ms)
- `press:KEY` … `unpress:KEY` — hold one key down while actuating others under it
  (for parallel presses; raises entropy)

Keys: `KEY_POWER`, `KEY_VOLUMEUP`, `KEY_VOLUMEDOWN`. Example:
`click:KEY_VOLUMEUP,click:KEY_VOLUMEUP,hold:KEY_VOLUMEDOWN`.

**Timing.** Enter the whole recipe in one go: each step must follow the previous within
~4 s — a longer idle gap resets the in-progress match and you start over (so pressing
one button now and the rest much later is *not* counted). `hold_ms` (default 500 ms) is
the click-vs-hold threshold. The 4 s reset window is a fixed constant.

⚠️ Avoid **Power + Volume-Down** — Android screenshots on it (lands in your gallery).

---

## Install

1. Build the module zip (see **Build**) or download a release.
2. Install the module zip (`lockem-<version>.zip`) through your root manager's app
   (Magisk / KernelSU / APatch).
3. Reboot.
4. Open the **WebUI**: tap the module's **Action** button in your root manager. Magisk
   and APatch don't render a module's WebUI themselves, so `action.sh` hands off to a
   standalone renderer (KsuWebUI / WebUI X), installing **KsuWebUI** if none is present.
   Then pick apps and record both combos.

The installer lays out `/data/adb/lockem/` (config, `packages.list`, `ctl/`), picks the
right-ABI daemon for your device, and never clobbers an existing config on upgrade.

**Updates.** `module.prop` ships an `updateJson`, so once a new `v*` release is tagged your
root manager shows an in-app **Update** for the module (bump `versionCode` each release).

## Verifying the download

Release builds carry a [SLSA build-provenance attestation](https://docs.github.com/actions/security-guides/using-artifact-attestations)
generated by the release workflow. After downloading `lockem-<version>.zip` from the
[Releases](https://github.com/iamnalinor/lockem/releases) page, confirm it was built
by this repo's CI (and not tampered with) using the GitHub CLI:

```bash
gh attestation verify lockem-<version>.zip --repo iamnalinor/lockem
```

This checks the zip's digest against the signed provenance (source repo, commit, and
workflow that produced it). Attestations are also listed at
<https://github.com/iamnalinor/lockem/attestations>. GitHub also shows each release
asset's SHA-256 digest in the Releases UI. Note this is **supply-chain provenance** — it
proves where the zip came from, not a cryptographic signature of the installer itself.

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

See `default-config` for every key and `docs/RISKS.md` for the risk of each.

## Recovery (forgot the combo?)

In order: unlock combo → **boot-combo escape** (hold Volume-Up while the daemon starts,
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
bash pack.sh     # -> dist/lockem-<version>.zip
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
