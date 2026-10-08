// PSS-270 brain on the WiFi Mega (ATmega2560 side). Port of pss270_nano/nano_spilink to the
// Mega pin map, plus key-matrix learn/scan and a MIDI-driven YM2413 synth.
//
// Wiring (5 V everywhere, no shifters):
//   D2  <- CPU-side stub of the cut /CS jumper (INT4, the relay input)
//   D3  -> YM2413 /CS, chip pin 12 (chip side of the cut jumper)
//   D4-D11 -> YM2413 D0-D7 (chip pins 17,18,2,3,4,5,6,7)
//   D12 -> YM2413 A0, chip pin 10
//   D13 -> YM2413 /IC, chip pin 13 (needed once parked: P74 floats, board pulldown resets the chip)
//   D22 -> 4.7k-10k -> Tr3 base. HIGH holds the HD6301 in reset ("parked")
//   Even pins 24-52 -> the 15 keyboard ribbon leads, any order (learn mode sorts them out)
//   Serial1 D18/D19 -> MIDI shield (31250). Serial3 = onboard ESP8266. Serial = USB console.
//
// Modes:
//   UNPARKED (boot default): the PSS-270 runs stock. The Mega only relays /CS from D2 to D3.
//     "lock on" additionally blocks the CPU's writes to regs 0-7 so a custom tone survives.
//   PARKED: CPU held in reset, the Mega owns the YM2413 and scans the keys itself.
//     Keys -> local sound + MIDI out. MIDI in -> 9-voice YM2413 synth.
//     Program 0-99 = the PSS-270's own 100 tones, 100-114 = YM2413 ROM instruments 1-15.
//
// Console (USB 115200 or the ESP on Serial3), one command per line:
//   status | park | unpark | learn | done | base <note> | prog <0-114> | lock on|off
//   tone <8 hex bytes> | thru on|off | keys on|off | test | forget

#include <EEPROM.h>
#include <avr/wdt.h>
#include "pss270_tones.h"
#include "pss270_voices.h"
struct LineBuf { char b[64]; uint8_t n; };   // declared early so the auto-generated prototypes can see it
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---- pins ----
#define PIN_CS_CPU 2    // PE4 / INT4
#define PIN_CS     3    // PE5
const uint8_t DATA_PINS[8] = {4, 5, 6, 7, 8, 9, 10, 23};  // D0..D7. D7 moved from D11 to D23 (intermittent D7 on the old pin/wire)
#define PIN_ADDR   12   // PB6
#define PIN_IC     13   // PB7
#define PIN_HOLD   22
#define NLEADS     15
const uint8_t LEADS[NLEADS] = {24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52};

// ---- EEPROM ----
#define EE_MAGIC 0xA7
#define EE_BASE  2
#define EE_LOCK  3
#define EE_PROG  4
#define EE_THRU  5
#define EE_CHAN  1     // MIDI channel 0-15
#define EE_TRANS 15    // transpose, signed semitones
#define EE_CUSTOM  256   // 1 = the saved tone bytes (EE_TONE) are a hand-made tone, not a PSS voice
#define EE_ARPMODE 250
#define EE_ARPBPM  251
#define EE_ARPOCT  252
#define EE_ARPLATCH 253
#define EE_SENSE   254
#define EE_STANDBY 255   // seconds
#define EE_BOOT  14    // 1 = park (Mega synth) automatically after the PSS powers on
#define EE_TONE  6     // 8 bytes
#define EE_MAP   16    // NLEADS*NLEADS bytes, keyNote[driver][sense], 0xFF = no key

uint8_t keyNote[NLEADS][NLEADS];
bool    isDriver[NLEADS];
uint8_t baseNote = 36;  // PSS-270 lowest key C1 -> MIDI 36
bool    lockTone = false, midiThru = false, printKeys = true;
uint8_t prog = 0;       // 0-99 PSS tone, 100-114 ROM instrument
uint8_t userTone[8];
uint8_t midiChan = 0;               // 0-15 (channel 1 = 0), used for MIDI in and out
int8_t  transpose = 0;              // semitones, applied to the keys' notes
bool    menuMode = false, menuEdit = false;
uint8_t menuItem = 0;
uint8_t keyDown[16];                // bit per MIDI note currently held on the keyboard
uint8_t topNote1 = 0xFF, topNote2 = 0xFF, lowNote = 0xFF;   // highest, second highest, lowest learned key
unsigned long comboSince = 0, previewOffAt = 0;
bool comboLatched = false;
uint8_t previewNote = 0;
void computeTop() {
  topNote1 = topNote2 = lowNote = 0xFF;
  uint8_t hi1 = 0, hi2 = 0, lo = 0xFF; bool any = false;
  for (uint8_t d = 0; d < NLEADS; d++) for (uint8_t s2 = 0; s2 < NLEADS; s2++) {
    uint8_t n = keyNote[d][s2]; if (n == 0xFF) continue;
    any = true;
    if (n > hi1) { hi2 = hi1; hi1 = n; } else if (n > hi2) hi2 = n;
    if (n < lo) lo = n;
  }
  if (any && hi2) { topNote1 = hi1; topNote2 = hi2; lowNote = lo; }
}
bool noteIsDown(uint8_t n) { return keyDown[n >> 3] & (1 << (n & 7)); }


// Everything the Mega reports goes to USB and to the onboard ESP (web page / TCP console).
class Tee : public Print {
 public:
  size_t write(uint8_t c) override { Serial.write(c); Serial3.write(c); return 1; }
};
Tee LOG;

// ---- OLED (SSD1306 128x64 on D20/D21) ----
// Redrawn only when something changes: an I2C transfer while unparked could delay the /CS relay
// interrupt, so in stock mode the screen is touched only on mode changes.
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool oledOk = false;
bool oledDirty = true;
char oledLast[22] = "";      // last key / MIDI event line
void oledEvent(const char *t) { strncpy(oledLast, t, sizeof oledLast - 1); oledDirty = true; }

// ---- relay state ----
volatile bool parked = false;
volatile bool lockActive = false;      // lockTone AND past the CPU's boot wipe
volatile bool blockThis = false;
unsigned long lockAt = 0;
bool lockPending = false;
volatile bool relayOn = false;          // false while the PSS seems switched off
volatile bool relayJustOn = false;      // set by the ISR when the CPU's first /CS edge starts the relay

// /CS relay: mirror the CPU's /CS onto the chip, except data writes to regs 0-7 while locked.
// The register number sits on the bus during the address phase (A0 low), before the data phase.
// Bus sniff (stock mode): what the CPU puts on D4-D11 / D12 at each /CS falling edge.
#define SNIFF_N 160
volatile uint8_t sniffData[SNIFF_N], sniffA0[SNIFF_N];
volatile uint8_t sniffIdx = SNIFF_N;    // SNIFF_N = not capturing

volatile uint16_t parkedEdges = 0;     // CPU /CS edges seen while parked: must stay 0 (CPU held in reset)
ISR(INT4_vect) {
  if (parked) { parkedEdges++; return; }
  if (!(DDRE & _BV(5))) {                                   // first CPU /CS edge since power-up:
    PORTE |= _BV(5); DDRE |= _BV(5);                        // start relaying at once, so the CPU's
    relayOn = true; relayJustOn = true;                     // boot-time writes (tones!) aren't lost
  }
  if (PINE & _BV(4)) {                                      // CPU deselects: pass
    PORTE |= _BV(5);
    if (sniffIdx < SNIFF_N) {                               // the chip latches on this edge: record what it sees
      uint8_t g = PING, e = PINE, h = PINH, b = PINB;       // raw levels, bit i = Mega pin D(4+i)
      sniffData[sniffIdx] = ((g >> 5) & 1) | (((e >> 3) & 1) << 1) | (((h >> 3) & 1) << 2) | (((h >> 4) & 1) << 3)
                          | (((h >> 5) & 1) << 4) | (((h >> 6) & 1) << 5) | (((b >> 4) & 1) << 6) | (((b >> 5) & 1) << 7);
      sniffA0[sniffIdx] = (b >> 6) & 1;
      sniffIdx++;
    }
    return;
  }
  if (!lockActive) {
    PORTE &= ~_BV(5);
    return;
  }
  if (!(PINB & _BV(6))) {                                   // address phase
    blockThis = ((PINH & 0x70) | (PINB & 0x30)) == 0;       // D3-D5 = PH4-6, D6-D7 = PB4-5
    PORTE &= ~_BV(5);
  } else if (blockThis) {
    PORTE |= _BV(5);                                        // drop this data write
  } else {
    PORTE &= ~_BV(5);
  }
}

// ---- YM2413 bus (parked only) ----
void busIdle() {
  for (uint8_t i = 0; i < 8; i++) pinMode(DATA_PINS[i], INPUT);
  pinMode(PIN_ADDR, INPUT);
}
void busTakeOver() {
  for (uint8_t i = 0; i < 8; i++) pinMode(DATA_PINS[i], OUTPUT);
  pinMode(PIN_ADDR, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_CS, OUTPUT);
}
bool senseOn = false, pssDead = false, standby = false;   // PSS power sense (see senseTick)
bool customTone = false;        // userTone is a hand-edited tone (live panel / saved), not the table voice
bool bootMega = false;          // 'boot mega': park by itself after the PSS powers on
unsigned long parkAt = 0;
bool scanEnabled = true;   // 'scan off' pauses the key scan (noise tests)
bool ymSlow = true;    // relaxed write timing: the datasheet minimums gave wrong pitches on this wiring ("slow off" to test)
void ymWrite(uint8_t reg, uint8_t data) {
  for (uint8_t i = 0; i < 8; i++) digitalWrite(DATA_PINS[i], (reg >> i) & 1);
  digitalWrite(PIN_ADDR, LOW);
  digitalWrite(PIN_CS, LOW); delayMicroseconds(ymSlow ? 20 : 1); digitalWrite(PIN_CS, HIGH);
  delayMicroseconds(ymSlow ? 100 : 4);
  for (uint8_t i = 0; i < 8; i++) digitalWrite(DATA_PINS[i], (data >> i) & 1);
  digitalWrite(PIN_ADDR, HIGH);
  digitalWrite(PIN_CS, LOW); delayMicroseconds(ymSlow ? 20 : 1); digitalWrite(PIN_CS, HIGH);
  delayMicroseconds(ymSlow ? 400 : 24);
  // Park the chip's address latch on an unused register (0x09). Noise on the long /CS wire
  // (key-scan ribbon, OLED redraws) can cause a phantom data write; with the latch here it lands
  // nowhere instead of overwriting the pitch of the note just played.
  for (uint8_t i = 0; i < 8; i++) digitalWrite(DATA_PINS[i], (0x09 >> i) & 1);
  digitalWrite(PIN_ADDR, LOW);
  digitalWrite(PIN_CS, LOW); delayMicroseconds(ymSlow ? 20 : 1); digitalWrite(PIN_CS, HIGH);
  delayMicroseconds(ymSlow ? 100 : 4);
}
void ymReset() {
  for (uint8_t r = 0; r <= 0x38; r++)
    if (r <= 7 || r == 0x0E || (r >= 0x10 && r <= 0x18) || (r >= 0x20 && r <= 0x28) || r >= 0x30)
      ymWrite(r, 0);
}
void loadUserTone() { for (uint8_t i = 0; i < 8; i++) ymWrite(i, userTone[i]); }

// ---- synth ----
struct Voice { uint8_t note; uint8_t vel; bool on; bool held; uint32_t age; uint16_t fnum; uint8_t block; };
Voice voices[9];
uint32_t ageCounter = 0;
int  bend = 0;          // -8192..8191
bool sustain = false;

void calcPitch(uint8_t note, uint16_t &fnum, uint8_t &block) {
  float f = 440.0f * pow(2.0f, (note - 69 + bend / 4096.0f) / 12.0f);   // +-2 semitones
  for (block = 0; block < 7; block++) {
    float fn = f * (float)(1UL << (19 - block)) / 49716.0f;
    if (fn < 512) { fnum = (uint16_t)(fn + 0.5f); if (fnum > 511) fnum = 511; return; }
  }
  fnum = 511;
}
uint8_t instNibble() { return prog >= 100 ? (prog - 99) : 0; }

void voiceKey(uint8_t v, bool on) {
  // Same register order the PSS's own CPU uses: key-off with the NEW block/fnum8 first, then
  // fnum low, then the final key-on write. (Changing fnum low while the old block is still set
  // bends the release tail of whatever was sounding.)
  uint8_t hi = (sustain ? 0x20 : 0) | (voices[v].block << 1) | ((voices[v].fnum >> 8) & 1);
  ymWrite(0x20 + v, hi);
  ymWrite(0x10 + v, voices[v].fnum & 0xFF);
  ymWrite(0x20 + v, (on ? 0x10 : 0) | hi);
}
void synthNoteOn(uint8_t note, uint8_t vel) {
  if (!parked) return;
  if (vel == 0) { synthNoteOff(note); return; }
  int8_t pick = -1; uint32_t oldest = 0xFFFFFFFF;
  for (uint8_t v = 0; v < 9; v++) if (!voices[v].on && !voices[v].held && voices[v].age < oldest) { oldest = voices[v].age; pick = v; }
  if (pick < 0) for (uint8_t v = 0; v < 9; v++) if (voices[v].age < oldest) { oldest = voices[v].age; pick = v; }
  Voice &vc = voices[pick];
  if (vc.on || vc.held) voiceKey(pick, false);
  vc.note = note; vc.vel = vel; vc.on = true; vc.held = false; vc.age = ++ageCounter;
  calcPitch(note, vc.fnum, vc.block);
  ymWrite(0x30 + pick, (instNibble() << 4) | ((127 - vel) >> 3));
  voiceKey(pick, true);
}
void synthNoteOff(uint8_t note) {
  if (!parked) return;
  for (uint8_t v = 0; v < 9; v++) if (voices[v].on && voices[v].note == note) {
    voices[v].on = false;
    if (sustain) voices[v].held = true; else voiceKey(v, false);
  }
}
void allOff() {
  for (uint8_t v = 0; v < 9; v++) { voices[v].on = voices[v].held = false; if (parked) voiceKey(v, false); }
}
void applyBend() {
  for (uint8_t v = 0; v < 9; v++) if (voices[v].on || voices[v].held) {
    calcPitch(voices[v].note, voices[v].fnum, voices[v].block);
    voiceKey(v, true);
  }
}
void setProgram(uint8_t p) {
  if (p > 114) return;
  prog = p;
  customTone = false; EEPROM.update(EE_CUSTOM, 0);
  if (p < 100) {
    for (uint8_t i = 0; i < 8; i++) userTone[i] = pgm_read_byte(&PSS270_TONES[p].t[i]);
    if (parked) loadUserTone();
  }
  EEPROM.update(EE_PROG, prog);
  oledDirty = true;
}

// ---- park / unpark ----
void doPark() {
  if (parked) return;
  // Never drive the YM bus into an unpowered PSS: it back-feeds the board through the chips'
  // pin clamps, dips our 5V rail (the OLED resets) and stresses the chips.
  if (!relayOn || pssDead) { LOG.println(F("park refused: PSS is off")); return; }
  parked = true;                       // relay ISR stands down
  parkedEdges = 0;
  pinMode(PIN_HOLD, OUTPUT); digitalWrite(PIN_HOLD, HIGH);
  delay(5);
  pinMode(PIN_IC, OUTPUT); digitalWrite(PIN_IC, HIGH);
  delayMicroseconds(200);
  busTakeOver();
  ymReset();
  loadUserTone();
  for (uint8_t v = 0; v < 9; v++) voices[v] = Voice{0, 0, false, false, 0, 0, 0};
  for (uint8_t i = 0; i < NLEADS; i++) pinMode(LEADS[i], INPUT_PULLUP);
  LOG.println(F("parked: CPU in reset, Mega owns the chip and keys"));
  oledDirty = true;
}
void doUnpark() {
  if (!parked) return;
  for (uint8_t i = 0; i < NLEADS; i++) pinMode(LEADS[i], INPUT);   // CPU scans again
  blockThis = false; lockActive = false;
  lockPending = lockTone; lockAt = millis() + 30;   // let the CPU's boot wipe through first
  digitalWrite(PIN_CS, HIGH);
  busIdle();
  pinMode(PIN_IC, INPUT);
  parked = false;
  pinMode(PIN_HOLD, INPUT);            // CPU starts here
  LOG.println(F("unparked: PSS-270 runs stock, /CS relayed"));
  oledDirty = true;
}
// Brief takeover to put the custom tone back after the CPU's boot wipe (same as the Nano).
void reinjectTone() {
  parked = true;
  busTakeOver();
  loadUserTone();
  busIdle();
  digitalWrite(PIN_CS, HIGH);
  parked = false;
}

// ---- key matrix ----
bool learning = false;
uint8_t learnNext = 0;
unsigned long learnIdle = 0;
uint16_t keyState[NLEADS];   // bit per sense lead, for driver leads
uint16_t keyPrev[NLEADS];

uint16_t readSense(uint8_t d) {
  pinMode(LEADS[d], OUTPUT); digitalWrite(LEADS[d], LOW);
  delayMicroseconds(10);
  uint16_t bits = 0;
  for (uint8_t s = 0; s < NLEADS; s++) if (s != d && !digitalRead(LEADS[s])) bits |= 1 << s;
  pinMode(LEADS[d], INPUT_PULLUP);    // back to pulled-up input, never driven high
  return bits;
}
void rebuildDrivers() {
  for (uint8_t d = 0; d < NLEADS; d++) {
    isDriver[d] = false;
    for (uint8_t s = 0; s < NLEADS; s++) if (keyNote[d][s] != 0xFF) isDriver[d] = true;
  }
  computeTop();
}
void saveMap() {
  for (uint8_t d = 0; d < NLEADS; d++) for (uint8_t s = 0; s < NLEADS; s++) EEPROM.update(EE_MAP + d * NLEADS + s, keyNote[d][s]);
}
uint16_t learnBase[NLEADS];   // pairs already closed when learn starts (a stuck/dirty key): ignored
void learnStart() {
  if (!parked) doPark();
  if (!parked) return;                 // PSS off: park refused
  memset(keyNote, 0xFF, sizeof keyNote);
  learnNext = 0; learning = true; learnIdle = 0; oledDirty = true;
  for (uint8_t d = 0; d < NLEADS; d++) {
    learnBase[d] = readSense(d);
    for (uint8_t s2 = 0; s2 < NLEADS; s2++) if (learnBase[d] & (1 << s2)) {
      LOG.print(F("ignoring pair already closed (stuck key?): pin ")); LOG.print(LEADS[d]);
      LOG.print(F(" -> pin ")); LOG.println(LEADS[s2]);
    }
  }
  LOG.println(F("learn: press every key once, lowest to highest, one at a time. 'done' or 8 s idle ends it"));
}
void learnEnd() {
  learning = false;
  oledDirty = true;
  rebuildDrivers(); saveMap();
  uint8_t nd = 0; for (uint8_t d = 0; d < NLEADS; d++) nd += isDriver[d];
  LOG.print(F("learn done: ")); LOG.print(learnNext); LOG.print(F(" keys, "));
  LOG.print(nd); LOG.println(F(" scan (B) leads"));
  if (learnNext == 0) LOG.println(F("nothing seen: check the ribbon, or this matrix is active-high"));
}
void learnTick() {
  static int8_t lastD = -1, lastS = -1;
  int8_t fd = -1, fs = -1; uint8_t hits = 0;
  for (uint8_t d = 0; d < NLEADS; d++) {
    uint16_t b = readSense(d) & ~learnBase[d];
    for (uint8_t s = 0; s < NLEADS; s++) if (b & (1 << s)) { fd = d; fs = s; hits++; }
  }
  if (hits == 1 && (fd != lastD || fs != lastS) && keyNote[fd][fs] == 0xFF) {
    keyNote[fd][fs] = baseNote + learnNext;
    LOG.print(F("key ")); LOG.print(learnNext + 1); LOG.print(F(": lead "));
    LOG.print(fd + 1); LOG.print(F(" (pin ")); LOG.print(LEADS[fd]); LOG.print(F(") -> lead "));
    LOG.print(fs + 1); LOG.print(F(" (pin ")); LOG.print(LEADS[fs]); LOG.println(F(")"));
    learnNext++; learnIdle = millis();
    synthNoteOn(keyNote[fd][fs], 100); delay(80); synthNoteOff(keyNote[fd][fs]);
  }
  if (hits == 0) { lastD = -1; lastS = -1; } else if (hits == 1) { lastD = fd; lastS = fs; }
  if (learnNext && millis() - learnIdle > 8000) learnEnd();
}
// ---- arpeggiator ----
// Held notes (from the keys or MIDI in) are played one after another instead of together.
#define ARP_MAX 16
const char* const ARP_NAMES[6] = {"off", "up", "down", "up-down", "random", "as played"};
uint8_t  arpMode = 0, arpBpm = 120, arpOct = 1;
bool     arpLatch = false;
uint8_t  arpHeld[ARP_MAX], arpCount = 0;
int16_t  arpSounding = -1;
uint8_t  arpPressed = 0;                 // keys physically down
unsigned long arpNext = 0, arpOffAt = 0;
uint16_t arpPos = 0;

void arpStopNote() { if (arpSounding >= 0) { synthNoteOff(arpSounding); arpSounding = -1; } }
void arpClear() { arpStopNote(); arpCount = 0; arpPressed = 0; arpPos = 0; }
void noteOnUser(uint8_t n, uint8_t v) {
  if (!arpMode) { synthNoteOn(n, v); return; }
  if (v == 0) return;
  arpPressed++;
  if (arpLatch && arpPressed == 1) { arpStopNote(); arpCount = 0; arpPos = 0; }   // a new chord replaces the latched one
  for (uint8_t i = 0; i < arpCount; i++) if (arpHeld[i] == n) return;
  if (arpCount < ARP_MAX) arpHeld[arpCount++] = n;
  if (arpCount == 1) arpNext = millis();
}
void noteOffUser(uint8_t n) {
  if (!arpMode) { synthNoteOff(n); return; }
  if (arpPressed) arpPressed--;
  if (arpLatch) return;
  for (uint8_t i = 0; i < arpCount; i++) if (arpHeld[i] == n) {
    for (uint8_t j = i; j + 1 < arpCount; j++) arpHeld[j] = arpHeld[j + 1];
    arpCount--;
    break;
  }
  if (!arpCount) arpStopNote();
}
void arpTick() {
  if (!parked) return;
  unsigned long now = millis();
  if (arpSounding >= 0 && (long)(now - arpOffAt) >= 0) arpStopNote();
  if (!arpMode || !arpCount || (long)(now - arpNext) < 0) return;
  unsigned long step = 15000UL / arpBpm;                 // 16th notes
  uint8_t tmp[ARP_MAX];
  memcpy(tmp, arpHeld, arpCount);
  if (arpMode != 5)                                      // everything but "as played" runs low to high
    for (uint8_t a = 1; a < arpCount; a++) { uint8_t k = tmp[a]; int b = a - 1; while (b >= 0 && tmp[b] > k) { tmp[b + 1] = tmp[b]; b--; } tmp[b + 1] = k; }
  uint16_t total = (uint16_t)arpCount * arpOct, p;
  switch (arpMode) {
    case 2: p = total - 1 - (arpPos % total); break;
    case 3: { uint16_t per = total > 1 ? 2 * total - 2 : 1, q = arpPos % per; p = q < total ? q : per - q; break; }
    case 4: p = random(total); break;
    default: p = arpPos % total; break;
  }
  arpPos++;
  int n = tmp[p % arpCount] + 12 * (p / arpCount);
  arpStopNote();
  if (n <= 127) { synthNoteOn(n, 100); arpSounding = n; }
  arpOffAt = now + step * 6 / 10;
  arpNext += step;
  if ((long)(now - arpNext) > 0) arpNext = now + step;   // fell behind (e.g. a screen redraw): don't burst
}

// ---- PSS power sense (optional wire: PSS +5V rail -> 10k -> A0) ----
// With it, a parked Mega notices the PSS going off, lets go of every pin (no back-feed), and after
// standbyAfter seconds blanks the screen. It wakes by itself when the 5V comes back.
uint8_t  standbyAfter = 20;
unsigned long deadSince = 0, aliveSince = 0;
int      senseRaw = 0;
void releasePins() {
  if (parked) {
    for (uint8_t i = 0; i < NLEADS; i++) pinMode(LEADS[i], INPUT);
    busIdle();
    pinMode(PIN_IC, INPUT);
    pinMode(PIN_HOLD, INPUT);
    parked = false;
    allOff();
  }
  pinMode(PIN_CS, INPUT);
  relayOn = false;
  oledDirty = true;
}
void senseTick() {
  static unsigned long last = 0;
  unsigned long now = millis();
  if (now - last < 100) return;
  last = now;
  // Where does "the PSS is off" come from?
  //  - with the A0 wire (sense on): the PSS 5 V rail, works parked or not
  //  - without it, in stock mode: the CPU's own /CS line (relayOn), which needs no wire
  //  - without it, while parked: the CPU is held in reset, its /CS says nothing, so we cannot tell
  int pssOff;                                                  // 1 off, 0 on, -1 unknown
  if (senseOn) {
    senseRaw = analogRead(A0);
    pssOff = senseRaw < 600 ? 1 : (senseRaw > 700 ? 0 : -1);   // below about 2.9 V = off
  } else if (!parked) pssOff = relayOn ? 0 : 1;
  else return;
  if (pssOff < 0) return;
  if (pssOff == 1) {
    aliveSince = 0;
    if (!deadSince) deadSince = now;
    if (!pssDead && now - deadSince > 1000) {
      pssDead = true; releasePins();
      LOG.println(F("PSS power lost: every pin released"));
    }
    if (pssDead && !standby && now - deadSince > (unsigned long)standbyAfter * 1000UL) {
      standby = true;
      if (oledOk) oled.ssd1306_command(SSD1306_DISPLAYOFF);
      LOG.println(F("standby: screen off, waiting for the PSS"));
    }
  } else {
    deadSince = 0;
    if (!aliveSince) aliveSince = now;
    if (pssDead && now - aliveSince > 300) {
      pssDead = false;
      if (standby) { standby = false; if (oledOk) oled.ssd1306_command(SSD1306_DISPLAYON); }
      oledDirty = true;
      LOG.println(F("PSS power back"));
    }
  }
}

// ---- key menu ----
// Enter/leave: hold the two highest keys for 5 s. Inside: C = left, D = select, E = right
// (the three lowest white keys). Every change is saved to EEPROM at once.
#define MENU_ITEMS 11
const char* const MENU_NAMES[MENU_ITEMS] = {"Voice", "Transpose", "MIDI chan", "MIDI thru", "Boot mode", "Tone lock", "Arp mode", "Arp tempo", "Arp octs", "Arp latch", "Exit menu"};
void menuValue(char *buf, size_t n) {
  switch (menuItem) {
    case 0: if (prog < 100) snprintf(buf, n, "%02u %s", prog, PSS270_VOICES[prog].name); else snprintf(buf, n, "ROM voice %u", prog - 99); break;
    case 1: snprintf(buf, n, "%+d semitones", transpose); break;
    case 2: snprintf(buf, n, "%u", midiChan + 1); break;
    case 3: snprintf(buf, n, "%s", midiThru ? "on" : "off"); break;
    case 4: snprintf(buf, n, "%s", bootMega ? "MEGA synth" : "STOCK PSS"); break;
    case 5: snprintf(buf, n, "%s", lockTone ? "on" : "off"); break;
    case 6: snprintf(buf, n, "%s", ARP_NAMES[arpMode]); break;
    case 7: snprintf(buf, n, "%u BPM", arpBpm); break;
    case 8: snprintf(buf, n, "%u", arpOct); break;
    case 9: snprintf(buf, n, "%s", arpLatch ? "on" : "off"); break;
    default: snprintf(buf, n, "press D"); break;
  }
}
void menuOpen(bool open) {
  allOff(); arpClear();
  menuMode = open; menuEdit = false; menuItem = 0;
  oledDirty = true;
  LOG.println(open ? F("menu: open (C left, D select, E right)") : F("menu: closed, settings saved"));
}
void previewVoice() {                    // short note so you hear the voice you just picked
  allOff();
  previewNote = 60; synthNoteOn(60, 100); previewOffAt = millis() + 350;
}
void menuAdjust(int8_t dir) {            // value change while editing
  switch (menuItem) {
    case 0: { int p = (int)prog + dir; if (p < 0) p = 114; if (p > 114) p = 0; setProgram(p); previewVoice(); break; }
    case 1: { int t = transpose + dir; if (t < -24) t = -24; if (t > 24) t = 24; transpose = t; EEPROM.update(EE_TRANS, (uint8_t)transpose); break; }
    case 2: { midiChan = (midiChan + 16 + dir) & 15; EEPROM.update(EE_CHAN, midiChan); break; }
    case 7: { int t = (int)arpBpm + 5 * dir; if (t < 40) t = 40; if (t > 240) t = 240; arpBpm = t; EEPROM.update(EE_ARPBPM, arpBpm); break; }
  }
  oledDirty = true;
}
void menuKey(uint8_t note) {             // a key went down while the menu is open
  if (lowNote == 0xFF) return;
  if (note == lowNote) {                 // C = left
    if (menuEdit) menuAdjust(-1); else menuItem = (menuItem + MENU_ITEMS - 1) % MENU_ITEMS;
  } else if (note == lowNote + 4) {      // E = right
    if (menuEdit) menuAdjust(+1); else menuItem = (menuItem + 1) % MENU_ITEMS;
  } else if (note == lowNote + 2) {      // D = select
    switch (menuItem) {
      case 0: case 1: case 2: case 7: menuEdit = !menuEdit; break;
      case 3: midiThru = !midiThru; EEPROM.update(EE_THRU, midiThru); break;
      case 4: bootMega = !bootMega; EEPROM.update(EE_BOOT, bootMega ? 1 : 0); break;
      case 5: lockTone = !lockTone; EEPROM.update(EE_LOCK, lockTone); break;
      case 6: arpMode = (arpMode + 1) % 6; EEPROM.update(EE_ARPMODE, arpMode); arpClear(); break;
      case 8: arpOct = arpOct % 4 + 1; EEPROM.update(EE_ARPOCT, arpOct); break;
      case 9: arpLatch = !arpLatch; EEPROM.update(EE_ARPLATCH, arpLatch ? 1 : 0); arpClear(); break;
      default: menuOpen(false); return;
    }
  }
  oledDirty = true;
}
void comboTick() {                       // hold the two highest keys 5 s: toggle the menu
  static uint8_t lastSec = 0;
  if (parked && !learning && topNote1 != 0xFF && noteIsDown(topNote1) && noteIsDown(topNote2)) {
    if (!comboSince) { comboSince = millis(); lastSec = 0; }
    unsigned long held = millis() - comboSince;
    if (!comboLatched && !menuMode && held > 1000 && held / 1000 != lastSec && held < 5000) {
      lastSec = held / 1000;
      char t[22]; snprintf(t, sizeof t, "menu in %u s", 5 - lastSec); oledEvent(t);
    }
    if (!comboLatched && held >= 5000) { comboLatched = true; menuOpen(!menuMode); }
  } else { comboSince = 0; comboLatched = false; }
  if (previewOffAt && (long)(millis() - previewOffAt) >= 0) { previewOffAt = 0; synthNoteOff(previewNote); }
}

void midiOut3(uint8_t a, uint8_t b, uint8_t c) { Serial1.write(a); Serial1.write(b); Serial1.write(c); }
void scanTick() {
  for (uint8_t d = 0; d < NLEADS; d++) {
    if (!isDriver[d]) continue;
    uint16_t now = readSense(d);
    uint16_t stable = now & keyPrev[d] | (keyState[d] & (now | keyPrev[d]));   // 2-scan debounce
    keyPrev[d] = now;
    uint16_t chg = stable ^ keyState[d];
    keyState[d] = stable;
    for (uint8_t s = 0; chg && s < NLEADS; s++) if (chg & (1 << s)) {
      uint8_t n = keyNote[d][s];
      if (n == 0xFF) continue;
      bool on = stable & (1 << s);
      if (on) keyDown[n >> 3] |= 1 << (n & 7); else keyDown[n >> 3] &= ~(1 << (n & 7));
      if (menuMode) { if (on) menuKey(n); continue; }
      int pn = n + transpose; if (pn < 0 || pn > 127) continue;
      if (on) { noteOnUser(pn, 100); midiOut3(0x90 | midiChan, pn, 100); } else { noteOffUser(pn); midiOut3(0x80 | midiChan, pn, 0); }
      if (printKeys) { LOG.print(on ? F("key on  ") : F("key off ")); LOG.println(n); }
      if (on && !menuMode) { char t[22]; snprintf(t, sizeof t, "key  %u", n); oledEvent(t); }
    }
  }
}

// ---- MIDI in ----
uint8_t mStatus = 0, mData[2], mCount = 0;
void midiHandle(uint8_t st, uint8_t a, uint8_t b) {
  if ((st & 0x0F) != midiChan) return;          // the chosen MIDI channel only
  switch (st & 0xF0) {
    case 0x90: noteOnUser(a, b); if (b) { char t[22]; snprintf(t, sizeof t, "MIDI in %u vel %u", a, b); oledEvent(t); } break;
    case 0x80: noteOffUser(a); break;
    case 0xC0: setProgram(a); break;
    case 0xE0: bend = ((int)b << 7 | a) - 8192; if (parked) applyBend(); break;
    case 0xB0:
      if (a == 64) {
        sustain = b >= 64;
        if (!sustain) for (uint8_t v = 0; v < 9; v++) if (voices[v].held) { voices[v].held = false; if (parked) voiceKey(v, false); }
      } else if (a == 123 || a == 120) allOff();
      break;
  }
}
void midiByte(uint8_t c) {
  if (midiThru) Serial1.write(c);
  if (c >= 0xF8) return;                        // realtime
  if (c & 0x80) { mStatus = (c < 0xF0) ? c : 0; mCount = 0; return; }
  if (!mStatus) return;
  mData[mCount++] = c;
  uint8_t need = ((mStatus & 0xF0) == 0xC0 || (mStatus & 0xF0) == 0xD0) ? 1 : 2;
  if (mCount >= need) { midiHandle(mStatus, mData[0], need == 2 ? mData[1] : 0); mCount = 0; }
}

// ---- console ----
void status(Print &o) {
  o.print(F("mode ")); o.println(parked ? F("PARKED (Mega synth)") : F("unparked (stock PSS, relay)"));
  o.print(F("relay ")); o.println(relayOn ? F("on") : F("off (PSS looks powered down)"));
  o.print(F("prog ")); o.print(prog); o.print(F("  base ")); o.print(baseNote);
  o.print(F("  lock ")); o.print(lockTone ? F("on") : F("off")); o.print(F("  thru ")); o.println(midiThru ? F("on") : F("off"));
  uint8_t n = 0; for (uint8_t d = 0; d < NLEADS; d++) for (uint8_t s = 0; s < NLEADS; s++) n += keyNote[d][s] != 0xFF;
  o.print(F("boot mode ")); o.println(bootMega ? F("MEGA") : F("stock"));
  o.print(F("arp ")); o.print(ARP_NAMES[arpMode]); o.print(F("  ")); o.print(arpBpm); o.print(F(" BPM  octaves ")); o.print(arpOct); o.println(arpLatch ? F("  latch") : F(""));
  o.print(F("PSS sense ")); o.print(senseOn ? F("ON  A0=") : F("off  A0=")); o.print(analogRead(A0)); o.print(pssDead ? F("  PSS OFF") : F("")); o.println(standby ? F("  STANDBY") : F(""));
  o.print(F("midi channel ")); o.print(midiChan + 1); o.print(F("  transpose ")); o.println((int)transpose);
  o.print(F("learned keys ")); o.println(n);
  if (parked) { o.print(F("CPU /CS edges while parked: ")); o.println(parkedEdges); }
}
void command(char *line, Print &o) {
  if (line[0] == '#') return;                                  // ESP boot log
  for (char *p = line; *p; p++) if ((uint8_t)*p < 0x20 || (uint8_t)*p > 0x7E) return;   // ESP ROM noise
  char *arg = strchr(line, ' ');
  if (arg) *arg++ = 0;
  if (!strcmp(line, "status")) status(o);
  else if (!strcmp(line, "park")) doPark();
  else if (!strcmp(line, "unpark")) doUnpark();
  else if (!strcmp(line, "learn")) learnStart();
  else if (!strcmp(line, "done")) { if (learning) learnEnd(); }
  else if (!strcmp(line, "forget")) { memset(keyNote, 0xFF, sizeof keyNote); rebuildDrivers(); saveMap(); o.println(F("key map cleared")); }
  else if (!strcmp(line, "base") && arg) { baseNote = atoi(arg); EEPROM.update(EE_BASE, baseNote); o.println(F("ok")); }
  else if (!strcmp(line, "prog") && arg) { setProgram(atoi(arg)); o.println(F("ok")); }
  else if (!strcmp(line, "thru") && arg) { midiThru = !strcmp(arg, "on"); EEPROM.update(EE_THRU, midiThru); o.println(F("ok")); }
  else if (!strcmp(line, "keys") && arg) { printKeys = !strcmp(arg, "on"); o.println(F("ok")); }
  else if (!strcmp(line, "lock") && arg) {
    lockTone = !strcmp(arg, "on"); EEPROM.update(EE_LOCK, lockTone);
    if (!parked) { if (lockTone) { reinjectTone(); lockActive = true; } else lockActive = false; }
    o.println(F("ok"));
  }
  else if (!strcmp(line, "tone") && arg) {            // tone <8 hex bytes>: set AND save
    for (uint8_t i = 0; i < 8 && arg; i++) { userTone[i] = strtoul(arg, &arg, 16); }
    for (uint8_t i = 0; i < 8; i++) EEPROM.update(EE_TONE + i, userTone[i]);
    customTone = true; EEPROM.update(EE_CUSTOM, 1);
    if (parked) loadUserTone(); else if (lockTone) reinjectTone();
    oledDirty = true;
    o.println(F("ok"));
  }
  else if (!strcmp(line, "tw") && arg) {              // tw <8 hex bytes>: live change, not saved, silent (web panel sliders)
    for (uint8_t i = 0; i < 8 && arg; i++) { userTone[i] = strtoul(arg, &arg, 16); }
    customTone = true; oledDirty = true;
    if (parked) loadUserTone();
  }
  else if (!strcmp(line, "tonesave")) {
    for (uint8_t i = 0; i < 8; i++) EEPROM.update(EE_TONE + i, userTone[i]);
    customTone = true; EEPROM.update(EE_CUSTOM, 1);
    o.println(F("tone saved"));
  }
  else if (!strcmp(line, "gettone")) {                // gettone [n]: the 8 bytes of voice n (or the current tone)
    uint8_t b[8];
    if (arg && atoi(arg) >= 0 && atoi(arg) < 100) for (uint8_t i = 0; i < 8; i++) b[i] = pgm_read_byte(&PSS270_TONES[atoi(arg)].t[i]);
    else memcpy(b, userTone, 8);
    o.print(F("T"));
    for (uint8_t i = 0; i < 8; i++) { o.print(' '); if (b[i] < 16) o.print('0'); o.print(b[i], HEX); }
    o.println();
  }
  else if (!strcmp(line, "names")) {
    for (uint8_t i = 0; i < 100; i++) { wdt_reset(); if (i < 10) o.print('0'); o.print(i); o.print(' '); o.println(PSS270_VOICES[i].name); }
  }
  else if (!strcmp(line, "hold") && arg) { if (parked) { allOff(); arpClear(); synthNoteOn(atoi(arg), 100); } }
  else if (!strcmp(line, "release")) { allOff(); arpClear(); }
  else if (!strcmp(line, "reboot")) {
    // Jump straight into the bootloader so the ESP can flash a new sketch over WiFi.
    // (A watchdog reset would skip the bootloader on the Mega's stk500v2 bootloader.)
    if (parked) doUnpark();
    LOG.println(F("rebooting into bootloader"));
    Serial.flush(); Serial3.flush();
    wdt_disable();                       // the bootloader must not get reset mid-flash
    cli();
    EIMSK = 0; PCICR = 0; TIMSK0 = 0; TWCR = 0;
    UCSR0B = 0; UCSR1B = 0; UCSR3B = 0;
    MCUSR = 0;
    asm volatile("jmp 0x3E000");
  }
  else if (!strcmp(line, "slow") && arg) { ymSlow = !strcmp(arg, "on"); o.println(ymSlow ? F("slow writes on") : F("slow writes off")); }
  else if (!strcmp(line, "play") && arg) {
    // play <note> [ms]: one note through the same synth path the keys use (for measuring).
    if (!parked) { o.println(F("park first")); return; }
    uint8_t note = atoi(arg);
    char *sp = strchr(arg, ' ');
    uint16_t ms = sp ? atoi(sp + 1) : 600;
    if (ms > 3000) ms = 3000;
    synthNoteOn(note, 100);
    for (uint16_t t = 0; t < ms; t += 50) { wdt_reset(); delay(50); }
    synthNoteOff(note);
    o.println(F("ok"));
  }
  else if (!strcmp(line, "rep") && arg) {
    // rep <note> <count> <period_ms>: plays the same note on its own, no commands needed in between
    // (so the ESP can switch its radio off meanwhile).
    if (!parked) { o.println(F("park first")); return; }
    uint8_t note = atoi(arg);
    char *p1 = strchr(arg, ' '); uint8_t count = p1 ? atoi(p1 + 1) : 10;
    char *p2 = p1 ? strchr(p1 + 1, ' ') : nullptr; uint16_t per = p2 ? atoi(p2 + 1) : 1300;
    o.println(F("rep started"));
    for (uint8_t i = 0; i < count; i++) {
      unsigned long t = millis();
      synthNoteOn(note, 100);
      while (millis() - t < 600) { wdt_reset(); }
      synthNoteOff(note);
      while (millis() - t < per) { wdt_reset(); }
    }
    o.println(F("rep done"));
  }
  else if (!strcmp(line, "arp") && arg) {
    uint8_t m = 255;
    for (uint8_t i = 0; i < 6; i++) if (!strcmp(arg, ARP_NAMES[i]) || (i == 3 && !strcmp(arg, "updown")) || (i == 5 && !strcmp(arg, "played"))) m = i;
    if (m == 255) { o.println(F("arp off|up|down|updown|random|played")); return; }
    arpMode = m; EEPROM.update(EE_ARPMODE, arpMode); arpClear(); o.print(F("arp ")); o.println(ARP_NAMES[arpMode]);
  }
  else if (!strcmp(line, "bpm") && arg) { arpBpm = constrain(atoi(arg), 40, 240); EEPROM.update(EE_ARPBPM, arpBpm); o.print(F("arp tempo ")); o.println(arpBpm); }
  else if (!strcmp(line, "arpoct") && arg) { arpOct = constrain(atoi(arg), 1, 4); EEPROM.update(EE_ARPOCT, arpOct); arpClear(); o.print(F("arp octaves ")); o.println(arpOct); }
  else if (!strcmp(line, "latch") && arg) { arpLatch = !strcmp(arg, "on"); EEPROM.update(EE_ARPLATCH, arpLatch ? 1 : 0); arpClear(); o.println(arpLatch ? F("latch on") : F("latch off")); }
  else if (!strcmp(line, "sense") && arg) { senseOn = !strcmp(arg, "on"); EEPROM.update(EE_SENSE, senseOn ? 1 : 0); o.println(senseOn ? F("PSS power sense ON (needs the 5V wire on A0)") : F("PSS power sense off")); }
  else if (!strcmp(line, "standby") && arg) { standbyAfter = constrain(atoi(arg), 5, 250); EEPROM.update(EE_STANDBY, standbyAfter); o.print(F("standby after ")); o.print(standbyAfter); o.println(F(" s")); }
  else if (!strcmp(line, "menu") && arg) { if (!parked) { o.println(F("park first")); return; } menuOpen(!strcmp(arg, "on")); }
  else if (!strcmp(line, "boot") && arg) {
    bootMega = !strcmp(arg, "mega"); EEPROM.update(EE_BOOT, bootMega ? 1 : 0);
    o.println(bootMega ? F("boot mode: MEGA synth (parks itself after the PSS powers on)") : F("boot mode: STOCK PSS"));
  }
  else if (!strcmp(line, "scan boot") && arg) { scanEnabled = !strcmp(arg, "on"); o.println(scanEnabled ? F("key scan on") : F("key scan OFF")); }
  else if (!strcmp(line, "bittest") && arg) {
    // Four notes on channel 0 whose pitch numbers differ only in data bits <lo>,<lo+1>:
    // fnum = 256 + k*(1<<lo), k=0..3. Four clearly rising steps = both bits reach the chip.
    // "bittest 6" -> bits 6,7 (steps of 64); "bittest 4" -> bits 4,5; "bittest 2" -> bits 2,3.
    if (!parked) { o.println(F("park first")); return; }
    uint8_t lo = atoi(arg); if (lo > 6) return;
    for (uint8_t k = 0; k < 4; k++) {
      wdt_reset();
      uint16_t fn = 256 + ((uint16_t)k << lo);
      ymWrite(0x30, 0x02);
      ymWrite(0x20, (3 << 1) | ((fn >> 8) & 1));
      ymWrite(0x10, fn & 0xFF);
      ymWrite(0x20, 0x10 | (3 << 1) | ((fn >> 8) & 1));
      delay(800);
      ymWrite(0x20, (3 << 1) | ((fn >> 8) & 1));
      delay(250);
      o.print(F("fnum ")); o.println(fn);
    }
    o.println(F("bittest done"));
  }
  else if (!strcmp(line, "chscale") && arg) {
    // C major scale on ONE channel only (direct writes), to tell a bad channel from bad pitch maths.
    if (!parked) { o.println(F("park first")); return; }
    uint8_t v = atoi(arg); if (v > 8) return;
    const uint8_t sc[] = {60, 62, 64, 65, 67, 69, 71, 72};
    for (uint8_t i = 0; i < 8; i++) {
      wdt_reset();
      uint16_t fn; uint8_t bl; calcPitch(sc[i], fn, bl);
      ymWrite(0x30 + v, 0x02);
      ymWrite(0x20 + v, (bl << 1) | ((fn >> 8) & 1));
      ymWrite(0x10 + v, fn & 0xFF);
      ymWrite(0x20 + v, 0x10 | (bl << 1) | ((fn >> 8) & 1));
      delay(450);
      ymWrite(0x20 + v, (bl << 1) | ((fn >> 8) & 1));
      delay(100);
    }
    o.print(F("scale on channel ")); o.println(v);
  }
  else if (!strcmp(line, "scale")) {
    // C D E F G A B C through the synth path (same code the keys use), 500 ms each.
    if (!parked) { o.println(F("park first")); return; }
    const uint8_t sc[] = {60, 62, 64, 65, 67, 69, 71, 72};
    for (uint8_t i = 0; i < 8; i++) {
      wdt_reset();
      synthNoteOn(sc[i], 100); delay(400);
      synthNoteOff(sc[i]); delay(100);
      o.print(sc[i]); o.print(' ');
    }
    o.println(F("scale done"));
  }
  else if (!strcmp(line, "bustest")) {
    // Stock mode: sample D4-D12 for 3 s while the CPU plays. A pin that never changes isn't
    // connected to the YM bus (the CPU toggles A0 and every data line constantly).
    if (parked) { o.println(F("unpark first")); return; }
    o.println(F("bustest: play keys / chords now (10 s)"));
    const uint8_t pins[9] = {4, 5, 6, 7, 8, 9, 10, 11, 12};
    const char* names[9] = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "A0"};
    uint32_t hi[9] = {0}, total = 0;
    unsigned long t = millis();
    while (millis() - t < 10000) {
      for (uint8_t i = 0; i < 9; i++) if (digitalRead(pins[i])) hi[i]++;
      total++;
      if ((total & 1023) == 0) wdt_reset();
    }
    for (uint8_t i = 0; i < 9; i++) {
      o.print(F("Mega D")); o.print(pins[i]); o.print(F(" (YM ")); o.print(names[i]); o.print(F("): high "));
      o.print((uint8_t)(hi[i] * 100 / total)); o.print(F("%"));
      o.println((hi[i] == 0 || hi[i] == total) ? F("  <-- NEVER CHANGES") : F("  ok, toggles"));
    }
  }
  else if (!strcmp(line, "sniff")) {
    // Stock mode only: record 96 CPU writes (play some keys), then dump them.
    if (parked) { o.println(F("unpark first")); return; }
    sniffIdx = 0;
    o.println(F("sniffing: play some keys on the PSS now (10 s)"));
    unsigned long t = millis();
    while (sniffIdx < SNIFF_N && millis() - t < 10000) { wdt_reset(); delay(5); }
    uint8_t n = sniffIdx; sniffIdx = SNIFF_N;
    o.print(F("captured ")); o.println(n);
    for (uint8_t i = 0; i < n; i++) {
      o.print(sniffA0[i] ? F("D ") : F("A ")); 
      if (sniffData[i] < 16) o.print('0');
      o.print(sniffData[i], HEX);
      o.print((i % 8 == 7) ? '\n' : ' ');
    }
    o.println();
  }
  else if (!strcmp(line, "chantest")) {
    // Same note (middle C) on channels 0..8 in turn. All 9 must sound identical.
    if (!parked) { o.println(F("park first")); return; }
    uint16_t fn; uint8_t bl; calcPitch(60, fn, bl);
    for (uint8_t v = 0; v < 9; v++) {
      wdt_reset();
      o.print(F("channel ")); o.println(v);
      ymWrite(0x30 + v, 0x02);
      ymWrite(0x20 + v, (bl << 1) | ((fn >> 8) & 1));
      ymWrite(0x10 + v, fn & 0xFF);
      ymWrite(0x20 + v, 0x10 | (bl << 1) | ((fn >> 8) & 1));
      delay(900);
      ymWrite(0x20 + v, (bl << 1) | ((fn >> 8) & 1));
      delay(700);
    }
    o.println(F("chantest done"));
  }
  else if (!strcmp(line, "mx")) {
    // Matrix diagnostic (parked only): idle levels, then which lead pulls which in both polarities.
    if (!parked) { o.println(F("park first")); return; }
    wdt_reset();
    o.print(F("idle, pull-ups on, reads LOW:  "));
    for (uint8_t i = 0; i < NLEADS; i++) pinMode(LEADS[i], INPUT_PULLUP);
    delay(2);
    for (uint8_t i = 0; i < NLEADS; i++) if (!digitalRead(LEADS[i])) { o.print(LEADS[i]); o.print(' '); }
    o.println();
    o.print(F("idle, no pull-ups, reads HIGH: "));
    for (uint8_t i = 0; i < NLEADS; i++) pinMode(LEADS[i], INPUT);
    delay(2);
    for (uint8_t i = 0; i < NLEADS; i++) if (digitalRead(LEADS[i])) { o.print(LEADS[i]); o.print(' '); }
    o.println();
    o.println(F("drive LOW (others pulled up) -> reads LOW:"));
    for (uint8_t d = 0; d < NLEADS; d++) {
      for (uint8_t i = 0; i < NLEADS; i++) pinMode(LEADS[i], INPUT_PULLUP);
      pinMode(LEADS[d], OUTPUT); digitalWrite(LEADS[d], LOW); delayMicroseconds(50);
      bool any = false;
      for (uint8_t s2 = 0; s2 < NLEADS; s2++) if (s2 != d && !digitalRead(LEADS[s2])) {
        if (!any) { o.print(F("  ")); o.print(LEADS[d]); o.print(F(" ->")); any = true; }
        o.print(' '); o.print(LEADS[s2]);
      }
      if (any) o.println();
      pinMode(LEADS[d], INPUT_PULLUP);
    }
    o.println(F("drive HIGH (others no pull-up) -> reads HIGH:"));
    for (uint8_t d = 0; d < NLEADS; d++) {
      for (uint8_t i = 0; i < NLEADS; i++) pinMode(LEADS[i], INPUT);
      pinMode(LEADS[d], OUTPUT); digitalWrite(LEADS[d], HIGH); delayMicroseconds(50);
      bool any = false;
      for (uint8_t s2 = 0; s2 < NLEADS; s2++) if (s2 != d && digitalRead(LEADS[s2])) {
        if (!any) { o.print(F("  ")); o.print(LEADS[d]); o.print(F(" ->")); any = true; }
        o.print(' '); o.print(LEADS[s2]);
      }
      if (any) o.println();
      pinMode(LEADS[d], INPUT);
    }
    for (uint8_t i = 0; i < NLEADS; i++) pinMode(LEADS[i], INPUT_PULLUP);
    o.println(F("mx done"));
  }
  else if (!strcmp(line, "oled")) {
    // I2C scan, then re-init the display (a screen that lost power keeps its old blank state).
    uint8_t found = 0;
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) { o.print(F("i2c device at 0x")); o.println(a, HEX); found++; }
    }
    if (!found) o.println(F("i2c: nothing answers (check OLED power, SDA=D20, SCL=D21)"));
    oledOk = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C) || oled.begin(SSD1306_SWITCHCAPVCC, 0x3D);
    oledDirty = true;
    o.println(oledOk ? F("oled ok") : F("oled not found"));
  }
  else if (!strcmp(line, "miditest")) {
    // Loopback self-test: patch a MIDI cable from the shield's OUT to its IN first.
    while (Serial1.available()) Serial1.read();
    const uint8_t msg[3] = {0x9F, 0x3C, 0x40};      // note on, channel 16 (ignored by the synth)
    Serial1.write(msg, 3);
    uint8_t got[3]; uint8_t n = 0; unsigned long t = millis();
    while (n < 3 && millis() - t < 100) if (Serial1.available()) got[n++] = Serial1.read();
    const uint8_t off[3] = {0x8F, 0x3C, 0x00}; Serial1.write(off, 3);
    if (n == 3 && !memcmp(got, msg, 3)) o.println(F("MIDI loopback OK: OUT -> cable -> IN works"));
    else if (n == 0) o.println(F("MIDI loopback: nothing came back (cable OUT->IN in? shield switch ON? TX1/RX1 wires?)"));
    else { o.print(F("MIDI loopback: garbled, got ")); o.print(n); o.println(F(" bytes")); }
    delay(20); while (Serial1.available()) Serial1.read();
  }
  else if (!strcmp(line, "test")) {
    if (!parked) { o.println(F("park first")); return; }
    const uint8_t notes[] = {60, 64, 67, 72};
    for (uint8_t i = 0; i < 4; i++) { synthNoteOn(notes[i], 110); delay(250); }
    delay(500); allOff();
  }
  else o.println(F("? status park unpark learn done forget base prog lock tone tw tonesave gettone names hold release thru keys test reboot miditest oled mx chantest sniff bustest scale chscale bittest play rep scan"));
}
LineBuf usbLine, espLine;
void feed(Stream &s, LineBuf &L) {
  while (s.available()) {
    char c = s.read();
    if (c == '\r') continue;
    if (c == '\n') { L.b[L.n] = 0; if (L.n) command(L.b, LOG); L.n = 0; }
    else if (L.n < sizeof(L.b) - 1) L.b[L.n++] = c;
  }
}

// ---- PSS power detection ----
// The CPU's /CS idles HIGH while the PSS is on. Steady LOW for 50 ms = PSS off: stop driving
// the chip's /CS so the Mega doesn't feed power into an unpowered board.
void powerTick() {
  static unsigned long lowSince = 0, highSince = 0;
  if (parked) { lowSince = highSince = 0; return; }   // CPU in reset: its /CS says nothing about power
  bool hi = digitalRead(PIN_CS_CPU);
  unsigned long now = millis();
  if (hi) { lowSince = 0; if (!highSince) highSince = now; } else { highSince = 0; if (!lowSince) lowSince = now; }
  if (relayJustOn) {
    relayJustOn = false;
    LOG.println(F("PSS on: relay running"));
    oledDirty = true;
    if (lockTone) { lockPending = true; lockAt = now + 30; }
    if (bootMega) parkAt = now + 400;
  }
  if (relayOn && lowSince && now - lowSince > 50) {
    pinMode(PIN_CS, INPUT);
    relayOn = false;
    LOG.println(F("PSS looks off: relay stopped"));
    oledDirty = true;
  } else if (!relayOn && highSince && now - highSince > 50) {
    digitalWrite(PIN_CS, HIGH);
    pinMode(PIN_CS, OUTPUT);
    relayOn = true;
    LOG.println(F("PSS on: relay running"));
    oledDirty = true;
    if (lockTone) { lockPending = true; lockAt = now + 30; }
    if (bootMega) parkAt = now + 400;
  }
}

void oledDraw() {
  if (!oledOk || !oledDirty) return;
  oledDirty = false;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  if (menuMode) {
    oled.print(F("MENU  ")); oled.print(menuItem + 1); oled.print(F("/")); oled.print(MENU_ITEMS);
    if (menuEdit) oled.print(F("  EDIT"));
    oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);
    oled.setCursor(0, 16); oled.setTextSize(2); oled.print(MENU_NAMES[menuItem]);
    oled.setTextSize(1); oled.setCursor(0, 38);
    char v[28]; menuValue(v, sizeof v); oled.print(v);
    oled.setCursor(0, 54);
    oled.print(menuEdit ? F("C -   D done   E +") : F("C <   D select  E >"));
    oled.display();
    return;
  }
  oled.print(F("PSS-270  "));
  oled.print(learning ? F("LEARN") : parked ? F("MEGA SYNTH") : relayOn ? F("STOCK") : F("PSS OFF"));
  oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);
  oled.setCursor(0, 16);
  oled.setTextSize(2);
  if (prog < 100) { if (prog < 10) oled.print('0'); oled.print(prog); }
  else { oled.print(F("R")); oled.print(prog - 99); }
  oled.setTextSize(1);
  oled.setCursor(0, 36);
  if (customTone) oled.print(F("CUSTOM TONE")); else if (prog < 100) oled.print(PSS270_VOICES[prog].name); else oled.print(F("YM2413 ROM voice"));
  oled.setCursor(0, 48);
  oled.print(oledLast);
  oled.setCursor(0, 57);
  uint8_t n = 0; for (uint8_t d = 0; d < NLEADS; d++) for (uint8_t s2 = 0; s2 < NLEADS; s2++) n += keyNote[d][s2] != 0xFF;
  oled.print(F("keys ")); oled.print(n);
  if (lockTone) oled.print(F("  LOCK"));
  if (midiThru) oled.print(F("  THRU"));
  oled.display();
}

void setup() {
  MCUSR = 0; wdt_disable();
  busIdle();
  pinMode(PIN_CS, INPUT);          // the chip's own 47k pull-up keeps it deselected until the PSS is seen
  pinMode(PIN_CS_CPU, INPUT);
  pinMode(PIN_IC, INPUT);
  pinMode(PIN_HOLD, INPUT);
  for (uint8_t i = 0; i < NLEADS; i++) pinMode(LEADS[i], INPUT);

  Serial.begin(115200);
  Serial1.begin(31250);
  Wire.begin();
  Wire.setClock(400000);
  Wire.setWireTimeout(5000, true);     // a stuck I2C bus (noise at PSS power-up) must never hang the Mega
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C) || oled.begin(SSD1306_SWITCHCAPVCC, 0x3D);
  Serial3.begin(115200);

  if (EEPROM.read(0) != EE_MAGIC) {
    EEPROM.update(0, EE_MAGIC); EEPROM.update(EE_BASE, 36); EEPROM.update(EE_LOCK, 0);
    EEPROM.update(EE_PROG, 0); EEPROM.update(EE_THRU, 0); EEPROM.update(EE_CHAN, 0); EEPROM.update(EE_TRANS, 0);
    for (uint8_t i = 0; i < 8; i++) EEPROM.update(EE_TONE + i, pgm_read_byte(&PSS270_TONES[0].t[i]));
    for (uint16_t i = 0; i < NLEADS * NLEADS; i++) EEPROM.update(EE_MAP + i, 0xFF);
  }
  baseNote = EEPROM.read(EE_BASE);
  lockTone = EEPROM.read(EE_LOCK);
  bootMega = EEPROM.read(EE_BOOT) == 1;
  if (EEPROM.read(300) != 0x5A) {      // first run with the menu: write sane defaults (these bytes were unused before)
    EEPROM.update(EE_CHAN, 0); EEPROM.update(EE_TRANS, 0); EEPROM.update(300, 0x5A);
  }
  if (EEPROM.read(301) != 0x5B) {      // arp / sense settings were unused bytes before: write defaults once
    EEPROM.update(EE_ARPMODE, 0); EEPROM.update(EE_ARPBPM, 120); EEPROM.update(EE_ARPOCT, 1);
    EEPROM.update(EE_ARPLATCH, 0); EEPROM.update(EE_SENSE, 0); EEPROM.update(EE_STANDBY, 20); EEPROM.update(301, 0x5B);
  }
  arpMode = EEPROM.read(EE_ARPMODE) % 6; arpBpm = constrain(EEPROM.read(EE_ARPBPM), 40, 240);
  arpOct = constrain(EEPROM.read(EE_ARPOCT), 1, 4); arpLatch = EEPROM.read(EE_ARPLATCH) == 1;
  senseOn = EEPROM.read(EE_SENSE) == 1; standbyAfter = constrain(EEPROM.read(EE_STANDBY), 5, 250);
  midiChan = EEPROM.read(EE_CHAN) & 15;
  transpose = (int8_t)EEPROM.read(EE_TRANS); if (transpose < -24 || transpose > 24) transpose = 0;
  midiThru = EEPROM.read(EE_THRU);
  prog = EEPROM.read(EE_PROG); if (prog > 114) prog = 0;
  for (uint8_t i = 0; i < 8; i++) userTone[i] = EEPROM.read(EE_TONE + i);
  customTone = EEPROM.read(EE_CUSTOM) == 1;
  if (!customTone && prog < 100) for (uint8_t i = 0; i < 8; i++) userTone[i] = pgm_read_byte(&PSS270_TONES[prog].t[i]);   // the saved voice number must also bring back its tone
  for (uint8_t d = 0; d < NLEADS; d++) for (uint8_t s = 0; s < NLEADS; s++) keyNote[d][s] = EEPROM.read(EE_MAP + d * NLEADS + s);
  rebuildDrivers();

  EICRB = (EICRB & ~(_BV(ISC41) | _BV(ISC40))) | _BV(ISC40);   // INT4 on any edge
  EIFR = _BV(INTF4);
  EIMSK |= _BV(INT4);                  // always armed: the first CPU /CS edge starts the relay
  wdt_enable(WDTO_4S);                   // a hang (I2C, anything) restarts the Mega: the reset button is inside the PSS
  LOG.println(F("pss270_mega ready. 'status' for state, '?' for commands"));
}

void loop() {
  wdt_reset();
  powerTick();
  if (parkAt && (long)(millis() - parkAt) >= 0) {
    parkAt = 0;
    if (bootMega && relayOn && !parked) doPark();    // boot mode: Mega synth
  }
  static unsigned long lastDraw = 0, lastOledTry = 0;
  if (!oledOk && !parked && millis() - lastOledTry > 5000) {
    lastOledTry = millis();
    Wire.beginTransmission(0x3C);
    if (Wire.endTransmission() == 0) { oledOk = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C); oledDirty = true; }
  }
  if (oledDirty && millis() - lastDraw > 100) { lastDraw = millis(); oledDraw(); }
  if (lockPending && !parked && (long)(millis() - lockAt) >= 0) {
    lockPending = false;
    reinjectTone();
    lockActive = true;
  }
  feed(Serial, usbLine);
  feed(Serial3, espLine);
  while (Serial1.available()) midiByte(Serial1.read());

  static unsigned long lastScan = 0;
  if (parked && scanEnabled && !standby && millis() - lastScan >= 2) {
    lastScan = millis();
    if (learning) learnTick(); else scanTick();
  }
  comboTick();
  arpTick();
  senseTick();
}
