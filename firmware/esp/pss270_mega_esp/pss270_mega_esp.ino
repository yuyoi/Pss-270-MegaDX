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

void pumpMega() {
  while (Serial.available()) {
    char c = Serial.read();
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

void bootLog(const char* msg) {
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
int netCount() { return nSaved + NET_COUNT; }
String netSsid(int i) { return i < nSaved ? savedSsid[i] : String(NETS[i - nSaved].ssid); }
String netPass(int i) { return i < nSaved ? savedPass[i] : String(NETS[i - nSaved].pass); }

void startAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("PSS270-setup", "pss270setup");
  apActive = true;
  bootLog("no network: setup WiFi PSS270-setup is on (192.168.4.1/wifi)");
}
void stopAP() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
  bootLog("setup WiFi off");
}

bool joinWifi() {                      // blocking, used once at boot
  for (int i = 0; i < netCount(); i++) {
    String s = netSsid(i);
    char m[64]; snprintf(m, sizeof m, "trying %s", s.c_str()); bootLog(m);
    WiFi.begin(s.c_str(), netPass(i).c_str());
    unsigned long start = millis();
    while (millis() - start < 10000) {
      if (WiFi.status() == WL_CONNECTED) return true;
      delay(200);
    }
    WiFi.disconnect();
  }
  return false;
}

// Keeps the connection alive without ever blocking the web server: one network at a time,
// 10 s each; after a full round fails, wait 20 s and start the setup network.
void wifiService() {
  static unsigned long nextTry = 0, tryStart = 0, upSince = 0;
  static int idx = -1;
  static bool trying = false, wasUp = false;
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
    trying = false;
    if (!upSince) upSince = now;
    if (apActive && now - upSince > 60000 && WiFi.softAPgetStationNum() == 0) stopAP();   // online for a minute: the setup network is not needed
    return;
  }
  upSince = 0;
  if (forceRetry) { forceRetry = false; trying = false; idx = -1; nextTry = 0; }
  if (!trying) {
    if (now < nextTry || netCount() == 0) return;
    idx = (idx + 1) % netCount();
    WiFi.begin(netSsid(idx).c_str(), netPass(idx).c_str());
    trying = true; tryStart = now;
  } else if (now - tryStart > 10000) {
    trying = false;
    if (idx >= netCount() - 1) { idx = -1; nextTry = now + 20000; if (!apActive) startAP(); }
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
  j += ",\"saved\":[";
  for (int i = 0; i < nSaved; i++) { if (i) j += ","; j += "\"" + jesc(savedSsid[i]) + "\""; }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleWifiScan() {                  // blocking for a few seconds; the page shows "scanning..."
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
<h2>PSS-270<a href="/tone">tone editor</a><a href="/wifi">wifi</a></h2>
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
async function poll(){try{const r=await fetch('/log?from='+head);const h=r.headers.get('X-Log-Head');add(await r.text());if(h)head=+h}catch(e){}setTimeout(poll,500)}
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
  snprintf(m, sizeof m, "boot, last reset: %s", ESP.getResetReason().c_str()); bootLog(m);
  LittleFS.begin();
  loadSaved();
  snprintf(m, sizeof m, "%d saved network(s)", nSaved); bootLog(m);
  WiFi.persistent(false);
  delay(500);                          // let the rail settle before the radio draws anything
  bootLog("radio on (10 dBm)");
  WiFi.mode(WIFI_STA);
  WiFi.setOutputPower(10);
  WiFi.hostname("pss270");
  if (!joinWifi()) startAP();          // no network: open the setup WiFi, keep retrying in the background

  MDNS.begin("pss270");
  ArduinoOTA.setHostname("pss270");
  ArduinoOTA.begin();
  server.on("/", handleRoot);
  server.on("/cmd", handleCmd);
  server.on("/log", handleLog);
  server.on("/fire", handleFire);
  server.on("/tone", handleTone);
  server.on("/s.css", handleCss);
  server.on("/wifi", handleWifiPage);
  server.on("/wifi/list", handleWifiList);
  server.on("/wifi/scan", handleWifiScan);
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
}
