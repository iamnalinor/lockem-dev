"use strict";

/* lockem WebUI. Runs inside a root manager's WebUI (KsuWebUI / KernelSU / MMRL),
   which injects `ksu.exec` for synchronous-ish root shell. All device work goes
   through sh(); the daemon owns the buttons (we never touch evdev from here).

   App labels + icons come from a small helper (bin/labeldump.dex) run via
   app_process (the mechanism pm/am use) — a plain shell can't call PackageManager.
   Icons are rendered to a root-only cache and read back as base64 (the WebView,
   running as an app uid, cannot read /data/adb directly). If the helper is absent
   or fails, the list falls back to package names with no icons. */

const DIR = "/data/adb/lockem";
const CFG = DIR + "/config";
const PKG = DIR + "/packages.list";
const CTL = DIR + "/ctl";
const RESULT = DIR + "/record_result";
const REQUEST = CTL + "/request";
const LOGFILE = DIR + "/lockem.log";
const DAEMON = "/data/adb/modules/lockem/bin/lockemd";
const DEX = "/data/adb/modules/lockem/bin/labeldump.dex";
// Icons are rendered into the module's webroot so the host's (rooted) asset loader
// serves them straight to the WebView as a relative URL — no base64 over the bridge.
const ICON_WEBDIR = "/data/adb/modules/lockem/webroot/icons";
const ICON_URL = "icons/";

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
  pkgs: [], // {name, checked, system, magisk, label, icon}
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

function updateRootWarn() {
  const w = $("#cmd-root-warn");
  const sel = $("#cmd_user");
  if (w && sel) w.hidden = sel.value !== "root";
}

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
  if ($("#lock_cmd")) $("#lock_cmd").value = cfg.lock_cmd || "";
  if ($("#unlock_cmd")) $("#unlock_cmd").value = cfg.unlock_cmd || "";
  if (cfg.cmd_user && $("#cmd_user")) $("#cmd_user").value = cfg.cmd_user;
  if (cfg.cmd_timing && $("#cmd_timing")) $("#cmd_timing").value = cfg.cmd_timing;
  updateRootWarn();
  state.lock = parseRecipe(cfg.lock_pattern);
  state.unlock = parseRecipe(cfg.unlock_pattern);
  renderRecipe("lock");
  renderRecipe("unlock");
}

function buildConfig() {
  // config is a flat one-line-per-key file; keep custom commands single-line.
  const sanitize = (s) => (s || "").replace(/[\r\n]+/g, " ").trim();
  const lines = [];
  lines.push("# lockem configuration (written by WebUI)");
  BOOL_OPTS.forEach((k) => lines.push(k + "=" + ($("#" + k).checked ? "1" : "0")));
  NUM_OPTS.forEach((k) => {
    let v = parseInt($("#" + k).value, 10);
    if (!Number.isFinite(v)) v = 0;
    lines.push(k + "=" + v);
  });
  lines.push("escape_key=" + $("#escape_key").value);
  lines.push("lock_cmd=" + sanitize($("#lock_cmd") ? $("#lock_cmd").value : ""));
  lines.push("unlock_cmd=" + sanitize($("#unlock_cmd") ? $("#unlock_cmd").value : ""));
  lines.push("cmd_user=" + ($("#cmd_user") ? $("#cmd_user").value : "shell"));
  lines.push("cmd_timing=" + ($("#cmd_timing") ? $("#cmd_timing").value : "after"));
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

// Fetch real app labels for the given packages via the app_process helper, which
// also (re)renders their icons into the module's webroot (served to the WebView as
// a relative icons/<pkg>.png URL). Returns a {pkg: label} map; degrades to an empty
// map if the helper dex is missing or the bridge is unavailable (caller falls back
// to package names, and missing icons just hide via <img onerror>).
async function resolveLabels(names) {
  const labels = {};
  if (!HAS_BRIDGE || !names.length) return labels;

  const has = await sh("[ -f " + shq(DEX) + " ] && echo yes || echo no");
  if (has.stdout.trim() !== "yes") return labels;

  const args = names.map(shq).join(" ");
  // cwd = a writable root dir so app_process's dalvik-cache setup is happy
  const dump = await sh(
    "mkdir -p " + shq(ICON_WEBDIR) + "; cd " + shq(DIR) + " && CLASSPATH=" + shq(DEX) +
      " app_process /system/bin com.lockem.LabelDump " + shq(ICON_WEBDIR) + " " + args + " 2>/dev/null"
  );
  dump.stdout.split("\n").forEach((ln) => {
    const t = ln.indexOf("\t");
    if (t > 0) labels[ln.slice(0, t)] = ln.slice(t + 1).trim();
  });
  return labels;
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

  // real labels (best-effort; icons are rendered to webroot and loaded by URL below)
  const labels = await resolveLabels(names);

  state.pkgs = names.map((name) => ({
    name,
    checked: sel.has(name),
    system: all.get(name),
    magisk: magiskSet.has(name),
    label: labels[name] || "",
  }));
  // selected-first; order is frozen until the next (re)load so toggling doesn't
  // make rows jump around under the user's finger.
  state.pkgs.sort(
    (a, b) => Number(b.checked) - Number(a.checked) || sortName(a).localeCompare(sortName(b))
  );
  renderPackages();
}

function sortName(p) {
  return (p.label && p.label !== p.name ? p.label : p.name).toLowerCase();
}

function pkgRow(p) {
  const row = document.createElement("label");
  row.className = "pkg" + (p.system ? " sys" : "");

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

  // icon served from the module webroot (icons/<pkg>.png); hidden until it loads so
  // packages without an icon show no empty box.
  const icon = document.createElement("span");
  icon.className = "pkg-icon";
  icon.style.display = "none";
  const img = document.createElement("img");
  img.alt = "";
  img.loading = "lazy";
  img.onload = () => {
    icon.style.display = "";
  };
  img.src = ICON_URL + encodeURIComponent(p.name) + ".png";
  icon.appendChild(img);

  const text = document.createElement("span");
  text.className = "pkg-text";
  const hasLabel = p.label && p.label !== p.name;
  const main = document.createElement("span");
  main.className = "pkg-name";
  main.textContent = hasLabel ? p.label : p.name;
  text.appendChild(main);
  if (hasLabel) {
    const sub = document.createElement("span");
    sub.className = "pkg-sub";
    sub.textContent = p.name;
    text.appendChild(sub);
  }

  row.appendChild(cb);
  row.appendChild(icon);
  row.appendChild(text);
  return row;
}

function renderPackages() {
  const box = $("#pkglist");
  const filter = $("#pkg-filter").value.trim().toLowerCase();
  box.innerHTML = "";

  const groups = { apps: [], magisk: [], system: [] };
  state.pkgs.forEach((p) => {
    const hay = (p.name + " " + (p.label || "")).toLowerCase();
    if (filter && !hay.includes(filter)) return;
    if (p.magisk) groups.magisk.push(p);
    else if (p.system) groups.system.push(p);
    else groups.apps.push(p);
  });
  const total = groups.apps.length + groups.magisk.length + groups.system.length;

  const addBlock = (title, note, arr) => {
    if (!arr.length) return;
    const block = document.createElement("div");
    block.className = "pkg-block";
    const h = document.createElement("div");
    h.className = "pkg-block-h";
    h.textContent = title;
    block.appendChild(h);
    if (note) {
      const n = document.createElement("div");
      n.className = "pkg-note";
      n.textContent = note;
      block.appendChild(n);
    }
    const rows = document.createElement("div");
    rows.className = "pkg-rows";
    arr.forEach((p) => rows.appendChild(pkgRow(p)));
    block.appendChild(rows);
    box.appendChild(block);
  };

  // order: Apps, Magisk-related, System apps
  addBlock("Apps", "", groups.apps);
  addBlock("Magisk-related", "Hiding these avoids a root tell, but is risky.", groups.magisk);
  addBlock("System apps", "Disabling system apps can destabilize the device.", groups.system);

  if (!total) box.textContent = state.pkgs.length ? "no matches" : "no apps found";
}

function selectedPackages() {
  return state.pkgs.filter((p) => p.checked).map((p) => p.name);
}

// Hide the "Lock Private Space" option on devices with no Private Space profile.
async function hidePrivateSpaceIfAbsent() {
  const row = $("#row_private_space");
  if (!row) return;
  const r = await sh("pm list users 2>/dev/null");
  // use display (not the `hidden` attr): `label { display:block }` overrides [hidden]
  row.style.display = /[Pp]rivate/.test(r.stdout) ? "" : "none";
}

/* ---- recording (request/response handshake with the daemon) ------------- */
let recording = false;
let recordId = null;
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
  btn.textContent = "Stop recording";
  status.textContent = "press your buttons now…";

  // Unique request id so we only accept OUR response (no stale-file races).
  const id = "r" + Date.now() + "_" + Math.floor(Math.random() * 1e6);
  recordId = id;
  const action = kind === "lock" ? "record_lock" : "record_unlock";
  await sh("rm -f " + shq(RESULT));
  await sh("mkdir -p " + shq(CTL));
  const wrote = await writeFile(REQUEST, "id=" + id + "\naction=" + action + "\n");
  if (!wrote) {
    btn.setAttribute("aria-pressed", "false");
    btn.textContent = origLabel;
    recording = false;
    recordId = null;
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
  recordId = null;

  if (resStatus === null) {
    status.textContent = "timed out — try again";
    return;
  }
  if (resStatus === "cancelled") {
    status.textContent = "recording stopped — kept previous";
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

// Stop an in-progress recording: send a cancel request for the active id. The
// record() poll loop then sees status=cancelled and resets the UI.
async function stopRecord() {
  if (!recording || !recordId) return;
  await writeFile(REQUEST, "id=" + recordId + "\naction=cancel\n");
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
  status.textContent = ""; // the toast is the confirmation
  toast("Saved and applied.");
}

/* ---- init --------------------------------------------------------------- */
async function init() {
  $("#bridge-warn").hidden = HAS_BRIDGE;
  $$(".combo").forEach(wireBuilder);
  $$(".rec").forEach((b) =>
    b.addEventListener("click", () => {
      if (recording) stopRecord();
      else record(b.dataset.kind);
    })
  );
  $("#refresh-pkgs").addEventListener("click", async () => {
    // explicit refresh also drops the icon cache so updated-app icons re-render
    if (HAS_BRIDGE) await sh("rm -rf " + shq(ICON_WEBDIR));
    loadPackages();
  });
  $("#pkg-filter").addEventListener("input", renderPackages);
  $("#save").addEventListener("click", save);

  const sysToggle = $("#show-system");
  if (sysToggle)
    sysToggle.addEventListener("change", (e) => {
      showSystem = e.target.checked;
      loadPackages();
    });

  // any option edit marks the form dirty (programmatic loadConfig doesn't fire these)
  BOOL_OPTS.concat(NUM_OPTS, ["escape_key", "lock_cmd", "unlock_cmd", "cmd_user", "cmd_timing"]).forEach(
    (id) => {
      const el = $("#" + id);
      if (el) el.addEventListener("change", markDirty);
    }
  );
  // text command inputs: also mark dirty on each keystroke
  ["lock_cmd", "unlock_cmd"].forEach((id) => {
    const el = $("#" + id);
    if (el) el.addEventListener("input", markDirty);
  });
  const cu = $("#cmd_user");
  if (cu) cu.addEventListener("change", updateRootWarn);

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
  await hidePrivateSpaceIfAbsent();
  await loadPackages();
  loadLog();
  markClean(); // freshly loaded state is clean
}

document.addEventListener("DOMContentLoaded", init);
