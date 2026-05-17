/*
 * Water Heating & Monitoring System
 * Hardware: Arduino Mega, LCD 16x2 I2C, DS18B20, HC-SR04, Turbidity Sensor,
 *           4x4 Keypad, 4-Channel Relay, Buzzer, Heater, 2x Water Pumps
 * Tank Height: 27 cm
 *
 * Relay wiring note:
 *   If your relay module is ACTIVE-LOW (common), change RELAY_ON to LOW and RELAY_OFF to HIGH.
 *   Default below assumes ACTIVE-HIGH relay module.
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

// Active-HIGH relay (change to LOW/HIGH if using active-low module)
#define RELAY_ON   HIGH
#define RELAY_OFF  LOW

// ────────────────────── Sensors ────────────────────────
OneWire            oneWire(ONE_WIRE_BUS);
DallasTemperature  ds18b20(&oneWire);

// ─────────────────────── Config ────────────────────────
#define TANK_HEIGHT_CM      27.0
#define TURBIDITY_THRESHOLD 500    // 0-1023; tune for your sensor (higher = more turbid)
#define LEVEL_TOLERANCE     2.0    // ±2% dead-band for pump control
#define SENSOR_READ_MS      700    // sensor refresh period
#define DISPLAY_UPDATE_MS   500    // main-screen refresh period

// ──────────────────────── State ────────────────────────
enum Mode { MODE_MAIN, MODE_WATER_INPUT, MODE_TEMP_INPUT, MODE_TURBIDITY_INPUT };

Mode           currentMode    = MODE_WATER_INPUT;
String         inputBuffer    = "";

float          waterPct       = 0.0;   // measured water level (%)
float          tempC          = 0.0;   // measured temperature (°C)
float          turbidityPct   = 0.0;   // measured turbidity (0-100%)
bool           turbid         = false;

int            targetWater    = 0;     // desired water level (%)
int            targetTemp     = 0;     // desired temperature (°C)
int            turbidityLimit = 50;    // max acceptable turbidity % (default 50%)
bool           waterSet       = false;
bool           tempSet        = false;
bool           turbiditySet   = false;

bool           levelReached   = false; // flag: target water level achieved (stays true while maintaining)
bool           cleaningMode   = false; // flag: turbidity flush in progress

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
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 25000); // 25 ms timeout ≈ 4.3 m max
  if (duration == 0) return -1.0;
  return duration * 0.0343 / 2.0;
}

void readSensors() {
  // Ultrasonic → water level
  float dist = getDistanceCm();
  if (dist >= 0.0 && dist <= TANK_HEIGHT_CM) {
    float h = TANK_HEIGHT_CM - dist;
    waterPct = constrain((h / TANK_HEIGHT_CM) * 100.0, 0.0, 100.0);
  }

  // DS18B20 temperature
  ds18b20.requestTemperatures();
  float t = ds18b20.getTempCByIndex(0);
  if (t != DEVICE_DISCONNECTED_C && t > -50.0) tempC = t;

  // Turbidity: convert raw reading to percentage, compare against user limit
  turbidityPct = (analogRead(TURBIDITY_PIN) / 1023.0) * 100.0;
  turbid = (turbidityPct > turbidityLimit);
}

// ══════════════════════════════════════════════════════
//  DISPLAY
// ══════════════════════════════════════════════════════

/*
 * Line 1: "W:XXX%id T:XXX C"   (id shown when level reached)
 * Line 2: "TRB:XX%>XX S:XX%"   turbidity reading > limit, water setpoint
 */
void drawMain() {
  char line1[17], line2[17];

  snprintf(line1, sizeof(line1), "W:%3d%%%s T:%3dC",
           (int)waterPct,
           levelReached ? "id" : "  ",
           (int)tempC);

  // Show turbidity reading vs limit, and water setpoint
  snprintf(line2, sizeof(line2), "%s%2d%%>%2d%% S:%2d%%",
           turbid ? "!" : " ",
           (int)turbidityPct,
           turbidityLimit,
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

void drawTurbidityInput() {
  lcd.setCursor(0, 0); lcd.print("Turbidity Lim%: ");
  lcd.setCursor(0, 1);
  String row = "> " + inputBuffer + "_              ";
  lcd.print(row.substring(0, 16));
}

void showScreen() {
  if      (currentMode == MODE_MAIN)             drawMain();
  else if (currentMode == MODE_WATER_INPUT)      drawWaterInput();
  else if (currentMode == MODE_TEMP_INPUT)       drawTempInput();
  else if (currentMode == MODE_TURBIDITY_INPUT)  drawTurbidityInput();
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
  // ── Global navigation keys ──
  if (k == 'A') {
    currentMode = MODE_WATER_INPUT;
    inputBuffer = "";
    lcd.clear();
    showScreen();
    return;
  }
  if (k == 'B') {
    currentMode = MODE_TURBIDITY_INPUT;
    inputBuffer = "";
    lcd.clear();
    showScreen();
    return;
  }
  if (k == 'C') {
    currentMode = MODE_MAIN;
    lcd.clear();
    showScreen();
    return;
  }
  if (k == 'D') {
    currentMode = MODE_TEMP_INPUT;
    inputBuffer = "";
    lcd.clear();
    showScreen();
    return;
  }

  // ── Input-screen keys (do nothing in MAIN) ──
  if (currentMode == MODE_MAIN) return;

  // B/A/D/C already handled above; guard remaining keys for input modes only


  if (k == '*') {
    inputBuffer = "";
    showScreen();
    return;
  }

  if (k == '#') {
    if (inputBuffer.length() == 0) return;
    int val = inputBuffer.toInt();

    if (currentMode == MODE_WATER_INPUT) {
      if (val > 100) {
        showError("Invalid!(0-100) ", "Try again...    ");
        inputBuffer = "";
        showScreen();
      } else {
        targetWater  = val;
        waterSet     = true;
        levelReached = false;
        inputBuffer  = "";
        currentMode  = MODE_MAIN;
        lcd.clear();
        showScreen();
      }
    } else if (currentMode == MODE_TEMP_INPUT) {
      targetTemp  = val;
      tempSet     = true;
      inputBuffer = "";
      currentMode = MODE_MAIN;
      lcd.clear();
      showScreen();
    } else if (currentMode == MODE_TURBIDITY_INPUT) {
      if (val > 100) {
        showError("Invalid!(0-100) ", "Try again...    ");
        inputBuffer = "";
        showScreen();
      } else {
        turbidityLimit = val;
        turbiditySet   = true;
        inputBuffer    = "";
        currentMode    = MODE_MAIN;
        lcd.clear();
        showScreen();
      }
    }
    return;
  }

  // ── Digit input (max 3 digits) ──
  if (k >= '0' && k <= '9' && inputBuffer.length() < 3) {
    inputBuffer += k;
    showScreen();
  }
}

// ══════════════════════════════════════════════════════
//  CONTROL LOGIC
// ══════════════════════════════════════════════════════

void runControl() {
  // ── Turbidity cleaning mode ─────────────────────────
  if (turbid) {
    if (!cleaningMode) {
      cleaningMode = true;
      buzzerBeep(3000);  // alert: turbidity detected
    }

    relaySet(TEMP_RELAY, false); // heater always OFF during cleaning

    // Run both pumps to exchange water, keeping level near 50%
    if (waterPct >= 50.0) {
      // Both pumps on: drain dirty + refill fresh simultaneously
      relaySet(INLET_RELAY,  true);
      relaySet(OUTLET_RELAY, true);
    } else if (waterPct < 48.0) {
      // Level dropped below 50% - only refill
      relaySet(INLET_RELAY,  true);
      relaySet(OUTLET_RELAY, false);
    } else {
      // 48–50%: stop, let sensors stabilise
      relaySet(INLET_RELAY,  false);
      relaySet(OUTLET_RELAY, false);
    }
    return; // skip normal control while cleaning
  }

  // ── Turbidity cleared ──────────────────────────────
  cleaningMode = false;

  // ── Normal water level control ─────────────────────
  // Pumps maintain the level silently after first reach (no buzzer on maintenance).
  if (waterSet) {
    float diff = waterPct - targetWater;

    if (diff < -LEVEL_TOLERANCE) {
      // Below target: fill (silent if already reached once)
      relaySet(INLET_RELAY,  true);
      relaySet(OUTLET_RELAY, false);

    } else if (diff > LEVEL_TOLERANCE) {
      // Above target: drain (silent if already reached once)
      relaySet(OUTLET_RELAY, true);
      relaySet(INLET_RELAY,  false);

    } else {
      // Within tolerance: stop pumps
      relaySet(INLET_RELAY,  false);
      relaySet(OUTLET_RELAY, false);
      if (!levelReached) {
        // First time reaching target → sound buzzer and set flag
        levelReached = true;
        buzzerBeep(1500);
      }
      // If levelReached already true, pumps just stopped silently (maintenance done)
    }
  } else {
    relaySet(INLET_RELAY,  false);
    relaySet(OUTLET_RELAY, false);
  }

  // ── Heater control ─────────────────────────────────
  // Heater only allowed when water level >= 50%
  if (tempSet && waterPct >= 50.0) {
    relaySet(TEMP_RELAY, tempC < targetTemp);
  } else {
    relaySet(TEMP_RELAY, false);
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
  showScreen(); // show water-level input screen on boot
}

void loop() {
  unsigned long now = millis();

  // Read sensors periodically
  if (now - lastSensor >= SENSOR_READ_MS) {
    lastSensor = now;
    readSensors();
  }

  // Buzzer auto-off
  if (buzzerActive && now >= buzzerEnd) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerActive = false;
  }

  // Keypad input
  char k = keypad.getKey();
  if (k) handleKey(k);

  // Run actuator control
  runControl();

  // Refresh main screen periodically (avoids flicker on input screens)
  if (currentMode == MODE_MAIN && now - lastDisplay >= DISPLAY_UPDATE_MS) {
    lastDisplay = now;
    drawMain();
  }
}
