// WiFi setup page for the ESP8266. Served at /wifi. Look comes from /s.css.
#pragma once
const char WIFI_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>PSS-270 WiFi</title>
<link rel="stylesheet" href="/s.css">
<style>
body{max-width:560px}
input{width:100%;margin:3px 0}
input[type=checkbox]{width:auto}
.net{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:6px 0;border-bottom:1px solid #2a2923}
.net:last-child{border:0}
.scan{cursor:pointer;padding:6px 4px}.scan:hover{background:#26241f}
#msg{min-height:1.3em;margin-top:6px}
</style></head><body>
<h2>WiFi<a href="/">console</a><a href="/tone">tone editor</a></h2>

<div class="card"><h3>Connected now</h3><div id="now" class="dim">...</div></div>

<div class="card"><h3>Saved networks</h3>
<div id="saved" class="dim">...</div>
<div class="dim">Tried in this order. Your two original home networks stay built in as a fallback.</div></div>

<div class="card"><h3>Add a network</h3>
<button id="scan" class="alt">Scan for networks</button>
<div id="found"></div>
<input id="ssid" placeholder="Network name (SSID)" autocapitalize="none" autocorrect="off" spellcheck="false">
<input id="pass" type="password" placeholder="Password (empty for an open network)" autocomplete="off">
<label class="dim"><input id="show" type="checkbox"> show password</label><br>
<button id="save">Save</button> <button id="saveconn" class="go">Save and connect now</button>
<div id="msg"></div>
<div class="dim">If the board cannot join any network it starts its own WiFi named <b>PSS270-setup</b> (password <b>pss270setup</b>). Join it and open <b>192.168.4.1/wifi</b>.
After "connect now" the page may drop for a few seconds; reopen <b>pss270.local</b> if the address changed.</div></div>

<script>
const $ = id => document.getElementById(id);
const esc = s => s.replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;"}[c]));
function say(s, bad) { const m = $("msg"); m.textContent = s; m.className = bad ? "bad" : "ok"; }
async function post(url, body) {
  const r = await fetch(url, { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: new URLSearchParams(body) });
  return { ok: r.ok, text: await r.text() };
}
async function refresh() {
  try {
    const j = await (await fetch("/wifi/list")).json();
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
  } catch (e) { $("now").textContent = "no answer from the board (it may be switching network)"; }
}
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
refresh();
</script></body></html>)HTML";
