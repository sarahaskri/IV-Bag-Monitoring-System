#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "HX711.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>

// Firmware reconstruite depuis le brouillon Arduino et la presentation.
// Prototype academique : pas un dispositif medical certifie.

// Broches ESP32 reprises du brouillon
constexpr int HX711_DOUT_PIN = 4;
constexpr int HX711_SCK_PIN = 5;
constexpr int LED_PIN = 18;
constexpr int BUZZER_PIN = 19;
constexpr int WATER_SENSOR_PIN = 34;
constexpr int I2C_SDA_PIN = 21;
constexpr int I2C_SCL_PIN = 22;

// Matériel
constexpr uint8_t LCD_I2C_ADDRESS = 0x27;
constexpr uint8_t LCD_COLUMNS = 16;
LiquidCrystal_I2C lcd(LCD_I2C_ADDRESS, LCD_COLUMNS, 2);
HX711 scale;

// Valeurs provisoires à calibrer sur le montage réel
constexpr float CALIBRATION_FACTOR = -400.0f;
constexpr float EMPTY_BAG_TARE_G = 0.0f;  // Sac vide + support
constexpr float LOW_BAG_CONTENT_G = 50.0f;
constexpr int WATER_ALERT_THRESHOLD = 100; // Valeur ADC à ajuster
constexpr float MIN_WEIGHT_CHANGE_G = 1.0f;
constexpr unsigned long OCCLUSION_DELAY_MS = 20000UL;
constexpr unsigned long FLOW_STALE_MS = 5000UL;

// Renseigner localement; ne jamais publier de vrais identifiants Wi-Fi.
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* BACKEND_URL = "http://192.168.1.10:3000/api/measurements";
const char* DEVICE_ID = "iv-monitor-01";

constexpr unsigned long SAMPLE_INTERVAL_MS = 1000UL;
constexpr unsigned long POST_INTERVAL_MS = 5000UL;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 10000UL;
constexpr float FLOW_SMOOTHING = 0.35f;

enum AlarmStatus {
  STATUS_NORMAL,
  STATUS_WATER_ALERT,
  STATUS_BAG_LOW,
  STATUS_OCCLUSION_SUSPECTED
};

float currentWeightG = 0.0f;
float previousWeightG = 0.0f;
float flowMlPerMin = 0.0f;
float remainingContentG = 0.0f;
int waterSensorRaw = 0;
bool hasWeightBaseline = false;
bool sensorReady = true;
unsigned long lastSampleMs = 0;
unsigned long lastWeightChangeMs = 0;
unsigned long lastWeightDecreaseMs = 0;
unsigned long lastWeightReferenceMs = 0;
unsigned long lastPostMs = 0;
unsigned long lastWifiAttemptMs = 0;
AlarmStatus currentStatus = STATUS_NORMAL;
AlarmStatus lastPostedStatus = STATUS_NORMAL;
bool hasPostedStatus = false;

void printLcdLine(uint8_t row, const char* text) {
  lcd.setCursor(0, row);
  lcd.print("                ");
  lcd.setCursor(0, row);
  for (uint8_t i = 0; text[i] != '\0' && i < LCD_COLUMNS; ++i) {
    lcd.print(text[i]);
  }
}

float nonNegative(float value) {
  return value < 0.0f ? 0.0f : value;
}

const char* statusName(AlarmStatus status) {
  switch (status) {
    case STATUS_WATER_ALERT: return "water_alert";
    case STATUS_BAG_LOW: return "bag_low";
    case STATUS_OCCLUSION_SUSPECTED: return "occlusion_suspected";
    default: return "normal";
  }
}

bool wifiConfigured() {
  return String(WIFI_SSID) != "YOUR_WIFI_SSID" &&
         String(WIFI_PASSWORD) != "YOUR_WIFI_PASSWORD";
}

void connectToWiFi() {
  if (!wifiConfigured()) {
    Serial.println("Wi-Fi non configure; alertes locales actives.");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttemptMs = millis();
}

void maintainWiFi(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED || !wifiConfigured()) return;
  if (now - lastWifiAttemptMs >= WIFI_CONNECT_TIMEOUT_MS) {
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWifiAttemptMs = now;
  }
}
void postMeasurement() {
  if (WiFi.status() != WL_CONNECTED) return;

  // Exemple de contrat JSON; adapter les champs aux routes Node.js du projet.
  const float remainingMinutes =
      flowMlPerMin > 0.05f ? remainingContentG / flowMlPerMin : -1.0f;

  String json = "{";
  json += "\"deviceId\":\"" + String(DEVICE_ID) + "\",";
  json += "\"weightG\":" + String(currentWeightG, 1) + ",";
  json += "\"remainingContentG\":" + String(remainingContentG, 1) + ",";
  json += "\"waterSensorRaw\":" + String(waterSensorRaw) + ",";
  json += "\"flowMlMin\":" + String(flowMlPerMin, 2) + ",";
  json += "\"remainingMinutes\":" +
          (remainingMinutes >= 0.0f ? String(remainingMinutes, 1) : String("null")) + ",";
  json += "\"status\":\"" + String(statusName(currentStatus)) + "\"";
  json += "}";

  HTTPClient http;
  http.setTimeout(2500);
  if (!http.begin(BACKEND_URL)) {
    Serial.println("Initialisation HTTP impossible.");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(json);
  Serial.print("Reponse HTTP: ");
  Serial.println(code);
  http.end();

  if (code > 0) {
    lastPostedStatus = currentStatus;
    hasPostedStatus = true;
  }
}

void readSensors(unsigned long now) {
  if (!scale.wait_ready_timeout(500)) {
    sensorReady = false;
    printLcdLine(0, "HX711 absent");
    printLcdLine(1, "Verifier cablage");
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, LOW);
    Serial.println("Erreur: HX711 non disponible.");
    return;
  }

  sensorReady = true;
  currentWeightG = nonNegative(scale.get_units(5));
  remainingContentG = nonNegative(currentWeightG - EMPTY_BAG_TARE_G);

  const int a = analogRead(WATER_SENSOR_PIN);
  delay(5);
  const int b = analogRead(WATER_SENSOR_PIN);
  waterSensorRaw = (a + b) / 2;

  if (!hasWeightBaseline) {
    previousWeightG = currentWeightG;
    lastWeightReferenceMs = now;
    lastWeightChangeMs = now;
    lastWeightDecreaseMs = now;
    hasWeightBaseline = true;
    return;
  }

  const unsigned long elapsedMs = now - lastWeightReferenceMs;
  const float deltaG = currentWeightG - previousWeightG;

  if (fabsf(deltaG) >= MIN_WEIGHT_CHANGE_G) {
    lastWeightChangeMs = now;
    if (deltaG < 0.0f && elapsedMs > 0UL) {
      // Approximation expérimentale : 1 g de baisse ~= 1 mL écoulé.
      const float measuredFlow =
          (-deltaG) * 60000.0f / static_cast<float>(elapsedMs);
      flowMlPerMin = flowMlPerMin <= 0.0f
          ? measuredFlow
          : FLOW_SMOOTHING * measuredFlow +
                (1.0f - FLOW_SMOOTHING) * flowMlPerMin;
      lastWeightDecreaseMs = now;
    }
    previousWeightG = currentWeightG;
    lastWeightReferenceMs = now;
  }

  if (now - lastWeightDecreaseMs > FLOW_STALE_MS) {
    flowMlPerMin = 0.0f;
  }
}

AlarmStatus evaluateStatus(unsigned long now) {
  const bool waterAlert = waterSensorRaw > WATER_ALERT_THRESHOLD;
  const bool bagLow = remainingContentG < LOW_BAG_CONTENT_G;
  const bool occlusionSuspected =
      !bagLow && !waterAlert &&
      now - lastWeightChangeMs > OCCLUSION_DELAY_MS;

  if (waterAlert) return STATUS_WATER_ALERT;
  if (bagLow) return STATUS_BAG_LOW;
  if (occlusionSuspected) return STATUS_OCCLUSION_SUSPECTED;
  return STATUS_NORMAL;
}

void updateOutputs() {
  const char* label = "Etat normal";
  const bool alarm = currentStatus != STATUS_NORMAL;

  switch (currentStatus) {
    case STATUS_WATER_ALERT:
      label = "Eau detectee";
      Serial.println("ALERTE: capteur eau actif; fuite a verifier.");
      break;
    case STATUS_BAG_LOW:
      label = "Poche basse";
      Serial.println("ALERTE: poche presque vide.");
      break;
    case STATUS_OCCLUSION_SUSPECTED:
      label = "Debit interrompu";
      Serial.println("ALERTE: obstruction suspectee, poids stable.");
      break;
    default:
      break;
  }

  char weightLine[LCD_COLUMNS + 1];
  snprintf(weightLine, sizeof(weightLine), "Poids: %d g",
           static_cast<int>(currentWeightG));
  printLcdLine(0, weightLine);
  printLcdLine(1, label);

  digitalWrite(LED_PIN, alarm ? HIGH : LOW);
  digitalWrite(BUZZER_PIN, alarm ? HIGH : LOW);

  Serial.print("Poids: ");
  Serial.print(currentWeightG, 1);
  Serial.print(" g | Eau brute: ");
  Serial.print(waterSensorRaw);
  Serial.print(" | Debit estime: ");
  Serial.print(flowMlPerMin, 2);
  Serial.print(" mL/min | Statut: ");
  Serial.println(statusName(currentStatus));
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  analogReadResolution(12);
  scale.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
  scale.set_scale(CALIBRATION_FACTOR);

  printLcdLine(0, "Systeme IV");
  printLcdLine(1, "Initialisation");
  if (scale.wait_ready_timeout(3000)) {
    scale.tare();
  } else {
    sensorReady = false;
    Serial.println("Attention: HX711 non detecte au demarrage.");
  }

  connectToWiFi();
  lastSampleMs = millis();
  lastWeightChangeMs = lastSampleMs;
  lastWeightDecreaseMs = lastSampleMs;

  printLcdLine(0, "Surveillance IV");
  printLcdLine(1, "Systeme pret");
  Serial.println("Systeme de surveillance IV pret.");
}

void loop() {
  const unsigned long now = millis();
  maintainWiFi(now);
  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) return;
  lastSampleMs = now;

  readSensors(now);
  if (!sensorReady || !hasWeightBaseline) return;

  currentStatus = evaluateStatus(now);
  updateOutputs();

  const bool statusChanged =
      !hasPostedStatus || currentStatus != lastPostedStatus;
  if (statusChanged || now - lastPostMs >= POST_INTERVAL_MS) {
    postMeasurement();
    lastPostMs = now;
    lastPostedStatus = currentStatus;
    hasPostedStatus = true;
  }

  Serial.println("--------------------------------");
}






