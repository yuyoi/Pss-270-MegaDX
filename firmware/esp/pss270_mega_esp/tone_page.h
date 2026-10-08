// Live tone editor page for the YM2413 custom instrument (registers 0-7). Served at /tone.
// Look comes from /s.css. Each operator has a draggable envelope graph plus the sliders.
#pragma once
const char TONE_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>PSS-270 tone</title>
<link rel="stylesheet" href="/s.css">
<style>
.cols{display:flex;flex-wrap:wrap;gap:12px}
.col{flex:1 1 310px}
.f{display:flex;align-items:center;gap:8px;margin:5px 0;font-size:14px}
.f label{flex:0 0 150px}
.f input[type=range]{flex:1;min-width:80px}
.f .v{flex:0 0 38px;text-align:right;font:13px monospace}
canvas{width:100%;max-width:360px;display:block;border-radius:6px;border:1px solid var(--line);touch-action:none;margin-bottom:4px}
#msg{margin-left:8px}
#hex{font-family:monospace;width:270px}
</style></head><body>
<h2>Custom tone<a href="/">console</a><a href="/wifi">wifi</a></h2>
<div class="row">Load voice <select id="v"></select> <button id="ld" class="alt">Load</button> <button id="cur" class="alt">Read current</button></div>
<div class="row">Note <select id="n"></select> <button id="hold">Hold</button> <button id="play" class="go">Play 1 s</button> <button id="save" class="go">Save to Mega</button><span id="msg" class="ok"></span></div>
<div class="row">Bytes <input id="hex" type="text" spellcheck="false"> <button id="ap" class="alt">Apply</button></div>
<div class="cols">
<div class="col" id="mod"><h3>Modulator</h3><canvas id="em" width="300" height="120"></canvas><div class="dim">drag the gold dots: A attack, D decay and sustain level, R release</div></div>
<div class="col" id="car"><h3>Carrier</h3><canvas id="ec" width="300" height="120"></canvas><div class="dim">drag the gold dots: A attack, D decay and sustain level, R release</div></div>
</div>
<div class="cols" style="margin-top:12px"><div class="col" id="glob"><h3>Global</h3></div></div>
<script>
// field = [label, byte index, shift, bits]
const F = {
 mod: [["AM tremolo",0,7,1],["VIB vibrato",0,6,1],["EG sustained",0,5,1],["KSR key-scale rate",0,4,1],["MULT multiple",0,0,4],["KSL level scaling",2,6,2],["TL level (0 = loud)",2,0,6],["AR attack",4,4,4],["DR decay",4,0,4],["SL sustain level",6,4,4],["RR release",6,0,4]],
 car: [["AM tremolo",1,7,1],["VIB vibrato",1,6,1],["EG sustained",1,5,1],["KSR key-scale rate",1,4,1],["MULT multiple",1,0,4],["KSL level scaling",3,6,2],["AR attack",5,4,4],["DR decay",5,0,4],["SL sustain level",7,4,4],["RR release",7,0,4]],
 glob: [["Feedback",3,0,3],["Carrier rectify",3,4,1],["Modulator rectify",3,3,1]]
};
const MULT = ["0.5","1","2","3","4","5","6","7","8","9","10","10","12","12","15","15"];
let t = [0,0,0,0,0,0,0,0], held = false, timer = 0;
const $ = id => document.getElementById(id);
const get = f => (t[f[1]] >> f[2]) & ((1 << f[3]) - 1);
const put = (f, v) => { const m = ((1 << f[3]) - 1) << f[2]; t[f[1]] = (t[f[1]] & ~m) | ((v << f[2]) & m); };
const hex = () => t.map(x => x.toString(16).padStart(2, "0").toUpperCase()).join(" ");
const els = [];

// ---- envelope graphs ----
// [byte, shift, bits] for each envelope parameter of an operator
const ENV = {
 mod: { cv: "em", ar: [4,4,4], dr: [4,0,4], sl: [6,4,4], rr: [6,0,4], eg: [0,5,1] },
 car: { cv: "ec", ar: [5,4,4], dr: [5,0,4], sl: [7,4,4], rr: [7,0,4], eg: [1,5,1] }
};
const g3 = a => (t[a[0]] >> a[1]) & ((1 << a[2]) - 1);
const p3 = (a, v) => { const m = ((1 << a[2]) - 1) << a[1]; t[a[0]] = (t[a[0]] & ~m) | ((v << a[1]) & m); };
const W = 300, H = 120, X0 = 12, YB = H - 18, YT = 16, SPAN = 66, MINW = 4, HOLD = 44;
function geom(e) {
  const ar = g3(e.ar), dr = g3(e.dr), sl = g3(e.sl), rr = g3(e.rr), eg = g3(e.eg);
  const wA = MINW + SPAN * (1 - ar / 15), wD = MINW + SPAN * (1 - dr / 15), wR = MINW + SPAN * (1 - rr / 15);
  const A = { x: X0 + wA, y: YT };
  const yS = YT + (sl / 15) * (YB - YT);
  const D = { x: A.x + wD, y: yS };
  const frac = Math.max((YB - yS) / (YB - YT), 0.06);   // a release from a low level is shorter
  let K = null, R;
  if (eg) { K = { x: D.x + HOLD, y: yS }; R = { x: K.x + wR * frac, y: YB }; }   // sustained: holds until key-off
  else { R = { x: D.x + wR * frac, y: YB }; }                                      // percussive: keeps falling
  return { A, D, K, R, frac };
}
function drawEnv(e) {
  const c = $(e.cv).getContext("2d"), G = geom(e);
  c.fillStyle = "#0d0d0c"; c.fillRect(0, 0, W, H);
  c.strokeStyle = "#2a2923"; c.lineWidth = 1;
  for (let i = 0; i <= 4; i++) { const y = YT + (YB - YT) * i / 4; c.beginPath(); c.moveTo(X0, y); c.lineTo(W - 6, y); c.stroke(); }
  const path = () => { c.beginPath(); c.moveTo(X0, YB); c.lineTo(G.A.x, G.A.y); c.lineTo(G.D.x, G.D.y); if (G.K) c.lineTo(G.K.x, G.K.y); c.lineTo(G.R.x, G.R.y); };
  path(); c.lineTo(X0, YB); c.closePath(); c.fillStyle = "rgba(47,154,104,.30)"; c.fill();
  path(); c.strokeStyle = "#58d3c8"; c.lineWidth = 2; c.stroke();
  if (G.K) { c.setLineDash([4, 4]); c.strokeStyle = "#8b8fb3"; c.lineWidth = 1; c.beginPath(); c.moveTo(G.K.x, YT); c.lineTo(G.K.x, YB); c.stroke(); c.setLineDash([]);
    c.fillStyle = "#8b8fb3"; c.font = "10px sans-serif"; c.fillText("key off", G.K.x + 3, YT + 9); }
  [["A", G.A], ["D", G.D], ["R", G.R]].forEach(([n, p]) => {
    c.beginPath(); c.arc(p.x, p.y, 7, 0, 6.2832); c.fillStyle = "#c9b366"; c.fill();
    c.strokeStyle = "#17140a"; c.lineWidth = 1.5; c.stroke();
    c.fillStyle = "#17140a"; c.font = "bold 9px sans-serif"; c.textAlign = "center"; c.fillText(n, p.x, p.y + 3); c.textAlign = "left";
  });
  c.fillStyle = "#938d7a"; c.font = "10px monospace";
  c.fillText("AR " + g3(e.ar) + "  DR " + g3(e.dr) + "  SL " + g3(e.sl) + "  RR " + g3(e.rr) + (g3(e.eg) ? "" : "  percussive"), X0, H - 4);
}
function wireEnv(e) {
  const cv = $(e.cv); let drag = null;
  const pos = ev => { const r = cv.getBoundingClientRect(); return { x: (ev.clientX - r.left) * W / r.width, y: (ev.clientY - r.top) * H / r.height }; };
  cv.onpointerdown = ev => {
    const p = pos(ev), G = geom(e); let best = null, bd = 20 * 20;
    for (const k of ["A", "D", "R"]) { const d = (G[k].x - p.x) ** 2 + (G[k].y - p.y) ** 2; if (d < bd) { bd = d; best = k; } }
    if (best) { drag = best; cv.setPointerCapture(ev.pointerId); }
  };
  cv.onpointermove = ev => {
    if (!drag) return;
    const p = pos(ev), G = geom(e), clamp = (v, a, b) => Math.min(Math.max(v, a), b);
    const rate = w => Math.round(15 * (1 - (clamp(w, MINW, MINW + SPAN) - MINW) / SPAN));
    if (drag == "A") p3(e.ar, rate(p.x - X0));
    else if (drag == "D") { p3(e.dr, rate(p.x - G.A.x)); p3(e.sl, Math.round(15 * clamp((p.y - YT) / (YB - YT), 0, 1))); }
    else p3(e.rr, rate((p.x - (G.K ? G.K.x : G.D.x)) / G.frac));
    changed();
  };
  cv.onpointerup = cv.onpointercancel = () => { if (drag) { drag = null; send(); setTimeout(retrig, 120); } };
}

// ---- sliders ----
function build(box, list) {
  list.forEach(f => {
    const d = document.createElement("div"); d.className = "f";
    const l = document.createElement("label"); l.textContent = f[0]; d.appendChild(l);
    const i = document.createElement("input");
    const val = document.createElement("span"); val.className = "v";
    if (f[3] == 1) { i.type = "checkbox"; i.oninput = () => { put(f, i.checked ? 1 : 0); changed(); setTimeout(retrig, 120); }; }
    else { i.type = "range"; i.min = 0; i.max = (1 << f[3]) - 1; i.step = 1;
      i.oninput = () => { put(f, +i.value); changed(); }; i.onchange = () => { send(); setTimeout(retrig, 120); }; d.appendChild(i); }
    if (f[3] == 1) d.appendChild(i);
    d.appendChild(val); $(box).appendChild(d); els.push([f, i, val]);
  });
}
function show() {
  els.forEach(([f, i, val]) => {
    const v = get(f);
    if (f[3] == 1) i.checked = !!v; else i.value = v;
    val.textContent = f[0].startsWith("MULT") ? "x" + MULT[v] : (f[3] == 1 ? "" : v);
  });
  $("hex").value = hex();
  drawEnv(ENV.mod); drawEnv(ENV.car);
}
function send() { clearTimeout(timer); timer = 0; fetch("/fire?c=" + encodeURIComponent("tw " + hex())); }
function changed() { show(); if (!timer) timer = setTimeout(send, 40); }
function retrig() { if (held) fetch("/fire?c=hold%20" + $("n").value); }   // percussive tones fade while held: let go of a control and the note restarts
function say(s) { $("msg").textContent = s; setTimeout(() => { $("msg").textContent = ""; }, 2500); }
function parse(txt) {
  const m = /T((?: [0-9A-Fa-f]{2}){8})/.exec(txt);
  if (!m) { say("no tone in reply"); return false; }
  t = m[1].trim().split(" ").map(x => parseInt(x, 16)); show(); return true;
}
build("mod", F.mod); build("car", F.car); build("glob", F.glob);
wireEnv(ENV.mod); wireEnv(ENV.car);
for (let n = 36; n <= 84; n++) { const o = document.createElement("option"); o.value = n; o.textContent = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"][n % 12] + (Math.floor(n / 12) - 1) + " (" + n + ")"; if (n == 60) o.selected = true; $("n").appendChild(o); }
$("ld").onclick = async () => { const r = await fetch("/cmd?c=gettone%20" + $("v").value); if (parse(await r.text())) { send(); setTimeout(retrig, 120); say("loaded voice " + $("v").value); } };
$("cur").onclick = async () => { const r = await fetch("/cmd?c=gettone"); if (parse(await r.text())) say("read from the Mega"); };
$("ap").onclick = () => { const b = $("hex").value.trim().split(/\s+/); if (b.length == 8 && b.every(x => /^[0-9A-Fa-f]{1,2}$/.test(x))) { t = b.map(x => parseInt(x, 16)); show(); send(); } else say("need 8 hex bytes"); };
$("hold").onclick = () => { held = !held; $("hold").classList.toggle("on", held); $("hold").textContent = held ? "Release" : "Hold"; fetch("/fire?c=" + (held ? "hold%20" + $("n").value : "release")); };
$("n").onchange = () => { if (held) fetch("/fire?c=hold%20" + $("n").value); };
$("play").onclick = () => fetch("/fire?c=play%20" + $("n").value + "%201000");
$("save").onclick = () => { send(); setTimeout(() => { fetch("/fire?c=tonesave"); say("saved on the Mega"); }, 150); };
fetch("/cmd?c=names").then(r => r.text()).then(txt => {
  txt.split("\n").forEach(line => { const m = /^(\d\d) (.+)$/.exec(line.trim()); if (m) { const o = document.createElement("option"); o.value = +m[1]; o.textContent = m[1] + "  " + m[2]; $("v").appendChild(o); } });
  $("cur").onclick();
});
show();
</script></body></html>)HTML";
