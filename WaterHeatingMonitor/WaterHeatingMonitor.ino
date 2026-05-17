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
#define TANK_HEIGHT_CM        27.0
#define TURBIDITY_LIMIT       50.0    // cleaning triggers above this %
#define TURBID_CONFIRM_COUNT  5       // must exceed limit for 5 readings (~3.5s) before cleaning
#define DRAIN_STOP_PCT        5.0     // tank considered empty at this %
#define MIN_DRAIN_MS          10000UL // outlet runs at least 10 s before checking level
#define CLEAN_COOLDOWN_MS     30000UL // wait 30 s after cleaning before next cycle allowed
#define LEVEL_TOLERANCE       3.0     // pump starts when level deviates by more than ±3%
#define LEVEL_HYSTERESIS      1.0     // pump stops when level is within ±1% of target
#define SENSOR_READ_MS        700
#define DISPLAY_UPDATE_MS     500

// ──────────────────────── State ────────────────────────
enum Mode       { MODE_MAIN, MODE_WATER_INPUT, MODE_TEMP_INPUT, MODE_TURBIDITY_MONITOR };
enum CleanState { CLEAN_IDLE, CLEAN_DRAIN, CLEAN_FILL };
enum PumpState  { PUMP_IDLE, PUMP_FILLING, PUMP_DRAINING };

Mode           currentMode   = MODE_WATER_INPUT;
String         inputBuffer   = "";

float          waterPct      = 0.0;
float          tempC         = 0.0;
float          turbidityPct  = 0.0;
bool           turbid        = false;   // true only after TURBID_CONFIRM_COUNT readings
uint8_t        turbidCount   = 0;       // debounce counter

int            targetWater   = 0;
int            targetTemp    = 0;
bool           waterSet      = false;
bool           tempSet       = false;

bool           levelReached  = false;
CleanState     cleanState    = CLEAN_IDLE;
PumpState      pumpState     = PUMP_IDLE;
bool           heaterWasOn   = false;
bool           heaterRunning = false;

unsigned long  drainStartTime   = 0;    // when CLEAN_DRAIN phase began
unsigned long  cleanCooldownEnd = 0;    // millis() when next cleaning is allowed

bool           buzzerActive  = false;
unsigned long  buzzerEnd     = 0;
unsigned long  lastSensor    = 0;
unsigned long  lastDisplay   = 0;

// ══════════════════════════════════════════════════════
//  HARDWARE HELPERS
// ══════════════════════════════════════════════════════

void relaySet(uint8_t pin, bool on) {
  digitalWrite(pin, on ? RELAY_ON : RELAY_OFF);
}

// Turn off ALL relays — used as safety call
void allOff() {
  relaySet(INLET_RELAY,  false);
  relaySet(OUTLET_RELAY, false);
  relaySet(TEMP_RELAY,   false);
  pumpState     = PUMP_IDLE;
  heaterRunning = false;
}

void buzzerBeep(uint16_t ms) {
  digitalWrite(BUZZER_PIN, HIGH);
  buzzerActive = true;
  buzzerEnd    = millis() + ms;
}

// Write pump relays only when state actually changes.
void applyPumpState(PumpState desired) {
  if (desired == pumpState) return;
  pumpState = desired;
  switch (pumpState) {
    case PUMP_FILLING:
      relaySet(OUTLET_RELAY, false); // outlet OFF first
      relaySet(INLET_RELAY,  true);  // then inlet ON
      break;
    case PUMP_DRAINING:
      relaySet(INLET_RELAY,  false); // inlet OFF first
      relaySet(OUTLET_RELAY, true);  // then outlet ON
      break;
    case PUMP_IDLE:
      relaySet(INLET_RELAY,  false);
      relaySet(OUTLET_RELAY, false);
      break;
  }
}

void applyHeater(bool on) {
  if (on == heaterRunning) return;
  heaterRunning = on;
  relaySet(TEMP_RELAY, on);
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
  // Water level
  float dist = getDistanceCm();
  if (dist >= 0.0 && dist <= TANK_HEIGHT_CM)
    waterPct = constrain(((TANK_HEIGHT_CM - dist) / TANK_HEIGHT_CM) * 100.0, 0.0, 100.0);

  // Temperature
  ds18b20.requestTemperatures();
  float t = ds18b20.getTempCByIndex(0);
  if (t != DEVICE_DISCONNECTED_C && t > -50.0) tempC = t;

  // Turbidity with debounce: must exceed limit for TURBID_CONFIRM_COUNT readings
  turbidityPct = (analogRead(TURBIDITY_PIN) / 1023.0) * 100.0;
  if (turbidityPct > TURBIDITY_LIMIT) {
    if (turbidCount < TURBID_CONFIRM_COUNT) turbidCount++;
  } else {
    if (turbidCount > 0) turbidCount--;   // ramps down when clean
  }
  turbid = (turbidCount >= TURBID_CONFIRM_COUNT);
}

// ══════════════════════════════════════════════════════
//  DISPLAY
// ══════════════════════════════════════════════════════

/*
 * Line 1: "W:XXX%id T:XXX C"
 * Line 2: "T:XX% CLN  S:XX%"  or "T:XX%!TRB  S:XX%"
 */
void drawMain() {
  char line1[17], line2[17];
  snprintf(line1, sizeof(line1), "W:%3d%%%s T:%3dC",
           (int)waterPct, levelReached ? "id" : "  ", (int)tempC);
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
 * B screen: diagnostic — shows both water level AND turbidity so user can
 * verify sensors and spot if turbidity is falsely triggering cleaning.
 * Line 1: "W:XXX%  Trb:XX% "
 * Line 2: "CLN/!HIGH  [state]"
 */
void drawTurbidityMonitor() {
  char line1[17], line2[17];
  snprintf(line1, sizeof(line1), "W:%3d%% Trb:%2d%% ",
           (int)waterPct, (int)turbidityPct);
  const char* stateStr = "NORMAL ";
  if (cleanState == CLEAN_DRAIN) stateStr = "DRNING ";
  else if (cleanState == CLEAN_FILL) stateStr = "FILLING";
  snprintf(line2, sizeof(line2), "%-5s  %s",
           turbid ? "!HIGH" : "OK",
           stateStr);
  lcd.setCursor(0, 0); lcd.print(line1);
  lcd.setCursor(0, 1); lcd.print(line2);
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
  if (k == 'A') {
    currentMode = MODE_WATER_INPUT; inputBuffer = "";
    lcd.clear(); showScreen(); return;
  }
  if (k == 'B') {
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
        // BUG FIX: do NOT manually set pumpState before calling applyPumpState.
        // applyPumpState handles the relay write only if state changes.
        applyPumpState(PUMP_IDLE);
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

void runNormalControl() {
  // ── Water level ──
  if (waterSet) {
    float diff = waterPct - targetWater;

    if (pumpState == PUMP_FILLING) {
      if (diff >= -LEVEL_HYSTERESIS) {          // reached target
        applyPumpState(PUMP_IDLE);
        if (!levelReached) { levelReached = true; buzzerBeep(1500); }
      }
    } else if (pumpState == PUMP_DRAINING) {
      if (diff <= LEVEL_HYSTERESIS) {           // drained to target
        applyPumpState(PUMP_IDLE);
        if (!levelReached) { levelReached = true; buzzerBeep(1500); }
      }
    } else {
      // PUMP_IDLE: start pumping only when deviation > tolerance
      if      (diff < -LEVEL_TOLERANCE) applyPumpState(PUMP_FILLING);
      else if (diff >  LEVEL_TOLERANCE) applyPumpState(PUMP_DRAINING);
    }
  } else {
    applyPumpState(PUMP_IDLE);
  }

  // ── Heater: requires tempSet AND water >= 50% ──
  if (tempSet && waterPct >= 50.0 && tempC < targetTemp)
    applyHeater(true);
  else
    applyHeater(false);
}

/*
 * Turbidity cleaning state machine.
 * Debounce prevents false triggers; MIN_DRAIN_MS prevents skipping drain phase;
 * CLEAN_COOLDOWN_MS prevents immediate re-trigger after cleaning finishes.
 */
void runTurbidityControl() {
  int fillTarget = waterSet ? targetWater : 50;

  switch (cleanState) {

    case CLEAN_IDLE:
      if (turbid && millis() >= cleanCooldownEnd) {
        heaterWasOn    = heaterRunning;
        applyHeater(false);
        applyPumpState(PUMP_DRAINING);
        drainStartTime = millis();
        buzzerBeep(3000);
        cleanState = CLEAN_DRAIN;
      }
      break;

    case CLEAN_DRAIN:
      applyHeater(false);
      applyPumpState(PUMP_DRAINING);
      // Only switch to fill once outlet has run for MIN_DRAIN_MS AND tank is near empty
      if (millis() - drainStartTime >= MIN_DRAIN_MS && waterPct <= DRAIN_STOP_PCT) {
        applyPumpState(PUMP_FILLING);
        cleanState = CLEAN_FILL;
      }
      break;

    case CLEAN_FILL:
      applyHeater(false);
      applyPumpState(PUMP_FILLING);
      if (waterPct >= fillTarget - LEVEL_HYSTERESIS) {
        applyPumpState(PUMP_IDLE);
        if (heaterWasOn && waterPct >= 50.0) applyHeater(true);
        cleanCooldownEnd = millis() + CLEAN_COOLDOWN_MS; // 30 s before next clean
        turbidCount = 0;  // reset debounce so sensor must re-confirm turbidity
        buzzerBeep(1500);
        cleanState = CLEAN_IDLE;
      }
      break;
  }
}

void runControl() {
  // Do nothing until the user has entered a water level setpoint.
  // This prevents pumps from running at startup.
  if (!waterSet && cleanState == CLEAN_IDLE) {
    applyPumpState(PUMP_IDLE);
    applyHeater(false);
    return;
  }

  if (turbid || cleanState != CLEAN_IDLE)
    runTurbidityControl();
  else
    runNormalControl();
}

// ══════════════════════════════════════════════════════
//  SETUP & LOOP
// ══════════════════════════════════════════════════════

void setup() {
  // Set relay pins HIGH (inactive for active-LOW) BEFORE pinMode so the relay
  // never receives a LOW glitch during pin initialisation.
  digitalWrite(TEMP_RELAY,   RELAY_OFF);
  digitalWrite(INLET_RELAY,  RELAY_OFF);
  digitalWrite(OUTLET_RELAY, RELAY_OFF);

  pinMode(TEMP_RELAY,   OUTPUT);
  pinMode(INLET_RELAY,  OUTPUT);
  pinMode(OUTLET_RELAY, OUTPUT);
  pinMode(TRIG_PIN,     OUTPUT);
  pinMode(ECHO_PIN,     INPUT);
  pinMode(BUZZER_PIN,   OUTPUT);

  allOff();                          // sync state variables with relay state
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
