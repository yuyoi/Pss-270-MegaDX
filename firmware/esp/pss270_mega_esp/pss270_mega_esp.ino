// PSS-270 brain, WiFi side: the ESP8266 on the WiFi Mega board.
// Joins the home WiFi and gives the Mega a remote console. Nothing here touches the PSS directly;
// every command goes to the Mega over the onboard serial link (ESP UART <-> Mega Serial3,
// DIP 1+2 ON, slide switch on RXD0/TXD0 so the ESP also reaches the Mega's bootloader).
//
//   http://pss270.local/  (or the IP)   console: buttons + command box + live log
//   http://pss270.local/tone            live tone editor with draggable envelopes
//   http://pss270.local/wifi            WiFi setup: scan, add and delete networks
//   GET /cmd?c=status                   send one console line, returns what the Mega printed
//   GET /fire?c=...                     same, but answers at once (sliders use it)
//   TCP 23                              raw console (e.g. `ncat pss270.local 23`)
//   TCP 2323                            avrdude flashes the Mega through the ESP
//   ArduinoOTA, hostname pss270         ESP updates over WiFi
//
// WiFi: networks saved from the /wifi page live in flash (LittleFS) and are tried first, newest
// first; the two networks compiled in below stay as a fallback. If nothing connects, the board
// starts its own network "PSS270-setup" (password pss270setup) and serves /wifi at 192.168.4.1.
//
// The ESP's own messages go out on Serial as '#' lines (boot log); the Mega ignores them.
//
// First flash over USB: DIP 5+6+7 ON (rest OFF), replug USB, upload with
//   arduino-cli upload -p COMxx --fqbn esp8266:esp8266:generic:eesz=4M1M,ResetMethod=ck,baud=115200
// then DIP back to 1+2 (+3+4 to keep USB on the Mega too).

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include "theme.h"
#include "tone_page.h"
#include "wifi_page.h"
extern "C" {
#include <user_interface.h>
}

// Brownout workaround for this board's weak 3.3V regulator: before the SDK's radio init,
// skip the full ~200 ms RF calibration (powerup option 2 = VDD33 + TX power only) and cap TX
// power during it. Then WiFi comes up late, gently, at reduced output power.
RF_PRE_INIT() {
  system_phy_set_powerup_option(2);
  system_phy_set_max_tpw(40);          // 0.25 dBm units, 40 = 10 dBm (max is 82)
}

// Boot-loop guard: the RTC memory counts boots that did not survive 20 s (brownouts). After two of
// them the next start is "gentle": radio held off longer, 5 dBm, no blocking join. Reset at 20 s up.
struct RtcBoot { uint32_t magic; uint32_t count; };
const uint32_t RTC_MAGIC = 0x50533237;
bool gentle = false;
void rtcBootCount() {
  RtcBoot r;
  if (!ESP.rtcUserMemoryRead(0, (uint32_t*)&r, sizeof r) || r.magic != RTC_MAGIC) { r.magic = RTC_MAGIC; r.count = 0; }
  r.count++;
  ESP.rtcUserMemoryWrite(0, (uint32_t*)&r, sizeof r);
  gentle = r.count >= 3;
}
void rtcBootOk() { RtcBoot r = {RTC_MAGIC, 0}; ESP.rtcUserMemoryWrite(0, (uint32_t*)&r, sizeof r); }

struct Net { const char* ssid; const char* pass; };
#if __has_include("secrets.h")
#include "secrets.h"                   // your own fallback networks, see secrets.example.h (never committed)
#else
const Net NETS[] = {{"", ""}};         // none built in: add networks on the /wifi page (or from PSS270-setup)
#endif
// built-in fallback networks, always tried after the ones saved from the /wifi page
const uint8_t NET_COUNT = NETS[0].ssid[0] ? sizeof(NETS) / sizeof(NETS[0]) : 0;

ESP8266WebServer server(80);
WiFiServer tcp(23);
WiFiServer flashSrv(2323);   // avrdude -c stk500v2 -P net:<ip>:2323 flashes the Mega
WiFiClient tcpClient;

// Ring of the Mega's recent output, for the web page's live log.
const size_t LOGSZ = 4096;
char logBuf[LOGSZ];
size_t logHead = 0;     // total bytes ever written (position = logHead % LOGSZ)

void logByte(char c) { logBuf[logHead % LOGSZ] = c; logHead++; }

void applyWifiMode(bool local);      // defined with the WiFi code below
void pumpMega() {
  static char lb[40]; static uint8_t ln = 0;     // lines starting @wifi come from the Mega's key menu
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { lb[ln] = 0; if (!strncmp(lb, "@wifi ", 6)) applyWifiMode(!strcmp(lb + 6, "local")); ln = 0; }
    else if (c != '\r' && ln < sizeof lb - 1) lb[ln++] = c;
    logByte(c);
    if (tcpClient && tcpClient.connected()) tcpClient.write((uint8_t)c);
  }
}

String logSince(size_t from) {
  if (logHead - from > LOGSZ) from = logHead - LOGSZ;
  String s;
  s.reserve(logHead - from);
  for (size_t i = from; i < logHead; i++) s += logBuf[i % LOGSZ];
  return s;
}

bool fsOk = false;
void flog(const char* msg) {           // one line in /boot.txt: "<seconds since boot> <text>"
  if (!fsOk) return;
  File f = LittleFS.open("/boot.txt", "a");
  if (!f) return;
  f.print(millis() / 1000); f.print(' '); f.println(msg);
  f.close();
}
void bootLog(const char* msg) {
  flog(msg);
  Serial.print("# "); Serial.println(msg);
  for (const char* p = "# "; *p; p++) logByte(*p);
  for (const char* p = msg; *p; p++) logByte(*p);
  logByte('\n');
}

// ---- saved WiFi networks (LittleFS /wifi.txt, one "ssid<TAB>password" per line, newest first) ----
const int MAXSAVED = 6;
String savedSsid[MAXSAVED], savedPass[MAXSAVED];
int nSaved = 0;
bool apActive = false, forceRetry = false;
uint32_t minHeap = 0xFFFFFFFF;          // lowest free heap seen since boot, shown at /sys

void loadSaved() {
  nSaved = 0;
  File f = LittleFS.open("/wifi.txt", "r");
  if (!f) return;
  while (f.available() && nSaved < MAXSAVED) {
    String line = f.readStringUntil('\n');
    line.replace("\r", "");
    int tab = line.indexOf('\t');
    if (tab < 1) continue;
    savedSsid[nSaved] = line.substring(0, tab);
    savedPass[nSaved] = line.substring(tab + 1);
    nSaved++;
  }
  f.close();
}
void saveSaved() {
  File f = LittleFS.open("/wifi.txt", "w");
  if (!f) return;
  for (int i = 0; i < nSaved; i++) { f.print(savedSsid[i]); f.print('\t'); f.print(savedPass[i]); f.print('\n'); }
  f.close();
}
void delSaved(const String& ssid) {
  for (int i = 0; i < nSaved; i++) if (savedSsid[i] == ssid) {
    for (int j = i; j + 1 < nSaved; j++) { savedSsid[j] = savedSsid[j + 1]; savedPass[j] = savedPass[j + 1]; }
    nSaved--;
    i--;
  }
}
void addSaved(const String& ssid, const String& pass) {   // newest goes first, so it is tried first
  delSaved(ssid);
  if (nSaved == MAXSAVED) nSaved--;
  for (int j = nSaved; j > 0; j--) { savedSsid[j] = savedSsid[j - 1]; savedPass[j] = savedPass[j - 1]; }
  savedSsid[0] = ssid; savedPass[0] = pass;
  nSaved++;
  saveSaved();
}
// Always tried last: a phone hotspot called "pss270" with password "pss270setup". Joining it only needs the
// radio as a client (low current), so it can set up a new WiFi without the setup network.
const char HOT_SSID[] = "pss270", HOT_PASS[] = "pss270setup";
int netCount() { return nSaved + NET_COUNT + 1; }
String netSsid(int i) { return i < nSaved ? savedSsid[i] : (i - nSaved < NET_COUNT ? String(NETS[i - nSaved].ssid) : String(HOT_SSID)); }
String netPass(int i) { return i < nSaved ? savedPass[i] : (i - nSaved < NET_COUNT ? String(NETS[i - nSaved].pass) : String(HOT_PASS)); }

bool localMode = false;                // Local: the board's own WiFi only, never looks for a router (saved in /mode.txt)
void loadMode() { File f = LittleFS.open("/mode.txt", "r"); if (f) { localMode = f.readString().startsWith("local"); f.close(); } }
void saveMode() { File f = LittleFS.open("/mode.txt", "w"); if (f) { f.print(localMode ? "local" : "wifi"); f.close(); } }
bool wifiTrying = false;               // wifiService is in the middle of a connection attempt
unsigned long wifiHoldUntil = 0;       // no new round before this (the setup network was just opened)

const char* wlWhy(int st) {            // what the failed attempt means, in plain words (shown in the log)
  switch (st) {
    case WL_NO_SSID_AVAIL: return "network not found (wrong name, or it is 5 GHz only)";
    case WL_WRONG_PASSWORD: return "wrong password";
    case WL_CONNECT_FAILED: return "refused (wrong password, or the router is WPA3-only / needs WPA2)";
    default: return "no answer";
  }
}

// The setup network runs ALONE (AP mode): AP+STA with the station side hunting for a missing router
// scanned channels all the time, which dropped the phone every few seconds and drew the most current.
void startAP() {
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP("PSS270-setup", "pss270setup", 1, 0, 3);     // channel 1, visible, 3 clients max
  WiFi.setOutputPower(6);                                  // the phone is next to the keyboard: 6 dBm is plenty
  softap_config cfg;
  if (wifi_softap_get_config(&cfg)) { cfg.beacon_interval = 400; wifi_softap_set_config(&cfg); }   // 2.5 bursts a second instead of 10
  apActive = true;
  wifiTrying = false;
  wifiHoldUntil = millis() + 120000;
  bootLog("no network: setup WiFi PSS270-setup is on (password pss270setup, then 192.168.4.1/wifi)");
}
void stopAP() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setOutputPower(gentle ? 5 : 10);
  apActive = false;
  bootLog("setup WiFi off");
}

// One scan per round, logged in full (names, channel, signal, security) so a router that hides or
// splits its bands shows up in /bootlog. The scan only ORDERS the known networks (the ones in range
// first): a network the scan missed is still tried afterwards, because a weak or band-steering router
// can miss a single scan.
int txDbm = 10;                        // grows by 3 dBm after each failed attempt, up to 16 (8 in gentle mode)
uint8_t order[16]; int nOrder = 0;     // known networks in the order they will be tried this round
bool seenNet[16];

void buildOrder() {
  int n = WiFi.scanNetworks();
  char m[96];
  snprintf(m, sizeof m, "scan: %d network(s)", n < 0 ? 0 : n); bootLog(m);
  for (int i = 0; i < n && i < 12; i++) {
    snprintf(m, sizeof m, "  %.20s ch%d %ddBm enc%d", WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i), (int)WiFi.encryptionType(i));
    bootLog(m);
  }
  nOrder = 0;
  int total = netCount() < 16 ? netCount() : 16;
  for (int pass = 0; pass < 2; pass++)
    for (int c = 0; c < total; c++) {
      bool seen = false;
      String s = netSsid(c);
      for (int i = 0; i < n; i++) if (WiFi.SSID(i) == s) seen = true;
      if (pass == 0 && seen) { order[nOrder] = c; seenNet[nOrder++] = true; }
      if (pass == 1 && !seen) { order[nOrder] = c; seenNet[nOrder++] = false; }
    }
  WiFi.scanDelete();
}
unsigned long tryMs(int p) { return seenNet[p] ? 12000UL : 8000UL; }
void failedAttempt(int p) {            // log why, and ask the radio for a little more power for the next one
  char m[110]; snprintf(m, sizeof m, "%s: %s", netSsid(order[p]).c_str(), wlWhy(WiFi.status())); bootLog(m);
  WiFi.disconnect();
  int cap = gentle ? 8 : 16;
  if (txDbm < cap) { txDbm = txDbm + 3 > cap ? cap : txDbm + 3; WiFi.setOutputPower(txDbm); snprintf(m, sizeof m, "tx power now %d dBm", txDbm); bootLog(m); }
}

bool joinWifi() {                      // blocking, used once at boot
  buildOrder();
  for (int p = 0; p < nOrder; p++) {
    String s = netSsid(order[p]);
    char m[96]; snprintf(m, sizeof m, "trying %s%s", s.c_str(), seenNet[p] ? "" : " (not in the scan)"); bootLog(m);
    WiFi.begin(s.c_str(), netPass(order[p]).c_str());
    unsigned long start = millis();
    while (millis() - start < tryMs(p)) {
      if (WiFi.status() == WL_CONNECTED) return true;
      delay(200);
    }
    failedAttempt(p);
  }
  return false;
}

// Keeps the connection alive without ever blocking the web server: a round scans once, then tries every
// known network (the ones in range first), 8-12 s each. If none works the setup network (AP only) stays up
// and the round repeats every minute, but never while somebody is connected to the setup network.
void wifiService() {
  if (localMode) return;
  static unsigned long nextTry = 0, tryStart = 0, upSince = 0;
  static int pos = -1;
  static bool wasUp = false, forceNow = false;
  unsigned long now = millis();
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != wasUp) {
    wasUp = up;
    char m[96];
    if (up) snprintf(m, sizeof m, "on %s as %s, rssi %d", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    else snprintf(m, sizeof m, "WiFi lost");
    bootLog(m);
  }
  if (up) {
    wifiTrying = false;
    if (!upSince) upSince = now;
    if (apActive && now - upSince > 60000 && WiFi.softAPgetStationNum() == 0) stopAP();   // online for a minute: the setup network is not needed
    return;
  }
  upSince = 0;
  if (forceRetry) { forceRetry = false; wifiTrying = false; pos = -1; nextTry = 0; forceNow = true; }
  if (!wifiTrying) {
    if ((long)(now - nextTry) < 0 || (!forceNow && (long)(now - wifiHoldUntil) < 0)) return;
    if (apActive) {
      if (!forceNow && WiFi.softAPgetStationNum() > 0) { nextTry = now + 30000; return; }   // somebody is using the setup network: leave it alone
      WiFi.mode(WIFI_AP_STA);
    }
    forceNow = false;
    if (pos < 0) buildOrder();
    if (pos + 1 >= nOrder) {           // round over, nothing worked
      pos = -1; nextTry = now + 60000;
      if (apActive) { WiFi.mode(WIFI_AP); WiFi.setOutputPower(6); } else startAP();
      return;
    }
    pos++;
    WiFi.setOutputPower(txDbm);
    WiFi.begin(netSsid(order[pos]).c_str(), netPass(order[pos]).c_str());
    wifiTrying = true; tryStart = now;
  } else if (now - tryStart > tryMs(pos)) {
    failedAttempt(pos);
    wifiTrying = false;
  }
}

// ---- web handlers ----
void handleCmd() {
  String c = server.arg("c");
  c.trim();
  if (!c.length() || c.length() > 60) { server.send(400, "text/plain", "bad command\n"); return; }
  if (c.startsWith("radiooff")) {          // radiooff <sec>: WiFi fully off for a while, then reboot (noise test)
    int sec = c.length() > 9 ? c.substring(9).toInt() : 30;
    if (sec < 5) sec = 5;
    if (sec > 300) sec = 300;
    server.send(200, "text/plain", "radio off for " + String(sec) + " s, then the ESP reboots\n");
    delay(100);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    unsigned long end = millis() + (unsigned long)sec * 1000UL;
    while (millis() < end) { pumpMega(); delay(5); }
    ESP.restart();
    return;
  }
  size_t mark = logHead;
  Serial.print(c); Serial.print('\n');
  unsigned long t = millis(), quiet = millis();
  size_t seen = logHead;
  while (millis() - t < 3000) {          // wait for the reply, done after 300 ms of silence
    pumpMega();
    if (logHead != seen) { seen = logHead; quiet = millis(); }
    if (logHead != mark && millis() - quiet > 300) break;
    delay(5);
  }
  server.send(200, "text/plain", logSince(mark));
}

void handleLog() {
  size_t from = server.hasArg("from") ? strtoul(server.arg("from").c_str(), nullptr, 10) : (logHead > LOGSZ ? logHead - LOGSZ : 0);
  String body = logSince(from);
  server.sendHeader("X-Log-Head", String((unsigned long)logHead));
  server.send(200, "text/plain", body);
}

// /fire?c=...: send one console line to the Mega and answer at once (no waiting for a reply).
// The tone editor's sliders use it so dragging never blocks the web server.
void handleFire() {
  String c = server.arg("c");
  c.trim();
  if (!c.length() || c.length() > 90) { server.send(400, "text/plain", "bad command"); return; }
  Serial.print(c);
  Serial.print('\n');
  server.send(200, "text/plain", "ok");
}

// /sys: health numbers, to catch the web-server freeze (heap running out is the main suspect)
void handleReboot() { server.send(200, "text/plain", "rebooting"); delay(200); ESP.restart(); }
void handleBootlog() {
  if (server.arg("clear") == "1") { LittleFS.remove("/boot.txt"); server.send(200, "text/plain", "cleared\n"); return; }
  File f = LittleFS.open("/boot.txt", "r");
  if (!f) { server.send(200, "text/plain", "(empty)\n"); return; }
  server.streamFile(f, "text/plain");
  f.close();
}
void handleSys() {
  char b[260];
  snprintf(b, sizeof b, "uptime_s=%lu\nheap=%u min_heap=%u max_block=%u fragmentation=%u%%\nrssi=%d wifi=%s reset=%s gentle=%d ap=%d local=%d\n",
           millis() / 1000, ESP.getFreeHeap(), (unsigned)minHeap, ESP.getMaxFreeBlockSize(), ESP.getHeapFragmentation(),
           WiFi.RSSI(), WiFi.status() == WL_CONNECTED ? "up" : "down", ESP.getResetReason().c_str(), gentle, apActive, localMode);
  server.send(200, "text/plain", b);
}

String jesc(const String& s) {
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

void handleWifiList() {
  String j = "{\"connected\":";
  j += WiFi.status() == WL_CONNECTED ? "true" : "false";
  j += ",\"ssid\":\"" + jesc(WiFi.SSID()) + "\",\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"ap\":"; j += apActive ? "true" : "false";
  j += ",\"local\":"; j += localMode ? "true" : "false";
  j += ",\"clients\":" + String(WiFi.softAPgetStationNum());
  j += ",\"saved\":[";
  for (int i = 0; i < nSaved; i++) { if (i) j += ","; j += "\"" + jesc(savedSsid[i]) + "\""; }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleWifiScan() {                  // blocking for a few seconds; the page shows "scanning..."
  if (apActive && !wifiTrying) WiFi.mode(WIFI_AP_STA);      // scanning needs the station side
  int n = WiFi.scanNetworks();
  String names[24]; int rssi[24]; bool enc[24]; int m = 0;
  for (int i = 0; i < n && m < 24; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    int dup = -1;
    for (int k = 0; k < m; k++) if (names[k] == s) dup = k;
    if (dup >= 0) { if (WiFi.RSSI(i) > rssi[dup]) rssi[dup] = WiFi.RSSI(i); continue; }
    names[m] = s; rssi[m] = WiFi.RSSI(i); enc[m] = WiFi.encryptionType(i) != ENC_TYPE_NONE; m++;
  }
  WiFi.scanDelete();
  if (apActive && !wifiTrying && WiFi.status() != WL_CONNECTED) WiFi.mode(WIFI_AP);   // back to the quiet setup network
  for (int a = 0; a < m; a++) for (int b = a + 1; b < m; b++) if (rssi[b] > rssi[a]) {   // strongest first
    String ts = names[a]; names[a] = names[b]; names[b] = ts;
    int tr = rssi[a]; rssi[a] = rssi[b]; rssi[b] = tr;
    bool te = enc[a]; enc[a] = enc[b]; enc[b] = te;
  }
  String j = "[";
  for (int i = 0; i < m; i++) { if (i) j += ","; j += "{\"s\":\"" + jesc(names[i]) + "\",\"r\":" + String(rssi[i]) + ",\"e\":" + (enc[i] ? "1" : "0") + "}"; }
  j += "]";
  server.send(200, "application/json", j);
}

void handleWifiMode() {                  // POST mode=local|wifi
  String m = server.arg("mode");
  if (m != "local" && m != "wifi") { server.send(400, "text/plain", "mode must be local or wifi"); return; }
  server.send(200, "text/plain", "ok");
  delay(150);
  applyWifiMode(m == "local");
}
void applyWifiMode(bool wantLocal) {
  localMode = wantLocal;
  saveMode();
  bootLog(wantLocal ? "mode: local" : "mode: wifi");
  if (wantLocal) startAP();
  else { if (apActive) stopAP(); forceRetry = true; }
}

void handleWifiAdd() {
  String ssid = server.arg("ssid"), pass = server.arg("pass");
  if (!ssid.length() || ssid.length() > 32 || ssid.indexOf('\t') >= 0 || ssid.indexOf('\n') >= 0) { server.send(400, "text/plain", "bad network name"); return; }
  if (pass.length() && (pass.length() < 8 || pass.length() > 63 || pass.indexOf('\n') >= 0)) { server.send(400, "text/plain", "password must be 8 to 63 characters"); return; }
  addSaved(ssid, pass);
  bool connectNow = server.arg("connect") == "1";
  server.send(200, "text/plain", "ok");
  if (connectNow) { delay(150); WiFi.disconnect(); forceRetry = true; }
}

void handleWifiDel() {
  delSaved(server.arg("ssid"));
  saveSaved();
  server.send(200, "text/plain", "ok");
}

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>PSS-270</title>
<link rel="stylesheet" href="/s.css">
<style>
#log{background:#0d0d0c;border:1px solid var(--line);border-radius:6px;padding:8px;height:45vh;overflow:auto;white-space:pre-wrap;font:13px monospace;color:var(--cyan)}
#q{width:60%}
</style></head><body>
<h2>PSS-270<a href="/tone">tone editor</a><a href="/wifi#ext">extras</a><a href="/wifi#kbd">settings</a></h2>
<div class="row">
<button class="go" onclick="c('park')">Park (Mega synth)</button>
<button onclick="c('unpark')">Unpark (stock PSS)</button>
<button class="alt" onclick="c('status')">Status</button>
<button class="alt" onclick="c('test')">Test chord</button>
</div>
<div class="row">
<button class="alt" onclick="c('learn')">Learn keys</button>
<button class="alt" onclick="c('done')">Done</button>
<button onclick="c('lock on')">Lock tone on</button>
<button onclick="c('lock off')">Lock tone off</button>
</div>
<div class="row">Voice <input id="p" type="number" min="0" max="114" value="0" style="width:70px">
<button class="go" onclick="c('prog '+document.getElementById('p').value)">Set</button>
<button onclick="step(-1)">-</button><button onclick="step(1)">+</button></div>
<div class="row"><input id="q" placeholder="command" onkeydown="if(event.key=='Enter'){c(this.value);this.value=''}">
<button class="alt" onclick="c(document.getElementById('q').value)">Send</button></div>
<div id="log"></div>
<script>
let head=0;const L=document.getElementById('log');
function add(t){if(!t)return;L.textContent+=t;L.scrollTop=L.scrollHeight}
function c(x){if(x)fetch('/cmd?c='+encodeURIComponent(x))}
function step(d){const p=document.getElementById('p');p.value=Math.max(0,Math.min(114,+p.value+d));c('prog '+p.value)}
async function poll(){if(!document.hidden){try{const r=await fetch('/log?from='+head);const h=r.headers.get('X-Log-Head');add(await r.text());if(h)head=+h}catch(e){}}setTimeout(poll,900)}
poll();
</script></body></html>)HTML";

void handleRoot() { server.send_P(200, "text/html", PAGE); }
void handleTone() { server.send_P(200, "text/html", TONE_PAGE); }
void handleWifiPage() { server.send_P(200, "text/html", WIFI_PAGE); }
void handleCss() { server.sendHeader("Cache-Control", "max-age=600"); server.send_P(200, "text/css", THEME_CSS); }

// Mega OTA: on an avrdude connection, tell the Mega to jump into its bootloader, then pass bytes
// both ways until avrdude hangs up. Needs the slide switch on RXD0/TXD0 (bootloader = Serial0).
void flashBridge() {
  WiFiClient c = flashSrv.accept();
  c.setNoDelay(true);
  bootLog("flash: rebooting Mega into bootloader");
  Serial.print("\nreboot\n");
  Serial.flush();
  delay(60);
  while (Serial.available()) Serial.read();
  uint8_t buf[256];
  unsigned long idle = millis();
  while (c.connected() && millis() - idle < 15000) {
    int n = c.available();
    if (n > 0) { n = c.read(buf, min(n, (int)sizeof buf)); Serial.write(buf, n); idle = millis(); }
    int m = 0;
    while (Serial.available() && m < (int)sizeof buf) buf[m++] = Serial.read();
    if (m) { c.write(buf, m); idle = millis(); }
    yield();
  }
  c.stop();
  bootLog("flash: done");
}

void setup() {
  Serial.begin(115200);
  delay(50);
  char m[96];
  fsOk = LittleFS.begin();
  if (fsOk) { File t = LittleFS.open("/boot.txt", "r"); if (t && t.size() > 12000) { t.close(); LittleFS.remove("/boot.txt"); } else if (t) t.close(); }
  snprintf(m, sizeof m, "boot, last reset: %s", ESP.getResetReason().c_str()); bootLog(m);
  rtcBootCount();
  if (gentle) bootLog("gentle start: the last boots did not last (brownout?)");
  loadSaved();
  loadMode();
  snprintf(m, sizeof m, "%d saved network(s)", nSaved); bootLog(m);
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();              // radio completely off while the rail settles (the Mega boots at the same time)
  delay(gentle ? 2500 : 1200);
  WiFi.forceSleepWake();
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setPhyMode(WIFI_PHY_MODE_11G);  // 11b / 11n bursts draw the most current
  txDbm = gentle ? 5 : 10;
  WiFi.setOutputPower(txDbm);
  WiFi.setAutoReconnect(true);
  WiFi.hostname("pss270");
  bootLog(gentle ? "radio on (5 dBm, 11g)" : "radio on (10 dBm, 11g)");
  if (localMode) { bootLog("local mode: own WiFi only"); startAP(); }
  else if (!gentle && !joinWifi()) startAP();   // no network: the setup WiFi opens alone and stays up

  MDNS.begin("pss270");
  ArduinoOTA.setHostname("pss270");
  ArduinoOTA.begin();
  server.on("/", handleRoot);
  server.on("/cmd", handleCmd);
  server.on("/log", handleLog);
  server.on("/fire", handleFire);
  server.on("/sys", handleSys);
  server.on("/bootlog", handleBootlog);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/tone", handleTone);
  server.on("/s.css", handleCss);
  server.on("/wifi", handleWifiPage);
  server.on("/wifi/list", handleWifiList);
  server.on("/wifi/scan", handleWifiScan);
  server.on("/wifi/mode", HTTP_POST, handleWifiMode);
  server.on("/wifi/add", HTTP_POST, handleWifiAdd);
  server.on("/wifi/del", HTTP_POST, handleWifiDel);
  server.begin();
  tcp.begin();
  tcp.setNoDelay(true);
  flashSrv.begin();
  MDNS.addService("http", "tcp", 80);
}

void loop() {
  ArduinoOTA.handle();
  MDNS.update();
  server.handleClient();
  pumpMega();
  if (flashSrv.hasClient()) flashBridge();

  if (tcp.hasClient()) {
    if (tcpClient && tcpClient.connected()) tcpClient.stop();
    tcpClient = tcp.accept();
  }
  while (tcpClient && tcpClient.available()) Serial.write(tcpClient.read());

  wifiService();
  static unsigned long lastIp = 0;
  if (millis() - lastIp > 15000) {
    lastIp = millis();
    Serial.print(localMode ? "mode local\n" : "mode wifi\n");
    if (WiFi.status() == WL_CONNECTED) { Serial.print("ip "); Serial.print(WiFi.localIP().toString()); Serial.print('\n'); }
    else if (apActive) Serial.print("ip AP 192.168.4.1\n");
    else Serial.print("ip none\n");
  }
  static uint8_t beat = 0;
  static const uint16_t BEATS[] = {5, 10, 20, 40, 90, 300};
  if (beat < 6 && millis() / 1000 >= BEATS[beat]) {
    char hb[80]; snprintf(hb, sizeof hb, "alive %us heap %u %s rssi %d", BEATS[beat], ESP.getFreeHeap(), apActive ? "AP" : (WiFi.status() == WL_CONNECTED ? "online" : "no-wifi"), WiFi.RSSI());
    flog(hb); beat++;
  }
  static bool bootOk = false;
  if (!bootOk && millis() > 20000) { bootOk = true; rtcBootOk(); }

  uint32_t h = ESP.getFreeHeap();        // safety net: if memory runs out the web server stalls, so restart before that
  if (h < minHeap) minHeap = h;
  if (h < 7000) { bootLog("low memory: restarting"); delay(150); ESP.restart(); }
}
