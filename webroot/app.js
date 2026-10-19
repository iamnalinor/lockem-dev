"use strict";

/* lockem WebUI. Runs inside a root manager's WebUI (KsuWebUI / KernelSU / MMRL),
   which injects `ksu.exec` for synchronous-ish root shell. All device work goes
   through sh(); the daemon owns the buttons (we never touch evdev from here).

   NOTE: a plain root WebUI exposes only `pm` package NAMES — no human-readable
   labels or icons (those need a PackageManager call, which `ksu.exec` can't make).
   So the app list shows package names + an initial placeholder. Labels/icons are a
   later enhancement (bundled aapt2 or an MMRL-native API). */

const DIR = "/data/adb/lockem";
const CFG = DIR + "/config";
const PKG = DIR + "/packages.list";
const CTL = DIR + "/ctl";
const RESULT = DIR + "/record_result";
const REQUEST = CTL + "/request";
const LOGFILE = DIR + "/lockem.log";
const DAEMON = "/data/adb/modules/lockem/bin/lockemd";

const KEY_LABEL = {
  KEY_VOLUMEUP: "Vol+",
  KEY_VOLUMEDOWN: "Vol-",
  KEY_POWER: "Power",
};

/* Root managers + WebUI hosts + known Magisk forks. Shown in a separate
   "Magisk-related" group with an explicit confirm before they can be locked
   (hiding these is a deliberate anti-detection choice, not the daemon's default). */
const MAGISK_PKGS = new Set([
  "com.topjohnwu.magisk",        // Magisk
  "io.github.vvb2060.magisk",    // Magisk Alpha
  "io.github.huskydg.magisk",    // KitsuneMagisk / Delta
  "me.weishu.kernelsu",          // KernelSU
  "me.bmax.apatch",              // APatch
  "io.github.a13e300.ksuwebui",  // KsuWebUI host
  "com.dergoogler.mmrl",         // MMRL
  "com.dergoogler.mmrl.wx",      // WebUI X
]);

/* ---- root shell bridge (autodetect) ------------------------------------- */
let cbSeq = 0;
function sh(cmd) {
  // KernelSU / KsuWebUI (standalone) and MMRL all inject a global `ksu.exec`
  // that invokes window[callbackName](errno, stdout, stderr).
  if (typeof ksu !== "undefined" && ksu && typeof ksu.exec === "function") {
    return new Promise((resolve) => {
      const cb = "__lockem_cb_" + cbSeq++;
      window[cb] = (errno, stdout, stderr) => {
        delete window[cb];
        resolve({ errno: Number(errno) || 0, stdout: stdout || "", stderr: stderr || "" });
      };
      try {
        ksu.exec(cmd, "{}", cb);
      } catch (e) {
        delete window[cb];
        resolve({ errno: -1, stdout: "", stderr: String(e) });
      }
    });
  }
  // No bridge: opened in a plain browser (preview only).
  return Promise.resolve({ errno: -1, stdout: "", stderr: "no root WebUI bridge" });
}

let HAS_BRIDGE = typeof ksu !== "undefined" && ksu && typeof ksu.exec === "function";

/* ---- small helpers ------------------------------------------------------ */
const $ = (sel) => document.querySelector(sel);
const $$ = (sel) => Array.from(document.querySelectorAll(sel));

function b64(str) {
  return btoa(unescape(encodeURIComponent(str)));
}
function shq(s) {
  return "'" + String(s).replace(/'/g, "'\\''") + "'";
}
async function writeFile(path, content) {
  // base64 payload is [A-Za-z0-9+/=] only, safe to pass unquoted.
  // Write to a temp file and rename, so a crash mid-write can't truncate the
  // real file (a half-written packages.list would silently drop apps on unlock).
  const tmp = path + ".tmp";
  const r = await sh(
    "echo " + b64(content) + " | base64 -d > " + shq(tmp) + " && mv -f " + shq(tmp) + " " + shq(path)
  );
  return r.errno === 0;
}
let toastTimer = null;
function toast(msg) {
  const t = $("#toast");
  t.textContent = msg;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.hidden = true), 2600);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* ---- unsaved-changes tracking ------------------------------------------- */
let dirty = false;
function markDirty() {
  if (dirty) return;
  dirty = true;
  const b = $("#save");
  if (b) b.disabled = false;
  const u = $("#unsaved");
  if (u) u.hidden = false;
  const s = $("#savestatus");
  if (s) s.textContent = "";
}
function markClean() {
  dirty = false;
  const b = $("#save");
  if (b) b.disabled = true;
  const u = $("#unsaved");
  if (u) u.hidden = true;
}

/* ---- recipe model ------------------------------------------------------- */
// a recipe is an array of { type, key }
function parseRecipe(str) {
  const out = [];
  if (!str) return out;
  for (const part of str.split(",")) {
    const [type, key] = part.split(":").map((s) => s.trim());
    if (!type || !key) continue;
    if (!["click", "hold", "press", "unpress"].includes(type)) continue;
    if (!KEY_LABEL[key]) continue;
    out.push({ type, key });
  }
  return out;
}
function recipeToString(r) {
  return r.map((t) => t.type + ":" + t.key).join(",");
}

const state = {
  lock: [],
  unlock: [],
  pkgs: [], // {name, checked, system, magisk}
};

function renderRecipe(kind) {
  const el = $("#recipe-" + kind);
  const r = state[kind];
  if (!r.length) {
    el.textContent = "—";
    return;
  }
  el.innerHTML = "";
  r.forEach((t, i) => {
    const span = document.createElement("span");
    span.className = "tok";
    span.textContent = (KEY_LABEL[t.key] || t.key) + " " + t.type;
    const x = document.createElement("b");
    x.textContent = "×";
    x.title = "remove";
    x.addEventListener("click", () => {
      state[kind].splice(i, 1);
      renderRecipe(kind);
      markDirty();
    });
    span.appendChild(x);
    el.appendChild(span);
  });
}

/* ---- config load / form sync ------------------------------------------- */
const BOOL_OPTS = [
  "strict_disable", "opt_vibrate", "opt_private_space", "opt_lockdown",
  "opt_disable_devopts", "opt_autolock_screenoff", "opt_autolock_timeout",
];
const NUM_OPTS = ["autolock_timeout_min", "hold_ms", "escape_window_sec"];

async function loadConfig() {
  const r = await sh("cat " + shq(CFG) + " 2>/dev/null");
  const cfg = {};
  r.stdout.split("\n").forEach((line) => {
    line = line.trim();
    if (!line || line[0] === "#") return;
    const i = line.indexOf("=");
    if (i < 0) return;
    cfg[line.slice(0, i).trim()] = line.slice(i + 1).trim();
  });
  BOOL_OPTS.forEach((k) => {
    const el = $("#" + k);
    if (el) el.checked = cfg[k] === "1";
  });
  NUM_OPTS.forEach((k) => {
    const el = $("#" + k);
    if (el && cfg[k] != null && cfg[k] !== "") el.value = cfg[k];
  });
  if (cfg.escape_key && $("#escape_key")) $("#escape_key").value = cfg.escape_key;
  state.lock = parseRecipe(cfg.lock_pattern);
  state.unlock = parseRecipe(cfg.unlock_pattern);
  renderRecipe("lock");
  renderRecipe("unlock");
}

function buildConfig() {
  const lines = [];
  lines.push("# lockem configuration (written by WebUI)");
  BOOL_OPTS.forEach((k) => lines.push(k + "=" + ($("#" + k).checked ? "1" : "0")));
  NUM_OPTS.forEach((k) => {
    let v = parseInt($("#" + k).value, 10);
    if (!Number.isFinite(v)) v = 0;
    lines.push(k + "=" + v);
  });
  lines.push("escape_key=" + $("#escape_key").value);
  lines.push("lock_pattern=" + recipeToString(state.lock));
  lines.push("unlock_pattern=" + recipeToString(state.unlock));
  return lines.join("\n") + "\n";
}

/* ---- packages ----------------------------------------------------------- */
let showSystem = false;

// Resolve the (possibly repackaged/hidden) Magisk manager package name so we can
// group it under Magisk-related even when it has a random id.
async function magiskHiddenPkg() {
  const r = await sh(
    "magisk --sqlite \"SELECT value FROM strings WHERE key='requester'\" 2>/dev/null"
  );
  const out = r.stdout.trim();
  if (!out) return null;
  const m = out.match(/value=([A-Za-z0-9._]+)/); // magisk prints "value=<pkg>"
  if (m) return m[1];
  return /^[A-Za-z0-9._]+$/.test(out) ? out : null;
}

async function loadPackages() {
  const box = $("#pkglist");
  box.textContent = "loading…";
  const [inst, selected, sys, hidden] = await Promise.all([
    sh("pm list packages -3 2>/dev/null | sed 's/^package://' | sort"),
    sh("cat " + shq(PKG) + " 2>/dev/null"),
    showSystem
      ? sh("pm list packages -s 2>/dev/null | sed 's/^package://' | sort")
      : Promise.resolve({ stdout: "" }),
    magiskHiddenPkg(),
  ]);

  const sel = new Set(
    selected.stdout.split("\n").map((s) => s.trim()).filter((s) => s && s[0] !== "#")
  );
  const magiskSet = new Set(MAGISK_PKGS);
  if (hidden) magiskSet.add(hidden);

  // name -> isSystem
  const all = new Map();
  inst.stdout.split("\n").map((s) => s.trim()).filter(Boolean).forEach((n) => all.set(n, false));
  sys.stdout.split("\n").map((s) => s.trim()).filter(Boolean).forEach((n) => {
    if (!all.has(n)) all.set(n, true);
  });
  // keep any selected package even if pm didn't list it (e.g. already hidden)
  sel.forEach((n) => {
    if (!all.has(n)) all.set(n, false);
  });
  if (hidden && !all.has(hidden)) all.set(hidden, false);

  const names = Array.from(all.keys()).sort();
  state.pkgs = names.map((name) => ({
    name,
    checked: sel.has(name),
    system: all.get(name),
    magisk: magiskSet.has(name),
  }));
  // selected-first; order is frozen until the next (re)load so toggling doesn't
  // make rows jump around under the user's finger.
  state.pkgs.sort(
    (a, b) => Number(b.checked) - Number(a.checked) || a.name.localeCompare(b.name)
  );
  renderPackages();
}

function pkgInitial(name) {
  const leaf = name.replace(/^.*\./, "");
  return (leaf[0] || name[0] || "?").toUpperCase();
}

function pkgRow(p) {
  const label = document.createElement("label");
  label.className = "pkg" + (p.system ? " sys" : "");
  const cb = document.createElement("input");
  cb.type = "checkbox";
  cb.checked = p.checked;
  cb.addEventListener("change", () => {
    if (cb.checked && p.magisk) {
      let ok = true;
      try {
        ok = confirm(
          'Lock Magisk-related app "' + p.name + '"?\n\n' +
            "Hiding your root manager / WebUI host is risky: you may lose the config UI " +
            "and new su prompts until you unlock."
        );
      } catch (e) {
        ok = true; // host without confirm() — the section warning already stands
      }
      if (!ok) {
        cb.checked = false;
        return;
      }
    }
    p.checked = cb.checked;
    markDirty();
  });
  const av = document.createElement("span");
  av.className = "avatar";
  av.textContent = pkgInitial(p.name);
  const nm = document.createElement("span");
  nm.className = "pkg-name";
  nm.textContent = p.name + (p.system ? "  (system)" : "");
  label.appendChild(cb);
  label.appendChild(av);
  label.appendChild(nm);
  return label;
}

function renderPackages() {
  const box = $("#pkglist");
  const filter = $("#pkg-filter").value.trim().toLowerCase();
  box.innerHTML = "";
  const magisk = [];
  const normal = [];
  state.pkgs.forEach((p) => {
    if (filter && !p.name.toLowerCase().includes(filter)) return;
    (p.magisk ? magisk : normal).push(p);
  });
  const shown = magisk.length + normal.length;

  if (magisk.length) {
    const h = document.createElement("div");
    h.className = "pkg-group";
    h.textContent = "Magisk-related — hiding these avoids a root tell, but is risky";
    box.appendChild(h);
    magisk.forEach((p) => box.appendChild(pkgRow(p)));
  }
  if (normal.length) {
    if (magisk.length) {
      const h = document.createElement("div");
      h.className = "pkg-group";
      h.textContent = "Apps";
      box.appendChild(h);
    }
    normal.forEach((p) => box.appendChild(pkgRow(p)));
  }
  if (!shown) box.textContent = state.pkgs.length ? "no matches" : "no apps found";
}

function selectedPackages() {
  return state.pkgs.filter((p) => p.checked).map((p) => p.name);
}

/* ---- recording (request/response handshake with the daemon) ------------- */
let recording = false;
async function record(kind) {
  if (recording) return;
  if (!HAS_BRIDGE) {
    toast("No root bridge — open from your root manager's WebUI.");
    return;
  }
  recording = true;
  const btn = $('.rec[data-kind="' + kind + '"]');
  const status = $("#recstatus-" + kind);
  const origLabel = btn.textContent;
  btn.setAttribute("aria-pressed", "true");
  btn.textContent = "Stop recording"; // cosmetic: shows a grab is in progress
  status.textContent = "press your buttons now…";

  // Unique request id so we only accept OUR response (no stale-file races).
  const id = "r" + Date.now() + "_" + Math.floor(Math.random() * 1e6);
  const action = kind === "lock" ? "record_lock" : "record_unlock";
  await sh("rm -f " + shq(RESULT));
  await sh("mkdir -p " + shq(CTL));
  const wrote = await writeFile(REQUEST, "id=" + id + "\naction=" + action + "\n");
  if (!wrote) {
    btn.setAttribute("aria-pressed", "false");
    btn.textContent = origLabel;
    recording = false;
    status.textContent = "couldn't start recording — write failed";
    return;
  }

  const field = kind === "lock" ? "lock_pattern" : "unlock_pattern";
  let resStatus = null;
  let pattern = "";
  for (let i = 0; i < 22; i++) {
    await sleep(1000);
    const r = await sh("cat " + shq(RESULT) + " 2>/dev/null");
    const idm = r.stdout.match(/^id=(.*)$/m);
    if (!idm || idm[1].trim() !== id) continue; // not our response yet
    const sm = r.stdout.match(/^status=(.*)$/m);
    resStatus = sm ? sm[1].trim() : "ok";
    const pm = r.stdout.match(new RegExp("^" + field + "=(.*)$", "m"));
    pattern = pm ? pm[1].trim() : "";
    break;
  }
  await sh("rm -f " + shq(RESULT));

  btn.setAttribute("aria-pressed", "false");
  btn.textContent = origLabel;
  recording = false;

  if (resStatus === null) {
    status.textContent = "timed out — try again";
    return;
  }
  if (resStatus === "empty") {
    // Nothing captured — keep the previous pattern, do NOT clobber state[kind].
    status.textContent = "nothing recorded — kept previous";
    return;
  }
  state[kind] = parseRecipe(pattern);
  renderRecipe(kind);
  markDirty();
  status.textContent = state[kind].length ? "captured" : "nothing recorded";
}

/* ---- manual builder ----------------------------------------------------- */
function wireBuilder(comboEl) {
  const kind = comboEl.dataset.kind;
  comboEl.querySelector(".b-add").addEventListener("click", () => {
    const type = comboEl.querySelector(".b-type").value;
    const key = comboEl.querySelector(".b-key").value;
    if (state[kind].length >= 32) {
      toast("recipe too long");
      return;
    }
    state[kind].push({ type, key });
    renderRecipe(kind);
    markDirty();
  });
  comboEl.querySelector(".b-clear").addEventListener("click", () => {
    state[kind] = [];
    renderRecipe(kind);
    markDirty();
  });
}

/* ---- log viewer --------------------------------------------------------- */
async function loadLog() {
  const el = $("#logview");
  if (!el) return;
  el.textContent = "loading…";
  const r = await sh("lockem log 200 2>/dev/null");
  el.textContent = r.stdout.trim() || "(log empty)";
}
async function clearLog() {
  if (!HAS_BRIDGE) {
    toast("No root bridge.");
    return;
  }
  await sh(": > " + shq(LOGFILE));
  loadLog();
}

/* ---- debug lock / unlock (reuse the CLI) -------------------------------- */
async function debugLock() {
  if (!HAS_BRIDGE) {
    toast("No root bridge.");
    return;
  }
  await sh("lockem lock");
  toast("Lock signalled.");
  refreshDaemon();
}
async function debugUnlock() {
  if (!HAS_BRIDGE) {
    toast("No root bridge.");
    return;
  }
  await sh("lockem unlock"); // self-contained; works even if the daemon is down
  toast("Unlocked all packages.");
}

/* ---- status + save ------------------------------------------------------ */
async function refreshDaemon() {
  const el = $("#daemon");
  const r = await sh("pgrep -f " + shq(DAEMON) + " >/dev/null 2>&1 && echo up || echo down");
  const up = r.stdout.trim() === "up";
  el.textContent = "daemon: " + (up ? "running" : "not running");
  el.className = "badge " + (up ? "ok" : "bad");
}

async function save() {
  if (!HAS_BRIDGE) {
    toast("No root bridge — cannot save.");
    return;
  }
  // Combos are the whole feature — never save an empty one (it would silently
  // disable locking/unlocking).
  if (!state.lock.length || !state.unlock.length) {
    toast("Both Lock and Unlock combos are required — record or build them first.");
    return;
  }
  const status = $("#savestatus");
  status.textContent = "saving…";

  const pkgText = selectedPackages().join("\n") + "\n";
  const okCfg = await writeFile(CFG, buildConfig());
  const okPkg = await writeFile(PKG, pkgText);
  if (!okCfg || !okPkg) {
    status.textContent = "save failed (permissions?)";
    toast("Save failed — check root permissions.");
    return;
  }
  await sh("mkdir -p " + shq(CTL) + " && : > " + shq(CTL + "/reload"));
  markClean();
  status.textContent = "saved · daemon reloaded";
  toast("Saved and applied.");
}

/* ---- init --------------------------------------------------------------- */
async function init() {
  $("#bridge-warn").hidden = HAS_BRIDGE;
  $$(".combo").forEach(wireBuilder);
  $$(".rec").forEach((b) => b.addEventListener("click", () => record(b.dataset.kind)));
  $("#refresh-pkgs").addEventListener("click", loadPackages);
  $("#pkg-filter").addEventListener("input", renderPackages);
  $("#save").addEventListener("click", save);

  const sysToggle = $("#show-system");
  if (sysToggle) sysToggle.addEventListener("change", (e) => {
    showSystem = e.target.checked;
    loadPackages();
  });

  // any option edit marks the form dirty (programmatic loadConfig doesn't fire these)
  BOOL_OPTS.concat(NUM_OPTS, ["escape_key"]).forEach((id) => {
    const el = $("#" + id);
    if (el) el.addEventListener("change", markDirty);
  });

  const logRefresh = $("#log-refresh");
  if (logRefresh) logRefresh.addEventListener("click", loadLog);
  const logClear = $("#log-clear");
  if (logClear) logClear.addEventListener("click", clearLog);
  const dbgLock = $("#dbg-lock");
  if (dbgLock) dbgLock.addEventListener("click", debugLock);
  const dbgUnlock = $("#dbg-unlock");
  if (dbgUnlock) dbgUnlock.addEventListener("click", debugUnlock);

  refreshDaemon();
  await loadConfig();
  await loadPackages();
  loadLog();
  markClean(); // freshly loaded state is clean
}

document.addEventListener("DOMContentLoaded", init);
