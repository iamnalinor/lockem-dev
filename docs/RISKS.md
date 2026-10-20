# lockem — what it protects, what it risks, and how to get back in

Read this before you install lockem and start hiding apps. It's deliberately
blunt. lockem is a root module that hides apps behind a hardware-button combo —
this page is about what that actually protects you from, what it can't, the risks
of running it, and (most importantly) **how you get back in if you lock yourself
out.**

## 1. What lockem does — and what it can't

lockem is **concealment**, not a cryptographic lock. It's good against:

- someone casually poking around your unlocked phone,
- a shoulder-surfer,
- a quick glance through the app drawer, Settings, or recents.

It does **not** protect against someone technical who has your device, forensic
tools, or anyone who can get root over your data. That's not a bug we can fix — it
follows from how a rooted phone works:

- your bootloader is unlocked (that's what makes root possible in the first place),
- your combo and your hidden-apps list are stored **in plaintext** under
  `/data/adb/lockem/`,
- there's no encryption of that data at rest.

So anyone who can reach your data as root can just read the combo or undo the
hiding directly, and hidden apps can still be discovered with the right tools.

**The unavoidable trade-off:** every way *you* can recover is also a way someone
who holds your phone can undo lockem. You can't have both "I can always get back
in" and "an attacker can never undo it" while the combo is plaintext and the
bootloader is open. lockem buys privacy from everyday snooping, not security
against a determined attacker.

## 2. Risks of installing the module

- **It's a root module.** In general a misbehaving root module can break boot.
  lockem is built specifically to avoid that: it never blocks boot, it restarts
  its background service with backoff (and gives up if it keeps crashing so it
  can't burn your battery), and it refuses to hide core system apps (Android
  system, SystemUI, Settings, your launcher, Google Play services, Phone). The
  realistic risk of a bootloop is low — but you're still accepting the usual
  risks of a rooted, bootloader-unlocked phone.
- **Your combo and hidden list are plaintext.** See section 1. Don't treat lockem
  as a vault.
- **Lockdown on lock** (optional): forces the next unlock to require your
  PIN/pattern and blocks fingerprint/face until you enter it. That's the point of
  it — but it means you must know your PIN to get back in.
- **Custom command** (optional): a shell command *you* supply that runs on
  lock/unlock. It runs exactly as written, with no safety checks. If you set it to
  run as **root**, a bad command can **brick the device or break boot** — only use
  root if you know exactly what the command does. Prefer the safer `shell` option.
- **App hiding is aggressive.** lockem fully disables each hidden app so it
  disappears from Settings → Apps entirely (not just from the launcher); unlocking
  re-enables it. Don't add system or critical apps — lockem blocks the obvious
  ones and makes you explicitly confirm before selecting a system/Magisk app.
- **Hiding Developer Options keeps adb on.** lockem deliberately leaves adb
  enabled so the adb recovery route below still works.

## 3. If you hide Magisk + the WebUI — read this first

lockem's settings screen (where you'd normally turn it off or change the combo)
**lives inside your root manager's WebUI** (KsuWebUI / KernelSU / MMRL). You *can*
add the Magisk/KernelSU manager app and the WebUI host to lockem's hide list — but
understand what that costs you:

- Once they're hidden and locked, **there is no in-app off switch anymore.** You
  can't open lockem's settings to disable it, because the screen you'd use is
  itself hidden.
- The manager app being disabled also means a **new** root-permission prompt can't
  appear, so any app asking for root for the first time gets denied until you
  unlock. Root that was already granted (lockem itself, and adb if you
  pre-authorized it) keeps working.

So if you hide Magisk + the WebUI, the **only** ways back are the recovery routes
in section 4 — the unlock combo restores the manager and WebUI along with
everything else. Don't hide them unless you've got at least one recovery route you
trust set up and tested.

**Recommendation:** instead of hiding the manager app *through lockem*, use your
root manager's own built-in hide (Magisk's "repackage / random name"). That hides
the app without breaking your root grants and without trapping you out of the
settings. Treat hiding the manager/WebUI via lockem as an advanced, opt-in choice.

## 4. Getting back in (recovery), in order

Try these top to bottom — the first one is the normal path.

1. **Unlock combo** — the intended way. It restores **everything**, including the
   Magisk manager and WebUI if you hid them, so your settings come back.
2. **Boot-combo escape** — the built-in safety net; it's always on and can't be
   turned off. Reboot, unlock your lock screen with your PIN as usual, then during
   the first few seconds the daemon is starting up **hold the escape key** (default
   **Volume-Up**) — it unlocks everything. This doesn't clash with the
   bootloader/recovery key combos, which happen earlier from a powered-off state.
3. **Pre-authorized adb** — `adb shell su -c 'lockem unlock'`, if you granted root
   to adb ahead of time and adb is on.
4. **Any root shell** — `lockem unlock`. It's self-contained and works even if the
   background daemon is dead.
5. **External root over `/data`** — if the daemon won't even start, boot a
   root-capable image via `fastboot boot <img>` (a rooted GSI/recovery — **not**
   re-flashing the same patched boot image, which just reproduces the problem) and
   then run `lockem unlock`, read the plaintext combo, or delete the module. The
   exact image is device-specific.

### Turning the module off completely

- From adb or recovery: `touch /data/adb/modules/lockem/disable` (or delete the
  `/data/adb/modules/lockem` folder), then reboot.
- Or uninstall it from your root manager — on removal lockem un-hides and
  re-enables **every** app it touched and restores the settings it changed. The
  most reliable order is to run `lockem unlock` on a live phone **first**, then
  remove the module.
- Emergency stop without removing anything: `touch /data/adb/lockem/disable` halts
  the daemon (your hidden apps stay hidden until you unlock).

## 5. Before you experiment

- Back up your boot / `init_boot` image first — standard hygiene for a rooted
  phone. lockem never touches boot itself, but you want a way back regardless.
- Make sure at least one recovery route works for you **before** you hide the
  Magisk manager or the WebUI: know your lock-screen PIN (for the boot-combo
  escape), or pre-authorize adb root. Don't rely solely on a route you've never
  tested.
