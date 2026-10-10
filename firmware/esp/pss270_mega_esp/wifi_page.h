// Settings pages for the ESP8266 (mode, networks, keyboard, arp, extras, system). Served at /wifi. Look comes from /s.css.
#pragma once
const char WIFI_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>PSS-270 Settings</title>
<link rel="stylesheet" href="/s.css">
<style>
body{max-width:640px}
input[type=text],input[type=password],input:not([type]){width:100%;margin:3px 0}
.net{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:6px 0;border-bottom:1px solid #2a2923}
.net:last-child{border:0}
.scan{cursor:pointer;padding:6px 4px}.scan:hover{background:#26241f}
#msg,#mmsg,#xmsg{min-height:1.3em;margin-top:6px}
.tabs{display:flex;flex-wrap:wrap;gap:6px;margin:10px 0}
.tab{flex:1;min-width:84px;background:#1c1b18;color:var(--cream,#ebe5d3);border:1px solid #3a382f}
.tab.on{background:#a8944a;color:#121211;border-color:#a8944a}
.pane{display:none}.pane.on{display:block}
.mode{display:block;cursor:pointer;border:1px solid #3a382f;border-radius:8px;padding:10px 12px;margin:8px 0}
.mode.on{border-color:#2f9a68;background:#16231c}
.mode b{display:block;font-size:1.05em}
.mode input{width:auto;margin-right:8px}
.sl{display:grid;grid-template-columns:1fr auto;gap:2px 10px;align-items:center;margin:9px 0 2px}
.sl label{font-size:.95em}.sl output{color:var(--cyan,#58d3c8);font-variant-numeric:tabular-nums}
.sl input[type=range]{grid-column:1/3;width:100%;margin:0}
.row2{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:8px 0}
select{background:#1c1b18;color:var(--cream,#ebe5d3);border:1px solid #3a382f;border-radius:6px;padding:7px;max-width:100%}
pre{background:#0d0d0c;border:1px solid #2a2923;border-radius:6px;padding:8px;white-space:pre-wrap;max-height:40vh;overflow:auto;font:12px monospace;color:var(--cyan,#58d3c8)}
body.embed{max-width:none;padding:4px}.embed h2,.embed .tabs{display:none}
details{margin-top:8px}summary{cursor:pointer;color:var(--cream,#ebe5d3)}
</style></head><body>
<h2>Synth<a href="/">console</a><a href="/tone">tone editor</a></h2>

<div class="tabs">
<button class="tab" data-t="ext">Extras</button><button class="tab" data-t="arp">Arp</button><button class="tab" data-t="kbd">Keyboard</button>
<button class="tab on" data-t="mode">WiFi mode</button><button class="tab" data-t="net">Networks</button><button class="tab" data-t="sys">System</button>
</div>

<div id="p_mode" class="pane on">
<div class="card"><h3>How the board is reached</h3>
<label class="mode" id="mWifi"><input type="radio" name="mode" value="wifi"><b>WiFi</b>
<span class="dim">Joins your router (home, Altibox...). Open it at the address on the OLED, or <b>pss270.local</b>. If no network is in range it opens the setup WiFi by itself.</span></label>
<label class="mode" id="mLocal"><input type="radio" name="mode" value="local"><b>Local</b>
<span class="dim">The board makes its own WiFi <b>PSS270-setup</b> (password <b>pss270setup</b>) and never looks for a router. Join it with your phone and open <b>192.168.4.1</b>. Works anywhere, no router.</span></label>
<button id="apply" class="go">Apply</button>
<div id="mmsg"></div>
<div class="dim">The choice is saved and survives power-off. It is also in the keyboard menu (WiFi mode). Switching drops this page: in Local mode join <b>PSS270-setup</b> first, in WiFi mode reopen the board's address.</div></div>
<div class="card"><h3>Right now</h3><div id="now2" class="dim">...</div></div>
</div>

<div id="p_net" class="pane">
<div class="card"><h3>Connected now</h3><div id="now" class="dim">...</div></div>
<div class="card"><h3>Saved networks</h3>
<div id="saved" class="dim">...</div>
<div class="dim">Tried in this order. Your original home networks stay built in as a fallback.</div></div>
<div class="card"><h3>Add a network</h3>
<button id="scan" class="alt">Scan for networks</button>
<div id="found"></div>
<input id="ssid" placeholder="Network name (SSID)" autocapitalize="none" autocorrect="off" spellcheck="false">
<input id="pass" type="password" placeholder="Password (empty for an open network)" autocomplete="off">
<label class="dim"><input id="show" type="checkbox"> show password</label><br>
<button id="save">Save</button> <button id="saveconn" class="go">Save and connect now</button>
<div id="msg"></div>
<div class="dim">If the board cannot join any network it starts its own WiFi named <b>PSS270-setup</b> (password <b>pss270setup</b>). Join it and open <b>192.168.4.1/wifi</b>.</div></div>
</div>

<div id="p_kbd" class="pane">
<div class="card"><h3>Keyboard &amp; MIDI</h3><div id="kbd" class="dim">loading...</div></div>
</div>

<div id="p_arp" class="pane">
<div class="card"><h3>Arpeggiator</h3><div id="arp" class="dim">loading...</div></div>
<div id="arpx"></div>
</div>

<div id="p_ext" class="pane">
<div class="card"><div class="row2"><button id="xsave" class="go">Save all</button><button id="xdef" class="alt">Reset all</button><button id="xreload" class="alt">Reload</button></div>
<div id="xmsg" class="dim">Changes are heard at once but only kept after <b>Save all</b>.</div></div>
<div id="extras"></div>
</div>

<div id="p_sys" class="pane">
<div class="card"><h3>Keyboard</h3><div class="row2">
<button class="go" data-c="park">Park (Mega synth)</button><button data-c="unpark">Unpark (stock PSS)</button>
<button class="alt" data-c="learn">Learn keys</button><button class="alt" data-c="done">Done</button><button class="alt" data-c="test">Test chord</button></div></div>
<div class="card"><h3>Board health</h3><pre id="sys">...</pre>
<div class="row2"><button id="sysr" class="alt">Refresh</button><button id="bootlog" class="alt">Show boot log</button><button id="reboot" class="alt">Reboot WiFi chip</button></div>
<pre id="blog" style="display:none"></pre></div>
</div>

<script>
const $ = id => document.getElementById(id);
const esc = s => s.replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;"}[c]));
async function cmd(c) { const r = await fetch("/cmd?c=" + encodeURIComponent(c)); return await r.text(); }
function fire(c) { return fetch("/fire?c=" + encodeURIComponent(c)); }
function say(s, bad) { const m = $("msg"); m.textContent = s; m.className = bad ? "bad" : "ok"; }
function msay(s, bad) { const m = $("mmsg"); m.textContent = s; m.className = bad ? "bad" : "ok"; }
async function post(url, body) {
  const r = await fetch(url, { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: new URLSearchParams(body) });
  return { ok: r.ok, text: await r.text() };
}

/* ---------- tabs ---------- */
const loaded = {};
function tab(n) {
  document.querySelectorAll(".pane").forEach(p => p.className = "pane" + (p.id == "p_" + n ? " on" : ""));
  document.querySelectorAll(".tab").forEach(t => t.className = "tab" + (t.dataset.t == n ? " on" : ""));
  try { history.replaceState(null, "", "#" + n); } catch (e) {}
  if (!loaded[n]) { loaded[n] = true; if (n == "kbd") loadKbd(); if (n == "arp") loadArp(); if (n == "ext") loadExtras(); if (n == "sys") loadSys(); }
}
document.querySelectorAll(".tab").forEach(t => t.onclick = () => tab(t.dataset.t));

/* ---------- mode + networks ---------- */
function markMode(local) {
  $("mLocal").className = "mode" + (local ? " on" : "");
  $("mWifi").className = "mode" + (local ? "" : " on");
}
document.querySelectorAll("input[name=mode]").forEach(r => r.onchange = () => markMode(r.value == "local"));
let shownMode = false;
async function refresh() {
  try {
    const j = await (await fetch("/wifi/list")).json();
    if (!shownMode) {
      shownMode = true;
      document.querySelector("input[name=mode][value=" + (j.local ? "local" : "wifi") + "]").checked = true;
      markMode(j.local);
    }
    $("now2").innerHTML = j.local
      ? "Local mode: own WiFi <b>PSS270-setup</b> is on &middot; " + j.clients + " device(s) joined &middot; address 192.168.4.1"
      : (j.connected
        ? "WiFi mode: <b>" + esc(j.ssid) + "</b> &middot; " + j.ip + " &middot; signal " + j.rssi + " dBm"
        : "WiFi mode: <span class=\"bad\">not connected</span>" + (j.ap ? " &middot; setup network <b>PSS270-setup</b> is on (192.168.4.1)" : ""));
    $("now").innerHTML = j.connected
      ? "<b>" + esc(j.ssid) + "</b> &middot; " + j.ip + " &middot; signal " + j.rssi + " dBm" + (j.ap ? "<br>setup network <b>PSS270-setup</b> is also on" : "")
      : "<span class=\"bad\">not connected</span>" + (j.ap ? " &middot; setup network <b>PSS270-setup</b> is on" : "");
    $("saved").innerHTML = j.saved.length ? "" : "none saved yet";
    j.saved.forEach(s => {
      const d = document.createElement("div"); d.className = "net";
      d.innerHTML = "<span>" + esc(s) + "</span>";
      const b = document.createElement("button"); b.textContent = "Delete"; b.className = "alt";
      b.onclick = async () => { if (confirm("Delete " + s + "?")) { await post("/wifi/del", { ssid: s }); refresh(); } };
      d.appendChild(b); $("saved").appendChild(d);
    });
  } catch (e) { $("now").textContent = $("now2").textContent = "no answer from the board (it may be switching network)"; }
}
$("apply").onclick = async () => {
  const mode = document.querySelector("input[name=mode]:checked").value;
  if (mode == "local" && !confirm("Switch to Local mode?\nThis page will drop. Join the WiFi PSS270-setup (password pss270setup) and open 192.168.4.1")) return;
  const r = await post("/wifi/mode", { mode });
  msay(r.ok ? (mode == "local" ? "Local mode on. Join PSS270-setup, then open 192.168.4.1" : "WiFi mode on. Reconnecting to your router, look at the OLED for the address.") : r.text, !r.ok);
};
$("scan").onclick = async () => {
  $("found").innerHTML = "<span class=\"dim\">scanning...</span>";
  try {
    const l = await (await fetch("/wifi/scan")).json();
    $("found").innerHTML = l.length ? "" : "<span class=\"dim\">nothing found</span>";
    l.forEach(n => {
      const d = document.createElement("div"); d.className = "net scan";
      d.innerHTML = "<span>" + esc(n.s) + (n.e ? " &#128274;" : "") + "</span><span class=\"dim\">" + n.r + " dBm</span>";
      d.onclick = () => { $("ssid").value = n.s; $("pass").focus(); };
      $("found").appendChild(d);
    });
  } catch (e) { $("found").innerHTML = "<span class=\"bad\">scan failed</span>"; }
};
$("show").onchange = () => { $("pass").type = $("show").checked ? "text" : "password"; };
async function save(connect) {
  const ssid = $("ssid").value, pass = $("pass").value;
  if (!ssid || ssid.length > 32) return say("network name must be 1 to 32 characters", true);
  if (pass && (pass.length < 8 || pass.length > 63)) return say("password must be 8 to 63 characters", true);
  const r = await post("/wifi/add", { ssid, pass, connect: connect ? "1" : "0" });
  say(r.ok ? (connect ? "saved, connecting..." : "saved") : r.text, !r.ok);
  if (r.ok) { $("pass").value = ""; setTimeout(refresh, connect ? 6000 : 300); }
}
$("save").onclick = () => save(false);
$("saveconn").onclick = () => save(true);

/* ---------- little UI builders ---------- */
let timers = {};
function slider(host, o) {   // o: {label, min, max, step, value, mult, unit, onset}
  const id = "s" + Math.random().toString(36).slice(2, 8);
  const d = document.createElement("div"); d.className = "sl";
  const fmt = v => (o.mult && o.mult != 1 ? (v * o.mult).toFixed(o.mult < 1 ? 1 : 0) : v) + (o.unit ? " " + o.unit : "");
  d.innerHTML = "<label for=\"" + id + "\">" + o.label + "</label><output>" + fmt(o.value) + "</output><input id=\"" + id + "\" type=\"range\" min=\"" + o.min + "\" max=\"" + o.max + "\" step=\"" + (o.step || 1) + "\" value=\"" + o.value + "\">";
  const inp = d.querySelector("input"), out = d.querySelector("output");
  inp.oninput = () => { out.textContent = fmt(+inp.value); clearTimeout(timers[id]); timers[id] = setTimeout(() => o.onset(+inp.value), 70); };
  host.appendChild(d);
  return { set: v => { inp.value = v; out.textContent = fmt(+v); } };
}
function select(host, label, opts, value, onset) {
  const d = document.createElement("div"); d.className = "row2";
  d.innerHTML = "<span>" + label + "</span>";
  const s = document.createElement("select");
  opts.forEach((t, i) => { const o = document.createElement("option"); o.value = i; o.textContent = t; s.appendChild(o); });
  s.value = value; s.onchange = () => onset(+s.value);
  d.appendChild(s); host.appendChild(d);
  return s;
}
function check(host, label, value, onset) {
  const d = document.createElement("label"); d.className = "row2";
  const c = document.createElement("input"); c.type = "checkbox"; c.checked = !!value; c.style.width = "auto";
  c.onchange = () => onset(c.checked);
  d.appendChild(c); d.appendChild(document.createTextNode(" " + label)); host.appendChild(d);
  return c;
}
function kv(txt) { const o = {}; txt.split("\n").forEach(l => { const p = l.trim().split(/\s+/); if (p.length >= 2 && /^-?\d+$/.test(p[1])) o[p[0]] = +p[1]; }); return o; }

/* ---------- keyboard ---------- */
async function loadKbd() {
  const host = $("kbd");
  const c = kv(await cmd("cfg"));
  if (c.chan === undefined) { host.innerHTML = "<span class=\"bad\">No answer. Is the Mega on? This tab needs the newest Mega firmware.</span>"; loaded.kbd = false; return; }
  host.innerHTML = "";
  const names = (await cmd("names")).split("\n").filter(l => /^\d\d /.test(l)).map(l => l.trim());
  const opts = names.slice(); for (let i = 1; i <= 15; i++) opts.push("R" + i + "  YM2413 ROM voice " + i);
  select(host, "Voice", opts, c.prog, v => fire("prog " + v));
  slider(host, { label: "Transpose", min: -24, max: 24, value: c.trans, unit: "semitones", onset: v => fire("trans " + v) });
  select(host, "MIDI channel", Array.from({ length: 16 }, (_, i) => "" + (i + 1)), c.chan - 1, v => fire("chan " + (v + 1)));
  check(host, "MIDI thru", c.thru, v => fire("thru " + (v ? "on" : "off")));
  check(host, "Start as Mega synth when the PSS powers on", c.boot, v => fire("boot " + (v ? "mega" : "stock")));
  check(host, "Tone lock (keep the custom tone against the stock CPU)", c.lock, v => fire("lock " + (v ? "on" : "off")));
  check(host, "PSS power sense wire on A0", c.sense, v => fire("sense " + (v ? "on" : "off")));
  slider(host, { label: "Screen off after the PSS is switched off", min: 5, max: 250, value: c.standby, unit: "s", onset: v => fire("standby " + v) });
}

/* ---------- extras ---------- */
const LAB = {
  fon: "Fake filter", fcut: ["Cutoff (63 = open, 0 = dark)", 1, ""], fres: ["Resonance (feedback)", 1, ""], fea: ["Envelope amount", 1, ""],
  fa: ["Envelope attack", 10, "ms"], fd: ["Envelope decay", 10, "ms"], fkt: ["Key tracking", 1, "%"], fvel: ["Velocity opens it", 1, ""],
  dly: ["Delay before vibrato / tremolo", 10, "ms"], vib: ["Vibrato depth", 1, "cents"], vibr: ["Vibrato speed", 0.1, "Hz"],
  trem: ["Tremolo depth", 1, "steps"], tremr: ["Tremolo speed", 0.1, "Hz"],
  pen: ["Pitch envelope start (+ up, - down)", 1, "cents"], pent: ["Pitch envelope time", 10, "ms"], gl: ["Glide time", 10, "ms"],
  ea: ["Attack", 10, "ms"], ed: ["Decay", 10, "ms"], es: ["Sustain dip", 1, "steps"], er: ["Release", 10, "ms"], rel: "Release",
  uni: ["Detune (two voices per note)", 1, "cents"], int: ["Second voice interval (12 = octave)", 1, "semitones"],
  swt: "Sweep target", sws: "Sweep shape", swd: ["Sweep depth", 1, ""], swr: ["Sweep speed", 0.1, "Hz"],
  tun: "Tuning", tunroot: ["Tuning root note (0 = C)", 1, ""],
  hump: ["Pitch drift", 1, "cents"], humv: ["Velocity drift", 1, ""], humt: ["Timing drift (chords, arp)", 1, "ms"],
  chord: "Chord memory", strum: ["Strum spread", 1, "ms"],
  ch1: ["Chord note 2 above the key", 1, "semitones"], ch2: ["Chord note 3", 1, "semitones"], ch3: ["Chord note 4", 1, "semitones"], ch4: ["Chord note 5", 1, "semitones"], ch5: ["Chord note 6", 1, "semitones"],
  eun: ["Euclid steps (0 = plain arp)", 1, ""], euk: ["Euclid hits", 1, ""], eur: ["Euclid rotate", 1, ""], gate: ["Note length", 1, "%"], swing: ["Swing", 1, "%"]
};
for (let i = 0; i < 12; i++) LAB["t" + i] = ["Cents on " + ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"][i] + " (from the root)", 1, "cents"];
const SEL = {
  fon: ["off", "on"], rel: ["chip release", "long release"], swt: ["off", "modulator level (wah)", "feedback"], sws: ["sine", "triangle", "saw", "random"],
  tun: ["12-tone equal", "just", "pythagorean", "werckmeister", "maqam rast", "maqam bayati", "custom"], chord: ["off", "on"]
};
const GX = [
  ["Fake filter", ["fon", "fcut", "fres", "fea", "fa", "fd", "fkt", "fvel"]],
  ["Vibrato &amp; tremolo", ["dly", "vib", "vibr", "trem", "tremr"]],
  ["Pitch envelope &amp; glide", ["pen", "pent", "gl"]],
  ["Volume envelope (software)", ["ea", "ed", "es", "er", "rel"]],
  ["Thickness &amp; layers", ["uni", "int"]],
  ["Tone sweep (wobble)", ["swt", "sws", "swd", "swr"]],
  ["Tuning", ["tun", "tunroot"]],
  ["Humanize", ["hump", "humv", "humt"]],
  ["Chord memory", ["chord", "strum", "ch1", "ch2", "ch3", "ch4", "ch5"]]
];
const GA = [["Euclid arp &amp; feel", ["eun", "euk", "eur", "gate", "swing"]]];
let XV = null;
async function getX() {
  if (XV) return XV;
  const txt = await cmd("x list"), o = {};
  txt.split("\n").forEach(l => { const p = l.trim().split(/\s+/); if (p.length == 5 && /^-?\d+$/.test(p[1])) o[p[0]] = { v: +p[1], lo: +p[2], hi: +p[3], def: +p[4] }; });
  XV = Object.keys(o).length > 10 ? o : false;
  return XV;
}
function buildGroups(host, groups, X) {
  groups.forEach(g => {
    const card = document.createElement("div"); card.className = "card"; card.innerHTML = "<h3>" + g[0] + "</h3>";
    g[1].forEach(n => {
      const x = X[n]; if (!x) return;
      const L = LAB[n];
      if (SEL[n]) {
        const ctl = select(card, (typeof L == "string" ? L : n), SEL[n], x.v, v => { x.v = v; fire("x " + n + " " + v); if (n == "tun") reloadTun(); });
        if (n == "tun") tunSel = ctl;
      } else {
        slider(card, { label: L[0], min: x.lo, max: x.hi, value: x.v, mult: L[1], unit: L[2], onset: v => { x.v = v; fire("x " + n + " " + v); } });
      }
    });
    if (g[0] == "Chord memory") {
      const r = document.createElement("div"); r.className = "row2";
      r.innerHTML = "<button class=\"alt\" id=\"cl\">Learn chord</button><button class=\"alt\" id=\"cc\">Clear chord</button><span class=\"dim\">Learn: tap it, then hold a chord on the keys for a second.</span>";
      card.appendChild(r);
      r.querySelector("#cl").onclick = () => fire("chord learn");
      r.querySelector("#cc").onclick = () => { fire("chord clear"); setTimeout(() => { XV = null; loaded.ext = false; loadExtras(); }, 400); };
    }
    if (g[0] == "Tuning") {
      const dt = document.createElement("details"); dt.innerHTML = "<summary>Hand-tune each note (makes the tuning custom)</summary>";
      for (let i = 0; i < 12; i++) { const x = X["t" + i]; if (x) slider(dt, { label: LAB["t" + i][0], min: x.lo, max: x.hi, value: x.v, unit: "cents", onset: v => { x.v = v; fire("x t" + i + " " + v); if (tunSel) tunSel.value = 6; } }); }
      card.appendChild(dt);
    }
    host.appendChild(card);
  });
}
let tunSel = null;
function reloadTun() { setTimeout(() => { XV = null; loaded.ext = false; if ($("p_ext").classList.contains("on")) loadExtras(); }, 500); }
async function loadExtras() {
  const host = $("extras"); host.innerHTML = "<div class=\"card dim\">loading...</div>";
  const X = await getX();
  if (!X) { host.innerHTML = "<div class=\"card\"><span class=\"bad\">The Mega has no extras (old firmware, or it did not answer).</span> <span class=\"dim\">Flash the full Mega firmware, then press Reload.</span></div>"; loaded.ext = false; return; }
  host.innerHTML = "";
  buildGroups(host, GX, X);
}
$("xreload").onclick = () => { XV = null; loadExtras(); };
$("xsave").onclick = async () => { await fire("xsave"); $("xmsg").textContent = "Saved. These settings now survive power-off."; };
$("xdef").onclick = async () => { if (!confirm("Reset every extra to its default?")) return; await fire("xdef"); XV = null; setTimeout(() => { loadExtras(); loadedArp(); }, 500); $("xmsg").textContent = "Reset (not saved until you press Save all)."; };

/* ---------- arp ---------- */
const ARPN = ["off", "up", "down", "updown", "random", "played"];
function loadedArp() { loaded.arp = false; if ($("p_arp").classList.contains("on")) tab("arp"); }
async function loadArp() {
  const host = $("arp");
  const c = kv(await cmd("cfg"));
  if (c.arp === undefined) { host.innerHTML = "<span class=\"bad\">No answer from the Mega.</span>"; loaded.arp = false; return; }
  host.innerHTML = "";
  select(host, "Mode", ["off", "up", "down", "up and down", "random", "as played"], c.arp, v => fire("arp " + ARPN[v]));
  slider(host, { label: "Tempo", min: 40, max: 240, value: c.bpm, unit: "BPM", onset: v => fire("bpm " + v) });
  slider(host, { label: "Octaves", min: 1, max: 4, value: c.arpoct, onset: v => fire("arpoct " + v) });
  check(host, "Latch (keeps playing after you let go)", c.latch, v => fire("latch " + (v ? "on" : "off")));
  const X = await getX(), ax = $("arpx"); ax.innerHTML = "";
  if (X) buildGroups(ax, GA, X);
  else ax.innerHTML = "<div class=\"card dim\">Euclid, swing and humanize need the full Mega firmware.</div>";
}

/* ---------- system ---------- */
async function loadSys() {
  try { $("sys").textContent = await (await fetch("/sys")).text(); } catch (e) { $("sys").textContent = "no answer"; }
}
$("sysr").onclick = loadSys;
$("bootlog").onclick = async () => { const b = $("blog"); b.style.display = "block"; b.textContent = "..."; try { b.textContent = await (await fetch("/bootlog")).text(); b.scrollTop = b.scrollHeight; } catch (e) { b.textContent = "no answer"; } };
$("reboot").onclick = async () => { if (confirm("Restart the WiFi chip? The page drops for about 20 seconds.")) { try { await post("/reboot", {}); } catch (e) {} } };
document.querySelectorAll("[data-c]").forEach(b => b.onclick = () => fire(b.dataset.c));

refresh();
if (location.search.indexOf("embed") >= 0) {
  document.body.classList.add("embed");
  ["ext", "arp", "kbd"].forEach(n => { $("p_" + n).className = "pane on"; loaded[n] = true; });
  loadExtras(); loadArp(); loadKbd();
  const rep = () => parent.postMessage({ h: document.body.scrollHeight }, "*");
  new ResizeObserver(rep).observe(document.body); setInterval(rep, 1500);
} else
tab((location.hash || "#mode").slice(1) in { mode: 1, net: 1, kbd: 1, arp: 1, ext: 1, sys: 1 } ? (location.hash || "#mode").slice(1) : "mode");
</script></body></html>)HTML";
