"use strict";

/* lockem WebUI. Runs inside a root manager's WebUI (KsuWebUI / KernelSU / MMRL),
   which injects `ksu.exec` for synchronous-ish root shell. All device work goes
   through sh(); the daemon owns the buttons (we never touch evdev from here). */

const DIR = "/data/adb/lockem";
const CFG = DIR + "/config";
const PKG = DIR + "/packages.list";
const CTL = DIR + "/ctl";
const RESULT = DIR + "/record_result";
const DAEMON = "/data/adb/modules/lockem/bin/lockemd";

const KEY_LABEL = {
  KEY_VOLUMEUP: "Vol+",
  KEY_VOLUMEDOWN: "Vol-",
  KEY_POWER: "Power",
};

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
  // base64 payload is [A-Za-z0-9+/=] only, safe to pass unquoted
  const r = await sh("echo " + b64(content) + " | base64 -d > " + shq(path));
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
  pkgs: [], // {name, checked}
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
async function loadPackages() {
  const box = $("#pkglist");
  box.textContent = "loading…";
  const [inst, selected] = await Promise.all([
    sh("pm list packages -3 2>/dev/null | sed 's/^package://' | sort"),
    sh("cat " + shq(PKG) + " 2>/dev/null"),
  ]);
  const sel = new Set(
    selected.stdout.split("\n").map((s) => s.trim()).filter((s) => s && s[0] !== "#")
  );
  const names = inst.stdout.split("\n").map((s) => s.trim()).filter(Boolean);
  // keep any selected package even if pm didn't list it (e.g. already hidden)
  sel.forEach((s) => {
    if (!names.includes(s)) names.push(s);
  });
  names.sort();
  state.pkgs = names.map((name) => ({ name, checked: sel.has(name) }));
  renderPackages();
}

function renderPackages() {
  const box = $("#pkglist");
  const filter = $("#pkg-filter").value.trim().toLowerCase();
  box.innerHTML = "";
  let shown = 0;
  state.pkgs.forEach((p) => {
    if (filter && !p.name.toLowerCase().includes(filter)) return;
    shown++;
    const label = document.createElement("label");
    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.checked = p.checked;
    cb.addEventListener("change", () => (p.checked = cb.checked));
    label.appendChild(cb);
    label.appendChild(document.createTextNode(" " + p.name));
    box.appendChild(label);
  });
  if (!shown) box.textContent = state.pkgs.length ? "no matches" : "no third-party apps found";
}

function selectedPackages() {
  return state.pkgs.filter((p) => p.checked).map((p) => p.name);
}

/* ---- recording ---------------------------------------------------------- */
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
  btn.setAttribute("aria-pressed", "true");
  status.textContent = "press your buttons now…";

  await sh("rm -f " + shq(RESULT));
  await sh("mkdir -p " + shq(CTL) + " && : > " + shq(CTL + "/record_" + kind));

  const field = kind === "lock" ? "lock_pattern" : "unlock_pattern";
  let captured = null;
  for (let i = 0; i < 22; i++) {
    await sleep(1000);
    const r = await sh("cat " + shq(RESULT) + " 2>/dev/null");
    const m = r.stdout.match(new RegExp("^" + field + "=(.*)$", "m"));
    if (m) {
      captured = m[1].trim();
      break;
    }
  }
  await sh("rm -f " + shq(RESULT));

  btn.setAttribute("aria-pressed", "false");
  recording = false;

  if (captured === null) {
    status.textContent = "timed out — try again";
    return;
  }
  state[kind] = parseRecipe(captured);
  renderRecipe(kind);
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
  });
  comboEl.querySelector(".b-clear").addEventListener("click", () => {
    state[kind] = [];
    renderRecipe(kind);
  });
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
  status.textContent = "saved · daemon reloaded";
  toast("Saved and applied.");
}

/* ---- init --------------------------------------------------------------- */
function init() {
  $("#bridge-warn").hidden = HAS_BRIDGE;
  $$(".combo").forEach(wireBuilder);
  $$(".rec").forEach((b) => b.addEventListener("click", () => record(b.dataset.kind)));
  $("#refresh-pkgs").addEventListener("click", loadPackages);
  $("#pkg-filter").addEventListener("input", renderPackages);
  $("#save").addEventListener("click", save);

  refreshDaemon();
  loadConfig();
  loadPackages();
}

document.addEventListener("DOMContentLoaded", init);
