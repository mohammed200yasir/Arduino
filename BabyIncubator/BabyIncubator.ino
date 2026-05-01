/*
 * Baby Incubator Controller
 * Components: Arduino Uno, DHT11, LCD 16x4 I2C, MLX90614,
 *             Relay (Fan + Heater), 2 Buttons
 *
 * Button UP   -> increase target temperature by 0.5°C
 * Button DOWN -> decrease target temperature by 0.5°C
 * Fan ON      -> when ambient temp exceeds target + tolerance
 * Heater ON   -> when ambient temp drops below target - tolerance
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <Adafruit_MLX90614.h>

// ── Pin Definitions ────────────────────────────────────────────────────────
#define DHT_PIN       7
#define DHT_TYPE      DHT11
#define FAN_RELAY     8   // Active LOW relay
#define HEAT_RELAY    9   // Active LOW relay
#define BTN_UP        2   // Increase target temp
#define BTN_DOWN      3   // Decrease target temp

// ── Objects ────────────────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 4);   // Try 0x3F if 0x27 doesn't work
DHT               dht(DHT_PIN, DHT_TYPE);
Adafruit_MLX90614 mlx;

// ── Settings ───────────────────────────────────────────────────────────────
float targetTemp       = 37.0;
const float TEMP_MIN   = 35.0;
const float TEMP_MAX   = 40.0;
const float TOLERANCE  = 0.5;   // ±0.5°C dead band

// Safe baby body temperature range
const float BABY_TEMP_LOW  = 36.0;
const float BABY_TEMP_HIGH = 37.5;

// ── Timing ─────────────────────────────────────────────────────────────────
unsigned long lastReadTime  = 0;
unsigned long lastBtnUpTime = 0;
unsigned long lastBtnDnTime = 0;
const unsigned long READ_INTERVAL   = 2000;  // ms between sensor reads
const unsigned long DEBOUNCE_DELAY  = 200;   // ms button debounce

// ── Sensor Values ──────────────────────────────────────────────────────────
float ambientTemp = 0.0;
float humidity    = 0.0;
float babyTemp    = 0.0;

// ── Custom LCD Characters ──────────────────────────────────────────────────
byte heartChar[8] = {
  0b00000, 0b01010, 0b11111, 0b11111,
  0b11111, 0b01110, 0b00100, 0b00000
};
byte degreeChar[8] = {
  0b00110, 0b01001, 0b01001, 0b00110,
  0b00000, 0b00000, 0b00000, 0b00000
};
byte arrowUp[8] = {
  0b00100, 0b01110, 0b11111, 0b00100,
  0b00100, 0b00100, 0b00100, 0b00000
};
byte arrowDown[8] = {
  0b00100, 0b00100, 0b00100, 0b00100,
  0b11111, 0b01110, 0b00100, 0b00000
};

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(9600);

  pinMode(FAN_RELAY,  OUTPUT);
  pinMode(HEAT_RELAY, OUTPUT);
  pinMode(BTN_UP,   INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);

  // Start with relays OFF (active LOW → HIGH = OFF)
  digitalWrite(FAN_RELAY,  HIGH);
  digitalWrite(HEAT_RELAY, HIGH);

  // LCD init
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, heartChar);
  lcd.createChar(1, degreeChar);
  lcd.createChar(2, arrowUp);
  lcd.createChar(3, arrowDown);

  // Sensor init
  dht.begin();
  mlx.begin();

  showSplashScreen();
}

// ── Main Loop ──────────────────────────────────────────────────────────────
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

// ── Sensor Reading ─────────────────────────────────────────────────────────
void readSensors() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (!isnan(h) && !isnan(t)) {
    humidity    = h;
    ambientTemp = t;
  }

  float obj = mlx.readObjectTempC();
  if (obj > 20.0 && obj < 50.0) {   // sanity check
    babyTemp = obj;
  }
}

// ── Relay Control ──────────────────────────────────────────────────────────
void controlRelays() {
  if (ambientTemp > targetTemp + TOLERANCE) {
    // Too hot → fan ON, heater OFF
    digitalWrite(FAN_RELAY,  LOW);
    digitalWrite(HEAT_RELAY, HIGH);
  } else if (ambientTemp < targetTemp - TOLERANCE) {
    // Too cold → heater ON, fan OFF
    digitalWrite(HEAT_RELAY, LOW);
    digitalWrite(FAN_RELAY,  HIGH);
  } else {
    // Within range → both OFF
    digitalWrite(FAN_RELAY,  HIGH);
    digitalWrite(HEAT_RELAY, HIGH);
  }
}

// ── Display Update ─────────────────────────────────────────────────────────
void updateDisplay() {
  bool fanOn  = (digitalRead(FAN_RELAY)  == LOW);
  bool heatOn = (digitalRead(HEAT_RELAY) == LOW);

  // Row 0: Ambient temperature + humidity
  lcd.setCursor(0, 0);
  lcd.print("Amb:");
  lcdPrintFloat(ambientTemp, 1);
  lcd.write(1);   // degree char
  lcd.print("C ");
  lcd.print("H:");
  lcd.print((int)humidity);
  lcd.print("%  ");

  // Row 1: Baby body temperature + status
  lcd.setCursor(0, 1);
  lcd.write(0);   // heart char
  lcd.print("Baby:");
  lcdPrintFloat(babyTemp, 1);
  lcd.write(1);
  lcd.print("C ");
  lcd.print(getBabyStatus());

  // Row 2: Target temperature setting
  lcd.setCursor(0, 2);
  lcd.print("Set:");
  lcdPrintFloat(targetTemp, 1);
  lcd.write(1);
  lcd.print("C ");
  lcd.write(2);   // up arrow
  lcd.print("/");
  lcd.write(3);   // down arrow
  lcd.print(" Btn ");

  // Row 3: System status
  lcd.setCursor(0, 3);
  lcd.print("Status:");
  if (fanOn) {
    lcd.print(" FAN ON  ");
  } else if (heatOn) {
    lcd.print(" HEAT ON ");
  } else {
    lcd.print(" NORMAL  ");
  }
}

// ── Button Handling ────────────────────────────────────────────────────────
void handleButtons() {
  unsigned long now = millis();

  if (digitalRead(BTN_UP) == LOW && (now - lastBtnUpTime > DEBOUNCE_DELAY)) {
    lastBtnUpTime = now;
    if (targetTemp < TEMP_MAX) {
      targetTemp += 0.5;
      refreshTargetRow();
    }
  }

  if (digitalRead(BTN_DOWN) == LOW && (now - lastBtnDnTime > DEBOUNCE_DELAY)) {
    lastBtnDnTime = now;
    if (targetTemp > TEMP_MIN) {
      targetTemp -= 0.5;
      refreshTargetRow();
    }
  }
}

// ── Helpers ────────────────────────────────────────────────────────────────
void refreshTargetRow() {
  lcd.setCursor(0, 2);
  lcd.print("Set:");
  lcdPrintFloat(targetTemp, 1);
  lcd.write(1);
  lcd.print("C ");
  lcd.write(2);
  lcd.print("/");
  lcd.write(3);
  lcd.print(" Btn ");
}

const char* getBabyStatus() {
  if (babyTemp < BABY_TEMP_LOW)       return "COLD";
  else if (babyTemp > BABY_TEMP_HIGH) return "HOT ";
  else                                return "OK  ";
}

void lcdPrintFloat(float val, int decimals) {
  // Right-pad to keep column positions stable
  if (val < 10.0) lcd.print(" ");
  lcd.print(val, decimals);
}

void showSplashScreen() {
  lcd.clear();
  lcd.setCursor(1, 1);
  lcd.print("Baby  Incubator");
  lcd.setCursor(3, 2);
  lcd.print("Initializing...");
  delay(2000);
  lcd.clear();
}

void printSerial() {
  Serial.print("Ambient: ");  Serial.print(ambientTemp);
  Serial.print("C  Humidity: "); Serial.print(humidity);
  Serial.print("%  Baby: ");  Serial.print(babyTemp);
  Serial.print("C  Target: "); Serial.print(targetTemp);
  bool fanOn  = (digitalRead(FAN_RELAY)  == LOW);
  bool heatOn = (digitalRead(HEAT_RELAY) == LOW);
  Serial.print("C  Fan:");  Serial.print(fanOn  ? "ON" : "OFF");
  Serial.print(" Heat:"); Serial.println(heatOn ? "ON" : "OFF");
}
