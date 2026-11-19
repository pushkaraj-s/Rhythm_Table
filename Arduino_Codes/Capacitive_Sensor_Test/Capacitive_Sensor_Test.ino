/*
  MPR121ShieldTest.ino
  Full functional test for the Adafruit 12 x Capacitive Touch Shield (MPR121, #2024)
  on an Arduino Uno R3.

  What it does
    1. Scans the I2C bus and finds the MPR121 (0x5A default, 0x5B/0x5C/0x5D possible).
    2. Starts the chip with Adafruit's defaults and runs the MPR121 auto configuration.
    3. Prints a full report: every config register decoded, plus a table for all
       12 pads (charge current, charge time, filtered data, baseline, delta,
       estimated capacitance, out of range flag).
    4. Prints a line every time any pad is TOUCHED or RELEASED, with the raw
       numbers at that moment and how long it was held.
    5. Keeps a checklist of pads you have touched and announces when all 12 pass.

  Serial Monitor: 115200 baud. Type a letter and press Enter:
    h = help                 r = full report (registers + 12 pad table)
    d = live data on/off     t = test progress      x = reset test progress
    c = recalibrate (hands off the pads!)
    + = less sensitive       - = more sensitive (changes touch threshold)

  Shield facts (from Adafruit's schematic):
    * Uses only I2C: the SDA/SCL pins next to AREF (same as A4/A5 on an Uno).
    * MPR121 runs at 3.3 V from an onboard regulator, with I2C level shifting.
    * Newer boards (Dec 2022+) power the I2C pull ups from the IOREF pin,
      which every Uno R3 has.
    * IRQ and ADDR are broken out as pads but NOT wired to any Arduino pin,
      so this sketch polls the chip over I2C. The small red LED sits on the
      IRQ line and flashes briefly on each touch or release.
    * Pads are labelled 0 to 11 on the shield; pad n = MPR121 electrode ELEn.

  Library: "Adafruit MPR121" from Library Manager (accept "Adafruit BusIO").
*/

#include <Wire.h>
#include <Adafruit_MPR121.h>

// ---------- Settings ----------
const uint8_t  NUM_PADS         = 12;
const uint8_t  TOUCH_THRESHOLD  = 12;   // Adafruit default (counts)
const uint8_t  RELEASE_THRESHOLD = 6;   // Adafruit default (counts)
const float    VDD              = 3.3;  // MPR121 supply on the shield
const uint16_t POLL_MS          = 10;   // how often we read touch status
const uint16_t STREAM_MS        = 250;  // live data print interval

// ---------- MPR121 registers not named in the library ----------
#define REG_OOR_STATUS   0x02   // 0x02/0x03: out of range + auto config fail
#define REG_DEBOUNCE     0x5B
#define REG_CONFIG1      0x5C   // FFI, global CDC
#define REG_CONFIG2      0x5D   // global CDT, SFI, ESI
#define REG_ECR          0x5E   // electrode config (run/stop)
#define REG_CDC0         0x5F   // per pad charge current, 0x5F..0x6A
#define REG_CDT01        0x6C   // per pad charge time, 2 pads per register
#define REG_AUTOCFG0     0x7B
#define REG_AUTOCFG1     0x7C
#define REG_USL          0x7D
#define REG_LSL          0x7E
#define REG_TL           0x7F

Adafruit_MPR121 cap = Adafruit_MPR121();

uint8_t  i2cAddr = 0;
uint8_t  touchTh = TOUCH_THRESHOLD;
uint8_t  releaseTh = RELEASE_THRESHOLD;
uint16_t lastTouched = 0;
uint16_t testedMask = 0;          // pads touched at least once
uint16_t touchCount[NUM_PADS];
uint32_t touchStart[NUM_PADS];
bool     allPassedShown = false;
bool     streaming = false;
uint32_t lastPoll = 0, lastStream = 0;

// ---------- Small print helpers ----------
void pad(long v, uint8_t width) {          // right aligned integer
  char buf[12];
  ltoa(v, buf, 10);
  for (int8_t i = (int8_t)width - (int8_t)strlen(buf); i > 0; i--) Serial.print(' ');
  Serial.print(buf);
}

void hex2(uint8_t v) {
  Serial.print(F("0x"));
  if (v < 0x10) Serial.print('0');
  Serial.print(v, HEX);
}

void line() { Serial.println(F("------------------------------------------------------------------")); }

void timestamp() {
  uint32_t ms = millis();
  Serial.print('[');
  pad(ms / 1000, 5);
  Serial.print('.');
  uint16_t f = ms % 1000;
  if (f < 100) Serial.print('0');
  if (f < 10)  Serial.print('0');
  Serial.print(f);
  Serial.print(F(" s] "));
}

// ---------- Decoders (MPR121 datasheet section 5) ----------
float cdtMicros(uint8_t code) {            // 0 = off, 1..7 = 0.5 us .. 32 us
  return code ? 0.5 * (1 << (code - 1)) : 0.0;
}

uint8_t padCDC(uint8_t i) {                // per pad current, 0 = use global
  return cap.readRegister8(REG_CDC0 + i) & 0x3F;
}

uint8_t padCDT(uint8_t i) {                // per pad time code, 0 = use global
  uint8_t v = cap.readRegister8(REG_CDT01 + i / 2);
  return (i & 1) ? (v >> 4) & 0x07 : v & 0x07;
}

int16_t padDelta(uint8_t i) {              // positive when touched
  return (int16_t)cap.baselineData(i) - (int16_t)cap.filteredData(i);
}

// ---------- I2C scan ----------
uint8_t scanI2C() {
  uint8_t found = 0, mpr = 0;
  Serial.println(F("Scanning I2C bus..."));
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  device at "));
      hex2(a);
      if (a >= 0x5A && a <= 0x5D) {
        Serial.print(F("  <- MPR121 address range"));
        if (!mpr) mpr = a;
      }
      Serial.println();
      found++;
    }
  }
  if (!found) Serial.println(F("  no I2C devices found"));
  return mpr;
}

// ---------- Start / recalibrate ----------
bool startSensor() {
  Serial.println(F("Calibrating. KEEP YOUR HANDS OFF THE PADS..."));
  delay(500);
  // Soft reset + Adafruit defaults + auto configuration (the chip picks the
  // charge current/time for each pad when it enters run mode).
  if (!cap.begin(i2cAddr, &Wire, touchTh, releaseTh, true)) return false;
  delay(100);                              // let auto config + baseline settle
  lastTouched = cap.touched();
  return true;
}

// Change thresholds with a single stop/run cycle (the library's writeRegister
// would otherwise stop and restart the chip for every one of the 24 writes).
void applyThresholds() {
  uint8_t ecr = cap.readRegister8(REG_ECR);
  cap.writeRegister(REG_ECR, 0x00);        // stop mode
  cap.setThresholds(touchTh, releaseTh);
  cap.writeRegister(REG_ECR, ecr);         // back to run mode
}

// ---------- Reports ----------
void printRegisters() {
  uint8_t c1  = cap.readRegister8(REG_CONFIG1);
  uint8_t c2  = cap.readRegister8(REG_CONFIG2);
  uint8_t ecr = cap.readRegister8(REG_ECR);
  uint8_t deb = cap.readRegister8(REG_DEBOUNCE);
  uint8_t ac0 = cap.readRegister8(REG_AUTOCFG0);
  static const uint8_t ffi[] = {6, 10, 18, 34};
  static const uint8_t sfi[] = {4, 6, 10, 18};

  line();
  Serial.println(F("MPR121 CHIP DETAILS"));
  line();
  Serial.print(F("I2C address ............ ")); hex2(i2cAddr);
  Serial.println(i2cAddr == 0x5A ? F(" (default, ADDR pad unconnected)") : F(" (ADDR pad strapped)"));
  Serial.print(F("Supply (VDD) ........... ")); Serial.print(VDD, 1); Serial.println(F(" V (onboard regulator)"));

  Serial.print(F("CONFIG1 0x5C = ")); hex2(c1);
  Serial.print(F("   first filter: ")); Serial.print(ffi[c1 >> 6]);
  Serial.print(F(" samples, global charge current: ")); Serial.print(c1 & 0x3F); Serial.println(F(" uA"));

  Serial.print(F("CONFIG2 0x5D = ")); hex2(c2);
  Serial.print(F("   charge time: ")); Serial.print(cdtMicros(c2 >> 5), 1);
  Serial.print(F(" us, second filter: ")); Serial.print(sfi[(c2 >> 3) & 3]);
  Serial.print(F(" samples, sample period: ")); Serial.print(1 << (c2 & 7)); Serial.println(F(" ms"));

  uint8_t ele = ecr & 0x0F;
  Serial.print(F("ECR     0x5E = ")); hex2(ecr);
  Serial.print(F("   electrodes enabled: ")); Serial.print(ele >= 12 ? 12 : ele);
  Serial.print(F(", proximity: ")); Serial.print((ecr >> 4) & 3 ? F("on") : F("off"));
  Serial.print(F(", baseline tracking mode CL=")); Serial.println(ecr >> 6);
  Serial.println(ele ? F("                        chip is in RUN mode") : F("                        chip is in STOP mode!"));

  Serial.print(F("DEBOUNCE 0x5B = ")); hex2(deb);
  Serial.print(F("  touch: ")); Serial.print(deb & 7);
  Serial.print(F(", release: ")); Serial.print((deb >> 4) & 7); Serial.println(F(" extra samples"));

  Serial.print(F("Thresholds ............. touch ")); Serial.print(cap.readRegister8(0x41));
  Serial.print(F(" / release ")); Serial.print(cap.readRegister8(0x42));
  Serial.println(F(" counts (same on all 12 pads)"));

  Serial.print(F("AUTOCONFIG0 0x7B = ")); hex2(ac0);
  Serial.print(F("  auto config: ")); Serial.print(ac0 & 1 ? F("on") : F("off"));
  Serial.print(F(", auto reconfig: ")); Serial.println(ac0 & 2 ? F("on") : F("off"));
  Serial.print(F("Auto config limits ..... upper ")); Serial.print(cap.readRegister8(REG_USL) * 4);
  Serial.print(F(", target ")); Serial.print(cap.readRegister8(REG_TL) * 4);
  Serial.print(F(", lower ")); Serial.print(cap.readRegister8(REG_LSL) * 4);
  Serial.println(F(" counts"));

  uint16_t oor = cap.readRegister16(REG_OOR_STATUS);
  uint16_t ts  = cap.readRegister16(0x00);
  Serial.print(F("Auto config result ..... "));
  if (oor & 0x8000)      Serial.println(F("FAILED (ACFF set)"));
  else if (oor & 0x4000) Serial.println(F("reconfig FAILED (ARFF set)"));
  else                   Serial.println(F("OK"));
  Serial.print(F("Over current (OVCF) .... "));
  Serial.println(ts & 0x8000 ? F("YES, check REXT / shorts!") : F("no"));
}

void printPadTable() {
  uint8_t gCDC = cap.readRegister8(REG_CONFIG1) & 0x3F;
  uint8_t gCDT = cap.readRegister8(REG_CONFIG2) >> 5;
  uint16_t oor = cap.readRegister16(REG_OOR_STATUS);
  uint16_t now = cap.touched();

  line();
  Serial.println(F("PAD  CDC(uA)  CDT(us)  FILTERED  BASELINE  DELTA  ~CAP(pF)  STATUS"));
  line();
  for (uint8_t i = 0; i < NUM_PADS; i++) {
    uint8_t  cdc  = padCDC(i);  if (!cdc) cdc = gCDC;
    uint8_t  cdtc = padCDT(i);  if (!cdtc) cdtc = gCDT;
    float    cdt  = cdtMicros(cdtc);
    uint16_t filt = cap.filteredData(i);
    uint16_t base = cap.baselineData(i);
    int16_t  dlt  = (int16_t)base - (int16_t)filt;

    pad(i, 3);
    pad(cdc, 9);
    Serial.print(F("    ")); Serial.print(cdt, 1); if (cdt < 10) Serial.print(' ');
    pad(filt, 9);
    pad(base, 10);
    pad(dlt, 7);
    Serial.print(F("    "));
    // Datasheet eq. 4: C = I * T / V, with V = ADC * VDD / 1024
    if (filt) Serial.print(cdc * cdt * 1024.0 / (filt * VDD), 1); else Serial.print(F(" n/a"));
    Serial.print(F("     "));
    if (oor & (1 << i))         Serial.println(F("OUT OF RANGE"));
    else if (now & (1 << i))    Serial.println(F("TOUCHED"));
    else                        Serial.println(F("ok"));
  }
  line();
  Serial.println(F("DELTA = baseline - filtered. Touch is detected when DELTA > touch threshold."));
}

void printStream() {
  Serial.print(F("delta:"));
  for (uint8_t i = 0; i < NUM_PADS; i++) { pad(padDelta(i), 5); }
  Serial.print(F("   touched=0x"));
  Serial.println(cap.touched(), HEX);
}

void printProgress() {
  uint8_t n = 0;
  Serial.print(F("Test progress: "));
  for (uint8_t i = 0; i < NUM_PADS; i++) {
    bool ok = testedMask & (1 << i);
    if (ok) n++;
    Serial.print(ok ? F("[") : F(" "));
    Serial.print(i);
    Serial.print(ok ? F("] ") : F("  "));
  }
  Serial.print(F("  ")); Serial.print(n); Serial.println(F("/12 passed"));
  for (uint8_t i = 0; i < NUM_PADS; i++) {
    if (touchCount[i]) {
      Serial.print(F("  pad ")); pad(i, 2); Serial.print(F(" touched "));
      Serial.print(touchCount[i]); Serial.println(F(" times"));
    }
  }
}

void printHelp() {
  Serial.println(F("Commands: h=help  r=report  d=live data on/off  t=progress  x=reset progress"));
  Serial.println(F("          c=recalibrate  +=less sensitive  -=more sensitive"));
}

void resetProgress() {
  testedMask = 0;
  allPassedShown = false;
  for (uint8_t i = 0; i < NUM_PADS; i++) touchCount[i] = 0;
}

// ---------- Touch events ----------
void handleTouches() {
  uint16_t raw = cap.readRegister16(0x00);
  if (raw & 0x8000) {
    timestamp(); Serial.println(F("WARNING: over current flag set, chip stopped. Press c."));
  }
  uint16_t now = raw & 0x0FFF;
  if (now == lastTouched) return;

  for (uint8_t i = 0; i < NUM_PADS; i++) {
    uint16_t bit = 1 << i;
    if ((now & bit) && !(lastTouched & bit)) {
      touchStart[i] = millis();
      touchCount[i]++;
      bool firstTime = !(testedMask & bit);
      testedMask |= bit;
      timestamp();
      Serial.print(F("TOUCH    pad ")); pad(i, 2);
      Serial.print(F("   filtered ")); Serial.print(cap.filteredData(i));
      Serial.print(F("  baseline ")); Serial.print(cap.baselineData(i));
      Serial.print(F("  delta ")); Serial.print(padDelta(i));
      if (firstTime) Serial.print(F("   <- first touch, PASS"));
      Serial.println();
    }
    if (!(now & bit) && (lastTouched & bit)) {
      uint32_t held = millis() - touchStart[i];
      timestamp();
      Serial.print(F("RELEASE  pad ")); pad(i, 2);
      Serial.print(F("   held ")); Serial.print(held); Serial.print(F(" ms"));
      Serial.print(F("   (touch #")); Serial.print(touchCount[i]); Serial.println(')');
    }
  }

  // Which pads are down right now (multi touch is supported)
  Serial.print(F("            pads down now: "));
  if (!now) Serial.print(F("none"));
  for (uint8_t i = 0; i < NUM_PADS; i++) if (now & (1 << i)) { Serial.print(i); Serial.print(' '); }
  Serial.println();

  lastTouched = now;

  if (testedMask == 0x0FFF && !allPassedShown) {
    allPassedShown = true;
    line();
    Serial.println(F("*** ALL 12 PADS PASSED: every electrode detected a touch ***"));
    line();
    printProgress();
  }
}

void handleCommand(char c) {
  switch (c) {
    case 'h': case '?': printHelp(); break;
    case 'r': printRegisters(); printPadTable(); break;
    case 'd':
      streaming = !streaming;
      Serial.println(streaming ? F("Live data ON (pads 0..11, delta counts)") : F("Live data OFF"));
      break;
    case 't': printProgress(); break;
    case 'x': resetProgress(); Serial.println(F("Test progress cleared.")); break;
    case 'c':
      if (startSensor()) { Serial.println(F("Recalibrated.")); printPadTable(); }
      else Serial.println(F("Recalibration failed, MPR121 not responding."));
      break;
    case '+': case '-':
      if (c == '+' && touchTh < 60) touchTh += 2;
      if (c == '-' && touchTh > 4)  touchTh -= 2;
      releaseTh = touchTh / 2;
      applyThresholds();
      Serial.print(F("Thresholds now: touch ")); Serial.print(touchTh);
      Serial.print(F(" / release ")); Serial.print(releaseTh);
      Serial.println(c == '+' ? F("  (less sensitive)") : F("  (more sensitive)"));
      break;
  }
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }          // needed on Leonardo, harmless on Uno
  Serial.println();
  line();
  Serial.println(F("Adafruit 12 x Capacitive Touch Shield (MPR121) test"));
  line();

  Wire.begin();
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(25000, true);        // don't hang if the bus is stuck
#endif

  i2cAddr = scanI2C();
  if (!i2cAddr) {
    Serial.println(F("MPR121 NOT FOUND. Check:"));
    Serial.println(F("  * all header pins soldered, especially SDA/SCL next to AREF and IOREF"));
    Serial.println(F("  * shield fully seated on the Uno"));
    Serial.println(F("  * board without an IOREF pin? bridge the shield's pull up jumper to 5V"));
    while (true) delay(10);
  }

  if (!startSensor()) {
    Serial.println(F("Device answered but did not look like an MPR121 (CONFIG2 != 0x24)."));
    while (true) delay(10);
  }
  Serial.println(F("MPR121 found and running."));

  resetProgress();
  printRegisters();
  printPadTable();
  printHelp();
  Serial.println(F("Touch each pad 0..11 once. Every touch and release is printed below."));
  line();
}

void loop() {
  uint32_t ms = millis();

  if (ms - lastPoll >= POLL_MS) {
    lastPoll = ms;
    handleTouches();
  }

  if (streaming && ms - lastStream >= STREAM_MS) {
    lastStream = ms;
    printStream();
  }

  if (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') handleCommand(c);
  }
}
