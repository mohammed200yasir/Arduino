/*
 * Water Heating & Monitoring System
 * Hardware: Arduino Mega, LCD 16x2 I2C, DS18B20, HC-SR04, Turbidity Sensor,
 *           4x4 Keypad, 4-Channel Relay, Buzzer, Heater, 2x Water Pumps
 * Tank Height: 27 cm
 *
 * Relay wiring note:
 *   Default: ACTIVE-LOW relay module (most common 4-channel boards).
 *   If your module is ACTIVE-HIGH, swap: RELAY_ON = HIGH, RELAY_OFF = LOW.
 */

#include <Wire.h>
#include <LCD_I2C.h>
#include <Keypad.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ───────────────────────── LCD ─────────────────────────
LCD_I2C lcd(0x27, 16, 2);

// ─────────────────────── Keypad ────────────────────────
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte rowPins[ROWS] = {9, 8, 7, 6};
byte colPins[COLS]  = {5, 4, 3, 2};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ──────────────────────── Pins ─────────────────────────
#define TRIG_PIN        37
#define ECHO_PIN        36
#define INLET_RELAY     31   // pump 1: fills water
#define OUTLET_RELAY    30   // pump 2: drains water
#define BUZZER_PIN      48
#define TURBIDITY_PIN   A0
#define ONE_WIRE_BUS    10
#define TEMP_RELAY      11   // heater

// Active-LOW relay module (common 4-channel boards): LOW = relay ON, HIGH = relay OFF
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// ────────────────────── Sensors ────────────────────────
OneWire            oneWire(ONE_WIRE_BUS);
DallasTemperature  ds18b20(&oneWire);

// ─────────────────────── Config ────────────────────────
#define TANK_HEIGHT_CM      27.0
#define TURBIDITY_LIMIT     50.0   // fixed threshold: cleaning starts above 50%
#define DRAIN_STOP_PCT      5.0    // consider tank empty when water drops to 5%
#define LEVEL_TOLERANCE     3.0    // pump starts when diff exceeds ±3%
#define LEVEL_HYSTERESIS    1.0    // pump stops when within ±1% of target
#define SENSOR_READ_MS      700
#define DISPLAY_UPDATE_MS   500

// ──────────────────────── State ────────────────────────
enum Mode       { MODE_MAIN, MODE_WATER_INPUT, MODE_TEMP_INPUT, MODE_TURBIDITY_MONITOR };
enum CleanState { CLEAN_IDLE, CLEAN_DRAIN, CLEAN_FILL };
enum PumpState  { PUMP_IDLE, PUMP_FILLING, PUMP_DRAINING };   // tracks active pump

Mode           currentMode  = MODE_WATER_INPUT;
String         inputBuffer  = "";

float          waterPct     = 0.0;   // measured water level (%)
float          tempC        = 0.0;   // measured temperature (°C)
float          turbidityPct = 0.0;   // measured turbidity (0–100%)
bool           turbid       = false;

int            targetWater  = 0;     // desired water level (%)
int            targetTemp   = 0;     // desired temperature (°C)
bool           waterSet     = false;
bool           tempSet      = false;

bool           levelReached  = false;
CleanState     cleanState    = CLEAN_IDLE;
PumpState      pumpState     = PUMP_IDLE;   // current active pump in normal mode
bool           heaterWasOn   = false;       // remembers heater state before cleaning
bool           heaterRunning = false;       // actual heater relay state

bool           buzzerActive = false;
unsigned long  buzzerEnd    = 0;
unsigned long  lastSensor   = 0;
unsigned long  lastDisplay  = 0;

// ══════════════════════════════════════════════════════
//  HARDWARE HELPERS
// ══════════════════════════════════════════════════════

void relaySet(uint8_t pin, bool on) {
  digitalWrite(pin, on ? RELAY_ON : RELAY_OFF);
}

void buzzerBeep(uint16_t ms) {
  digitalWrite(BUZZER_PIN, HIGH);
  buzzerActive = true;
  buzzerEnd    = millis() + ms;
}

// ══════════════════════════════════════════════════════
//  SENSOR READING
// ══════════════════════════════════════════════════════

float getDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long d = pulseIn(ECHO_PIN, HIGH, 25000);
  return d ? d * 0.0343 / 2.0 : -1.0;
}

void readSensors() {
  float dist = getDistanceCm();
  if (dist >= 0.0 && dist <= TANK_HEIGHT_CM)
    waterPct = constrain(((TANK_HEIGHT_CM - dist) / TANK_HEIGHT_CM) * 100.0, 0.0, 100.0);

  ds18b20.requestTemperatures();
  float t = ds18b20.getTempCByIndex(0);
  if (t != DEVICE_DISCONNECTED_C && t > -50.0) tempC = t;

  turbidityPct = (analogRead(TURBIDITY_PIN) / 1023.0) * 100.0;
  turbid       = (turbidityPct > TURBIDITY_LIMIT);
}

// ══════════════════════════════════════════════════════
//  DISPLAY
// ══════════════════════════════════════════════════════

/*
 * Line 1: "W:XXX%id T:XXX C"
 * Line 2: "T:XX% CLN  S:XX%"   or   "T:XX%!TRB  S:XX%"
 */
void drawMain() {
  char line1[17], line2[17];

  snprintf(line1, sizeof(line1), "W:%3d%%%s T:%3dC",
           (int)waterPct,
           levelReached ? "id" : "  ",
           (int)tempC);

  snprintf(line2, sizeof(line2), "T:%2d%% %s  S:%2d%%",
           (int)turbidityPct,
           turbid ? "!TRB" : " CLN",
           waterSet ? targetWater : 0);

  lcd.setCursor(0, 0); lcd.print(line1);
  lcd.setCursor(0, 1); lcd.print(line2);
}

void drawWaterInput() {
  lcd.setCursor(0, 0); lcd.print("Water Level(%): ");
  lcd.setCursor(0, 1);
  String row = "> " + inputBuffer + "_              ";
  lcd.print(row.substring(0, 16));
}

void drawTempInput() {
  lcd.setCursor(0, 0); lcd.print("Set Temp  (C):  ");
  lcd.setCursor(0, 1);
  String row = "> " + inputBuffer + "_              ";
  lcd.print(row.substring(0, 16));
}

/*
 * B key: turbidity monitor screen (read-only, no input)
 * Line 1: "Turbidity:      "
 * Line 2: "Value:  XX.X %  "  + warning if above limit
 */
void drawTurbidityMonitor() {
  char line2[17];
  lcd.setCursor(0, 0);
  lcd.print("Turbidity:      ");
  snprintf(line2, sizeof(line2), "%-6s %5.1f %%  ",
           turbid ? "!HIGH" : "OK",
           turbidityPct);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

void showScreen() {
  if      (currentMode == MODE_MAIN)               drawMain();
  else if (currentMode == MODE_WATER_INPUT)        drawWaterInput();
  else if (currentMode == MODE_TEMP_INPUT)         drawTempInput();
  else if (currentMode == MODE_TURBIDITY_MONITOR)  drawTurbidityMonitor();
}

void showError(const char* msg1, const char* msg2) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(msg1);
  lcd.setCursor(0, 1); lcd.print(msg2);
  delay(1500);
  lcd.clear();
}

// ══════════════════════════════════════════════════════
//  KEYPAD HANDLER
// ══════════════════════════════════════════════════════

void handleKey(char k) {
  // ── Global navigation ──
  if (k == 'A') {
    currentMode = MODE_WATER_INPUT; inputBuffer = "";
    lcd.clear(); showScreen(); return;
  }
  if (k == 'B') {
    // Turbidity monitor: display-only, no input needed
    currentMode = MODE_TURBIDITY_MONITOR;
    lcd.clear(); showScreen(); return;
  }
  if (k == 'C') {
    currentMode = MODE_MAIN;
    lcd.clear(); showScreen(); return;
  }
  if (k == 'D') {
    currentMode = MODE_TEMP_INPUT; inputBuffer = "";
    lcd.clear(); showScreen(); return;
  }

  // In MAIN and TURBIDITY_MONITOR: only navigation keys work
  if (currentMode == MODE_MAIN || currentMode == MODE_TURBIDITY_MONITOR) return;

  if (k == '*') { inputBuffer = ""; showScreen(); return; }

  if (k == '#') {
    if (inputBuffer.length() == 0) return;
    int val = inputBuffer.toInt();

    if (currentMode == MODE_WATER_INPUT) {
      if (val > 100) {
        showError("Invalid!(0-100) ", "Try again...    ");
        inputBuffer = ""; showScreen();
      } else {
        targetWater  = val;
        waterSet     = true;
        levelReached = false;
        pumpState    = PUMP_IDLE;      // reset pump state for new target
        applyPumpState(PUMP_IDLE);     // stop any running pump immediately
        inputBuffer  = "";
        currentMode  = MODE_MAIN;
        lcd.clear(); showScreen();
      }
    } else if (currentMode == MODE_TEMP_INPUT) {
      targetTemp  = val;
      tempSet     = true;
      inputBuffer = "";
      currentMode = MODE_MAIN;
      lcd.clear(); showScreen();
    }
    return;
  }

  if (k >= '0' && k <= '9' && inputBuffer.length() < 3) {
    inputBuffer += k;
    showScreen();
  }
}

// ══════════════════════════════════════════════════════
//  CONTROL LOGIC
// ══════════════════════════════════════════════════════

/*
 * Turbidity cleaning cycle (state machine):
 *   CLEAN_IDLE  → turbidity > 50% detected → save heater state, start CLEAN_DRAIN
 *   CLEAN_DRAIN → outlet pump ON until water ≤ DRAIN_STOP_PCT → switch to CLEAN_FILL
 *   CLEAN_FILL  → inlet pump ON until water reaches targetWater (or 50% if not set)
 *               → restore heater, go back to CLEAN_IDLE
 */
void runTurbidityControl() {
  // Decide fill target: use user target if set, otherwise 50%
  int fillTarget = waterSet ? targetWater : 50;

  switch (cleanState) {

    case CLEAN_IDLE:
      if (turbid) {
        heaterWasOn = heaterRunning;   // save current heater state
        applyHeater(false);            // heater OFF
        applyPumpState(PUMP_DRAINING); // start draining
        buzzerBeep(3000);
        cleanState = CLEAN_DRAIN;
      }
      break;

    case CLEAN_DRAIN:
      applyHeater(false);
      applyPumpState(PUMP_DRAINING);
      if (waterPct <= DRAIN_STOP_PCT) {
        applyPumpState(PUMP_FILLING);  // switch to refill
        cleanState = CLEAN_FILL;
      }
      break;

    case CLEAN_FILL:
      applyHeater(false);
      applyPumpState(PUMP_FILLING);
      if (waterPct >= fillTarget - LEVEL_HYSTERESIS) {
        applyPumpState(PUMP_IDLE);
        if (heaterWasOn && waterPct >= 50.0) applyHeater(true);
        buzzerBeep(1500);
        cleanState = CLEAN_IDLE;
      }
      break;
  }
}

// Apply a pump state change only when it actually differs from current state.
// This prevents relay chatter from running on every loop iteration.
void applyPumpState(PumpState desired) {
  if (desired == pumpState) return;   // nothing to change
  pumpState = desired;
  switch (pumpState) {
    case PUMP_FILLING:
      relaySet(OUTLET_RELAY, false);  // outlet OFF first, then inlet ON
      relaySet(INLET_RELAY,  true);
      break;
    case PUMP_DRAINING:
      relaySet(INLET_RELAY,  false);  // inlet OFF first, then outlet ON
      relaySet(OUTLET_RELAY, true);
      break;
    case PUMP_IDLE:
      relaySet(INLET_RELAY,  false);
      relaySet(OUTLET_RELAY, false);
      break;
  }
}

void applyHeater(bool on) {
  if (on == heaterRunning) return;    // nothing to change
  heaterRunning = on;
  relaySet(TEMP_RELAY, on);
}

void runNormalControl() {
  // ── Water level control with hysteresis ──
  if (waterSet) {
    float diff = waterPct - targetWater;

    if (pumpState == PUMP_FILLING) {
      // Currently filling: stop only when water reaches target - hysteresis
      if (diff >= -LEVEL_HYSTERESIS) {
        applyPumpState(PUMP_IDLE);
        if (!levelReached) { levelReached = true; buzzerBeep(1500); }
      }
    } else if (pumpState == PUMP_DRAINING) {
      // Currently draining: stop only when water drops to target + hysteresis
      if (diff <= LEVEL_HYSTERESIS) {
        applyPumpState(PUMP_IDLE);
        if (!levelReached) { levelReached = true; buzzerBeep(1500); }
      }
    } else {
      // Idle: start a pump only when deviation exceeds tolerance
      if (diff < -LEVEL_TOLERANCE) {
        applyPumpState(PUMP_FILLING);    // water too low → fill
      } else if (diff > LEVEL_TOLERANCE) {
        applyPumpState(PUMP_DRAINING);   // water too high → drain
      }
    }
  } else {
    applyPumpState(PUMP_IDLE);
  }

  // ── Heater: only when tempSet AND water >= 50% ──
  if (tempSet && waterPct >= 50.0 && tempC < targetTemp) {
    applyHeater(true);
  } else {
    applyHeater(false);
  }
}

void runControl() {
  if (turbid || cleanState != CLEAN_IDLE) {
    // Hand full control to the cleaning state machine
    runTurbidityControl();
  } else {
    runNormalControl();
  }
}

// ══════════════════════════════════════════════════════
//  SETUP & LOOP
// ══════════════════════════════════════════════════════

void setup() {
  // Heater OFF first — before any other init — for safety
  pinMode(TEMP_RELAY, OUTPUT);
  digitalWrite(TEMP_RELAY, RELAY_OFF);

  pinMode(TRIG_PIN,     OUTPUT);
  pinMode(ECHO_PIN,     INPUT);
  pinMode(INLET_RELAY,  OUTPUT);
  pinMode(OUTLET_RELAY, OUTPUT);
  pinMode(BUZZER_PIN,   OUTPUT);

  relaySet(INLET_RELAY,  false);
  relaySet(OUTLET_RELAY, false);
  digitalWrite(BUZZER_PIN, LOW);

  ds18b20.begin();

  lcd.begin();
  lcd.backlight();
  lcd.clear();
  showScreen();
}

void loop() {
  unsigned long now = millis();

  if (now - lastSensor >= SENSOR_READ_MS) {
    lastSensor = now;
    readSensors();
  }

  if (buzzerActive && now >= buzzerEnd) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerActive = false;
  }

  char k = keypad.getKey();
  if (k) handleKey(k);

  runControl();

  if (now - lastDisplay >= DISPLAY_UPDATE_MS) {
    lastDisplay = now;
    showScreen();
  }
}
