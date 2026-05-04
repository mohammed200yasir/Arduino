/*
 * Baby Incubator Controller
 * Components: Arduino Uno, DHT11, LCD 16x4 I2C, MLX90614,
 *             Relay (Fan + Heater), 2 Buttons
 *
 * Button UP   -> increase target temperature +0.5°C
 * Button DOWN -> decrease target temperature -0.5°C
 * Fan ON      -> ambient > target + 0.5°C
 * Heater ON   -> ambient < target - 0.5°C
 *
 * LCD layout (16 chars × 4 rows — each row is EXACTLY 16 chars):
 *   Row 0: "Amb:37.2°cH: 65%"
 *   Row 1: "Baby:36.8°c  OK "
 *   Row 2: "Set:37.0°c  +/- "
 *   Row 3: "Sta: NORMAL     "
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <Adafruit_MLX90614.h>

// ── Pin Definitions ───────────────────────────────────────────────────────
#define DHT_PIN     7
#define DHT_TYPE    DHT11
#define FAN_RELAY   8    // Active LOW relay
#define HEAT_RELAY  9    // Active LOW relay
#define BTN_UP      2    // Raise target temp
#define BTN_DOWN    3    // Lower target temp

// ── Objects ───────────────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 4);  // Change to 0x3F if screen stays blank
DHT               dht(DHT_PIN, DHT_TYPE);
Adafruit_MLX90614 mlx;

// ── Settings ──────────────────────────────────────────────────────────────
float targetTemp         = 37.0;
const float TEMP_MIN     = 35.0;
const float TEMP_MAX     = 40.0;
const float TOLERANCE    = 0.5;    // ±0.5°C dead-band
const float BABY_LOW     = 36.0;   // healthy baby temp range
const float BABY_HIGH    = 37.5;

// ── Timing ────────────────────────────────────────────────────────────────
unsigned long lastReadTime  = 0;
unsigned long lastBtnUpTime = 0;
unsigned long lastBtnDnTime = 0;
const unsigned long READ_INTERVAL  = 2000;
const unsigned long DEBOUNCE_DELAY = 200;

// ── Sensor Values ─────────────────────────────────────────────────────────
float ambientTemp = 0.0;
float humidity    = 0.0;
float babyTemp    = 0.0;

// ── Custom LCD Char (heart only) ──────────────────────────────────────────
byte heartChar[8] = {
  0b00000, 0b01010, 0b11111, 0b11111,
  0b11111, 0b01110, 0b00100, 0b00000
};
// 0xDF is the HD44780 built-in degree symbol — used instead of a custom char

// ── Setup ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(9600);

  pinMode(FAN_RELAY,  OUTPUT);
  pinMode(HEAT_RELAY, OUTPUT);
  pinMode(BTN_UP,   INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);

  digitalWrite(FAN_RELAY,  HIGH);   // OFF
  digitalWrite(HEAT_RELAY, HIGH);   // OFF

  lcd.init();
  lcd.backlight();
  lcd.createChar(0, heartChar);

  dht.begin();
  mlx.begin();

  showSplashScreen();
}

// ── Main Loop ─────────────────────────────────────────────────────────────
void loop() {
  handleButtons();

  unsigned long now = millis();
  if (now - lastReadTime >= READ_INTERVAL) {
    lastReadTime = now;
    readSensors();
    controlRelays();
    updateDisplay();
    printSerial();
  }
}

// ── Sensor Reading ────────────────────────────────────────────────────────
void readSensors() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h) && !isnan(t)) {
    humidity    = h;
    ambientTemp = t;
  }

  float obj = mlx.readObjectTempC();
  if (obj > 20.0 && obj < 50.0) {
    babyTemp = obj;
  }
}

// ── Relay Control ─────────────────────────────────────────────────────────
void controlRelays() {
  if (ambientTemp > targetTemp + TOLERANCE) {
    digitalWrite(FAN_RELAY,  LOW);    // Fan ON
    digitalWrite(HEAT_RELAY, HIGH);   // Heater OFF
  } else if (ambientTemp < targetTemp - TOLERANCE) {
    digitalWrite(HEAT_RELAY, LOW);    // Heater ON
    digitalWrite(FAN_RELAY,  HIGH);   // Fan OFF
  } else {
    digitalWrite(FAN_RELAY,  HIGH);   // Both OFF
    digitalWrite(HEAT_RELAY, HIGH);
  }
}

// ── Display Update ────────────────────────────────────────────────────────
//
// Every row is written as exactly 16 characters so stale chars never remain.
// dtostrf(value, totalWidth, decimals, buffer) gives fixed-width floats.
//
void updateDisplay() {
  char tBuf[6], bBuf[6], sBuf[6], hBuf[4];

  // Fixed-width floats: width=4, 1 decimal → " 9.5" or "37.2"
  dtostrf(ambientTemp, 4, 1, tBuf);
  dtostrf(babyTemp,    4, 1, bBuf);
  dtostrf(targetTemp,  4, 1, sBuf);

  // Humidity: 3 chars → " 65" or "100"
  snprintf(hBuf, sizeof(hBuf), "%3d", (int)constrain(humidity, 0, 100));

  bool fanOn  = (digitalRead(FAN_RELAY)  == LOW);
  bool heatOn = (digitalRead(HEAT_RELAY) == LOW);

  // ── Row 0: Ambient temp + humidity ────────────────────────────────────
  // "Amb:37.2°cH: 65%"  (4+4+1+1+2+3+1 = 16)
  lcd.setCursor(0, 0);
  lcd.print("Amb:");      // 4
  lcd.print(tBuf);        // 4  e.g. "37.2"
  lcd.write(0xDF);        // 1  degree symbol
  lcd.print("cH:");       // 3
  lcd.print(hBuf);        // 3  e.g. " 65"
  lcd.print("%");         // 1  → total 16

  // ── Row 1: Baby temp + status ─────────────────────────────────────────
  // heart(1) + "Baby:"(5) + bBuf(4) + °(1) + "c"(1) + status(4) = 16
  lcd.setCursor(0, 1);
  lcd.write((byte)0);      // heart
  lcd.print("Baby:");      // 5
  lcd.print(bBuf);         // 4  e.g. "36.8"
  lcd.write(0xDF);         // degree
  lcd.print("c");          // 1
  lcd.print(babyStatus()); // 4  e.g. " OK " / "COLD" / "HOT "

  // ── Row 2: Target temp setting ────────────────────────────────────────
  // "Set:37.0°c  +/- "  (4+4+1+1+2+3+1 = 16)
  printRow2();

  // ── Row 3: System status ──────────────────────────────────────────────
  // "Sta: NORMAL     "  (4+12 = 16)
  lcd.setCursor(0, 3);
  lcd.print("Sta:");  // 4
  if (fanOn)
    lcd.print(" FAN ON     ");  // 12
  else if (heatOn)
    lcd.print(" HEAT ON    ");  // 12
  else
    lcd.print(" NORMAL     ");  // 12
}

// ── Row 2 helper (also called from button handler) ────────────────────────
void printRow2() {
  char sBuf[6];
  dtostrf(targetTemp, 4, 1, sBuf);
  // "Set:37.0°c  +/- "  (4+4+1+1+6 = 16)
  lcd.setCursor(0, 2);
  lcd.print("Set:");     // 4
  lcd.print(sBuf);       // 4
  lcd.write(0xDF);       // 1
  lcd.print("c  +/- ");  // 7  → total 16
}

// ── Button Handling ───────────────────────────────────────────────────────
void handleButtons() {
  unsigned long now = millis();

  if (digitalRead(BTN_UP) == LOW && (now - lastBtnUpTime > DEBOUNCE_DELAY)) {
    lastBtnUpTime = now;
    if (targetTemp < TEMP_MAX) {
      targetTemp += 0.5;
      printRow2();
    }
  }

  if (digitalRead(BTN_DOWN) == LOW && (now - lastBtnDnTime > DEBOUNCE_DELAY)) {
    lastBtnDnTime = now;
    if (targetTemp > TEMP_MIN) {
      targetTemp -= 0.5;
      printRow2();
    }
  }
}

// ── Helpers ───────────────────────────────────────────────────────────────

// Returns exactly 4 chars for consistent column alignment
const char* babyStatus() {
  if (babyTemp < BABY_LOW)  return "COLD";
  if (babyTemp > BABY_HIGH) return "HOT ";
  return " OK ";
}

void showSplashScreen() {
  lcd.clear();
  lcd.setCursor(1, 1);
  lcd.print("Baby  Incubator");
  lcd.setCursor(3, 2);
  lcd.print("Starting...   ");
  delay(2000);
  lcd.clear();
}

void printSerial() {
  Serial.print("Amb:");    Serial.print(ambientTemp, 1);
  Serial.print("C Hum:");  Serial.print(humidity, 0);
  Serial.print("% Baby:"); Serial.print(babyTemp, 1);
  Serial.print("C Set:");  Serial.print(targetTemp, 1);
  Serial.print("C Fan:");  Serial.print(digitalRead(FAN_RELAY)  == LOW ? "ON" : "OFF");
  Serial.print(" Heat:");  Serial.println(digitalRead(HEAT_RELAY) == LOW ? "ON" : "OFF");
}
