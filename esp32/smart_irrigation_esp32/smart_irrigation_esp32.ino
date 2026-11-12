/*
 * Fieldnote Smart Irrigation — ESP32 auto-pump version
 * Pins and soil mapping match the user's supplied working code.
 * Relay GPIO 14 is active-low: LOW = pump ON, HIGH = pump OFF.
 * The laptop API supplies a cached Open-Meteo forecast in its POST response.
 *
 * Install from Arduino Library Manager:
 * - DHT sensor library by Adafruit (and Adafruit Unified Sensor)
 * - Adafruit SSD1306
 * - Adafruit GFX Library
 * - ArduinoJson (version 6 or 7)
 *
 * Automatic watering: dry soil plus a safe forecast; 10-second run cap,
 * 10-minute soak, and maximum 3 cycles/hour. Manual mode follows the
 * dashboard ON/OFF switch until OFF is pressed. It stops on lost connection
 * or reboot and never inherits an old ON command after reconnecting.
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include "DHT.h"

// ---- Enter your Wi-Fi details here; laptop IP is current as of this setup ----
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* DASHBOARD_URL = "http://10.184.200.123:8000/api/readings";

// Exact pin definitions supplied by the user
#define SOIL_PIN 34
#define DHT_PIN 26
#define DHT_TYPE DHT11
#define OLED_SDA 32
#define OLED_SCL 33
#define RELAY_PIN 14

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// Soil-to-pump rules. Soil calibration remains the user's supplied mapping.
constexpr int DRY_SOIL_PERCENT = 20; // automatic watering trigger below 20%
constexpr float RAIN_DELAY_CHANCE_PERCENT = 50.0f;
constexpr float RAIN_DELAY_AMOUNT_MM = 1.0f; // expected over the next 6 hours
constexpr unsigned long SEND_EVERY_MS = 5000;
constexpr unsigned long MAX_PUMP_RUN_MS = 10000;
constexpr unsigned long SOAK_TIME_MS = 10UL * 60UL * 1000UL;
constexpr unsigned long CYCLE_WINDOW_MS = 60UL * 60UL * 1000UL;
constexpr int MAX_CYCLES_PER_HOUR = 3;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
DHT dht(DHT_PIN, DHT_TYPE);
bool oledReady = false;
bool dashboardConnected = false;
bool pumpOn = false;
bool manualMode = false;
bool manualCommandOn = false;
bool manualStopPending = false;
bool pumpStartedManual = false;
unsigned long lastSent = 0;
unsigned long pumpStartedAt = 0;
unsigned long nextPumpAllowedAt = 0;
unsigned long cycleWindowStartedAt = 0;
int pumpCyclesThisHour = 0;

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to Wi-Fi: %s", WIFI_SSID);
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("ESP32 IP: "); Serial.println(WiFi.localIP());
    Serial.print("Wi-Fi gateway: "); Serial.println(WiFi.gatewayIP());
  } else {
    Serial.println("Wi-Fi connection timed out; retrying later.");
  }
}

void setPump(bool on, const char* reason) {
  if (pumpOn == on) return;
  pumpOn = on;
  digitalWrite(RELAY_PIN, pumpOn ? LOW : HIGH); // active-low relay
  if (pumpOn) {
    pumpStartedAt = millis();
    pumpStartedManual = manualMode;
    if (!pumpStartedManual) pumpCyclesThisHour++;
    Serial.printf("PUMP ON: %s\n", reason);
  } else {
    if (!pumpStartedManual || !manualMode) {
      nextPumpAllowedAt = millis() + SOAK_TIME_MS;
      Serial.printf("PUMP OFF: %s; automatic soak period started\n", reason);
    } else {
      Serial.printf("PUMP OFF: %s\n", reason);
    }
  }
}

void updateOLED(int soilPercent, int soilRaw, float temperature, float humidity,
                float rainChance, float rainfallMm, bool forecastValid) {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(15, 0); display.println("SMART IRRIGATION");
  display.setCursor(0, 13); display.print("Soil: "); display.print(soilPercent);
  display.print("% Raw:"); display.print(soilRaw);
  display.setCursor(0, 26);
  if (isnan(temperature)) display.print("Temp: --");
  else { display.print("Temp: "); display.print(temperature, 1); display.print(" C"); }
  display.setCursor(70, 26);
  if (isnan(humidity)) display.print("Hum: --");
  else { display.print("Hum: "); display.print(humidity, 0); display.print("%"); }
  display.setCursor(0, 39);
  if (forecastValid) {
    display.print("Rain: "); display.print(rainChance, 0);
    display.print("% "); display.print(rainfallMm, 1); display.print("mm");
  } else display.print("Rain: no forecast");
  display.setCursor(0, 53); display.print("Pump: ");
  display.print(pumpOn ? "ON" : "OFF"); display.print(manualMode ? " MANUAL" : " AUTO");
  display.display();
}

bool postReadings(int soilPercent, int soilRaw, float temperature, float humidity,
                  float& rainChance, float& rainfallMm) {
  if (WiFi.status() != WL_CONNECTED) {
    dashboardConnected = false;
    if (pumpOn) setPump(false, "Wi-Fi unavailable");
    if (manualMode && manualCommandOn) manualStopPending = true;
    connectWiFi();
    return false;
  }

  String body = "{\"soil_moisture\":" + String(soilPercent) +
                ",\"soil_raw\":" + String(soilRaw) +
                ",\"pump_on\":" + String(pumpOn ? "true" : "false");
  if (manualStopPending) body += ",\"manual_stop_pending\":true";
  if (!isnan(temperature)) body += ",\"temperature\":" + String(temperature, 1);
  if (!isnan(humidity)) body += ",\"humidity\":" + String(humidity, 1);
  body += "}";

  HTTPClient http;
  http.setTimeout(2500);
  if (!http.begin(DASHBOARD_URL)) {
    dashboardConnected = false;
    Serial.println("Could not open dashboard URL; check laptop IP and port.");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  int status = http.POST(body);
  dashboardConnected = (status == HTTP_CODE_OK);
  Serial.printf("Dashboard POST -> HTTP %d\n", status);
  bool forecastValid = false;
  if (status == HTTP_CODE_OK) {
    String response = http.getString();
    DynamicJsonDocument doc(8192);
    DeserializationError error = deserializeJson(doc, response);
    if (!error) {
      JsonVariant chance = doc["readings"]["rain_probability"];
      JsonVariant amount = doc["readings"]["expected_rainfall"];
      const char* mode = doc["readings"]["pump_mode"] | "auto";
      manualMode = (String(mode) == "manual");
      manualCommandOn = doc["readings"]["pump_command"] | false;
      if (manualStopPending) manualStopPending = false;
      if (!chance.isNull() && !amount.isNull()) {
        rainChance = chance.as<float>();
        rainfallMm = amount.as<float>();
        forecastValid = true;
      }
    } else {
      Serial.printf("Dashboard JSON parse failed: %s\n", error.c_str());
    }
  } else if (status < 0) {
    Serial.print("HTTP connection error: ");
    Serial.println(http.errorToString(status));
    Serial.print("Target URL: "); Serial.println(DASHBOARD_URL);
  } else {
    Serial.println(http.getString());
  }
  http.end();
  if (forecastValid) Serial.printf("Next 6h rain: %.0f%%, %.1f mm\n", rainChance, rainfallMm);
  else Serial.println("Forecast unavailable; automatic watering stays off.");
  return forecastValid;
}

void controlPump(int soilPercent, bool forecastValid, float rainChance, float rainfallMm) {
  unsigned long now = millis();
  if (now - cycleWindowStartedAt >= CYCLE_WINDOW_MS) {
    cycleWindowStartedAt = now;
    pumpCyclesThisHour = 0;
  }

  if (!dashboardConnected) {
    if (pumpOn) setPump(false, "dashboard connection lost");
    if (manualMode && manualCommandOn) manualStopPending = true;
    return;
  }

  // Manual commands follow the dashboard switch until it is turned OFF.
  // The auto run, cooldown, and hourly caps do not apply in manual mode.
  if (manualMode) {
    if (!manualCommandOn) {
      if (pumpOn) setPump(false, "manual stop command");
      return;
    }
    if (!pumpOn) setPump(true, "manual dashboard command");
    else pumpStartedManual = true; // manual mode takes over an active auto run
    return;
  }

  // Fail safe: stop on missing forecast, rain expected, or adequate moisture.
  if (!forecastValid) {
    if (pumpOn) setPump(false, "forecast unavailable");
    return;
  }
  if (rainChance >= RAIN_DELAY_CHANCE_PERCENT || rainfallMm >= RAIN_DELAY_AMOUNT_MM) {
    if (pumpOn) setPump(false, "rain forecast");
    return;
  }
  if (soilPercent >= DRY_SOIL_PERCENT) {
    if (pumpOn) setPump(false, "soil moisture recovered");
    return;
  }

  if (pumpOn) {
    if (now - pumpStartedAt >= MAX_PUMP_RUN_MS) setPump(false, "10-second run limit");
    return;
  }
  if ((long)(now - nextPumpAllowedAt) < 0) return;
  if (pumpCyclesThisHour >= MAX_CYCLES_PER_HOUR) {
    Serial.println("Pump paused: hourly cycle limit reached.");
    return;
  }
  setPump(true, "dry soil and no significant rain");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("SMART IRRIGATION — AUTOMATIC PUMP");

  // Set relay OFF immediately at boot; motor power must use the relay contacts,
  // never an ESP32 GPIO. Use a suitable external motor supply.
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  pumpOn = false;

  analogReadResolution(12); // ESP32 ADC range: 0..4095
  dht.begin();
  Serial.println("DHT11: STARTED (GPIO 26)");
  Wire.begin(OLED_SDA, OLED_SCL);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  Serial.println(oledReady ? "OLED: OK" : "OLED: FAILED (continuing)");
  Serial.println("Relay GPIO 14: active-low, OFF at boot");

  connectWiFi();
  cycleWindowStartedAt = millis();
  // Clear a possibly stale ON command on the first successful API post.
  manualStopPending = true;
  lastSent = millis() - SEND_EVERY_MS;
}

void loop() {
  if (millis() - lastSent < SEND_EVERY_MS) {
    // Keep the pump run limit responsive between sensor/network updates.
    if (pumpOn && !manualMode && millis() - pumpStartedAt >= MAX_PUMP_RUN_MS) {
      setPump(false, "10-second run limit");
    }
    delay(20);
    return;
  }
  lastSent = millis();

  int soilRaw = analogRead(SOIL_PIN);
  int soilPercent = map(soilRaw, 3300, 2100, 0, 100); // user's calibration
  soilPercent = constrain(soilPercent, 0, 100);
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  float rainChance = -1.0f;
  float rainfallMm = -1.0f;

  Serial.println("--------------------------------");
  Serial.printf("SOIL RAW       : %d\n", soilRaw);
  Serial.printf("SOIL MOISTURE  : %d %%\n", soilPercent);
  if (isnan(temperature) || isnan(humidity)) Serial.println("DHT11          : FAILED");
  else {
    Serial.printf("TEMPERATURE    : %.1f C\n", temperature);
    Serial.printf("HUMIDITY       : %.1f %%\n", humidity);
  }

  bool forecastValid = postReadings(soilPercent, soilRaw, temperature, humidity,
                                    rainChance, rainfallMm);
  controlPump(soilPercent, forecastValid, rainChance, rainfallMm);
  updateOLED(soilPercent, soilRaw, temperature, humidity,
             rainChance, rainfallMm, forecastValid);
}
