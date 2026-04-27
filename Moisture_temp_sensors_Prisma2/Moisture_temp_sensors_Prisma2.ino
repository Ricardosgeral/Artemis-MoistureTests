#include <Arduino.h>
#include <U8g2lib.h>
#include <DS18B20.h>
#include <algorithm>
#include <WiFi.h>
#include <esp_eap_client.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include "arduino_secrets.h"

// ─── GLOBALS ──────────────────────────────────────────────────────────────────
const unsigned long INTERVAL = 30000UL - 420UL;  // ~30 seconds

unsigned long readingCount = 0;
unsigned long lastPublish  = 0;

char timeStr[25] = "NO TIME";

#define SENSOR_POWER_PIN 5  // transistor switch for resistive sensors

// ─── WIFI MODE ────────────────────────────────────────────────────────────────
#define USE_ENTERPRISE_WIFI true

// ─── ENTERPRISE WiFi (WPA2-EAP / 802.1X) ─────────────────────────────────────
const char* EAP_SSID = SECRET_EAP_SSID;
const char* EAP_ID   = SECRET_EAP_ID;
const char* EAP_USER = SECRET_EAP_USER;
const char* EAP_PASS = SECRET_EAP_PASS;

// ─── PERSONAL WiFi (WPA2-PSK) ─────────────────────────────────────────────────
const char* PSK_SSID = SECRET_PSK_SSID;
const char* PSK_PASS = SECRET_PSK_PASS;

// ─── MQTT ─────────────────────────────────────────────────────────────────────
const char* mqtt_server = "5af0da191bba4efe89e7bdd05d71b054.s1.eu.hivemq.cloud";
const char* mqtt_user   = SECRET_MQTT_USER;
const char* mqtt_pass   = SECRET_MQTT_PASS;
const int   mqtt_port   = 8883;
const char* mqtt_topic  = "/Set2";

// ─── OLED ─────────────────────────────────────────────────────────────────────
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(
  U8G2_R0, U8X8_PIN_NONE, 8, 10);

// ─── SENSORS ──────────────────────────────────────────────────────────────────
const int sensorPins[4] = { 4, 3, 0, 1 };
const int NUM_SAMPLES   = 30;

DS18B20 ds(6);
uint8_t address[8];
uint8_t selected = 0;

WiFiClientSecure espClient;
PubSubClient     mqttClient(espClient);

// ─── HELPERS ──────────────────────────────────────────────────────────────────
int readMedianADC(int pin) {
  int samples[NUM_SAMPLES];
  for (int i = 0; i < NUM_SAMPLES; i++) {
    samples[i] = analogRead(pin);
    delay(2);
  }
  std::sort(samples, samples + NUM_SAMPLES);
  return samples[NUM_SAMPLES / 2];
}

// ─── WiFi ─────────────────────────────────────────────────────────────────────
void setup_wifi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);

#if USE_ENTERPRISE_WIFI
  Serial.println("WiFi mode: Enterprise (WPA2-EAP)");
  Serial.print("Connecting to SSID: ");
  Serial.println(EAP_SSID);

  esp_eap_client_set_identity((uint8_t*)EAP_ID,   strlen(EAP_ID));
  esp_eap_client_set_username((uint8_t*)EAP_USER, strlen(EAP_USER));
  esp_eap_client_set_password((uint8_t*)EAP_PASS, strlen(EAP_PASS));
  esp_wifi_sta_enterprise_enable();
  WiFi.begin(EAP_SSID);
#else
  Serial.println("WiFi mode: Personal (WPA2-PSK)");
  Serial.print("Connecting to SSID: ");
  Serial.println(PSK_SSID);
  WiFi.begin(PSK_SSID, PSK_PASS);
#endif

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++attempts > 40) {
      Serial.println("\nWiFi timed out! Restarting...");
      ESP.restart();
    }
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
}

// ─── MQTT ─────────────────────────────────────────────────────────────────────
void reconnect_mqtt() {
  int attempts = 0;
  while (!mqttClient.connected()) {

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost during MQTT reconnect, reconnecting WiFi...");
      setup_wifi();
    }

    Serial.print("Connecting to MQTT...");
    String clientId = "C3Mini-" + String(random(0xffff), HEX);

    if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("connected");
    } else {
      Serial.printf("failed rc=%d, retrying in 2s\n", mqttClient.state());
      if (++attempts > 10) {
        Serial.println("MQTT failed too many times, restarting...");
        ESP.restart();
      }
      delay(2000);
    }
  }
}

// ─── PUBLISH ──────────────────────────────────────────────────────────────────
void publishSensorData(int v[4]) {
  StaticJsonDocument<200> doc;
  doc["C3"]  = v[0];
  doc["C4"]  = v[1];
  doc["W1"]  = v[2];
  doc["R1n"] = v[3];

  time_t now;
  time(&now);
  char ts[25];
  strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", localtime(&now));
  doc["ts"] = ts;

  char payload[200];
  serializeJson(doc, payload);

  if (mqttClient.publish(mqtt_topic, payload, true)) {
    Serial.println("MQTT published: " + String(payload));
  } else {
    Serial.println("MQTT publish failed");
  }
}

// ─── SETUP ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(2000);

  pinMode(LED_BUILTIN,      OUTPUT);
  pinMode(SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(SENSOR_POWER_PIN, LOW);

  analogReadResolution(12);
  for (int i = 0; i < 4; i++) {
    analogSetPinAttenuation(sensorPins[i], ADC_11db);
  }

  // ── Auto-detect DS18B20 ──
  Serial.println("Scanning for DS18B20...");
  if (ds.selectNext()) {
    ds.getAddress(address);
    Serial.print("Sensor found: {");
    for (int i = 0; i < 8; i++) {
      Serial.print(address[i]);
      if (i < 7) Serial.print(", ");
    }
    Serial.println("}");
    selected = ds.select(address);
    if (selected) ds.setResolution(12);
  } else {
    Serial.println("No DS18B20 found! Check wiring.");
  }

  u8g2.begin();

  setup_wifi();

  // Portugal time (WET/WEST)
  configTime(0, 3600, "pool.ntp.org");

  // Wait for NTP sync
  Serial.print("Waiting for NTP...");
  time_t now = 0;
  while (now < 1000000000UL) {
    delay(500);
    Serial.print(".");
    time(&now);
  }
  Serial.println(" synced");

  espClient.setInsecure();
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setKeepAlive(60);  // must exceed INTERVAL (30s)
}

// ─── LOOP ─────────────────────────────────────────────────────────────────────
void loop() {
  // ── Keep connections alive ──
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    setup_wifi();
  }
  if (!mqttClient.connected()) reconnect_mqtt();
  mqttClient.loop();

  // ── Non-blocking wait ──
  if (millis() - lastPublish < INTERVAL) return;
  lastPublish = millis();

  // ── Update timestamp ──
  time_t now;
  time(&now);
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", localtime(&now));

  digitalWrite(LED_BUILTIN, HIGH);

  // ── Read resistive sensors (power-cycled) ──
  digitalWrite(SENSOR_POWER_PIN, HIGH);
  delay(200);  // stabilize

  int v[4];
  for (int i = 0; i < 4; i++) {
    v[i] = readMedianADC(sensorPins[i]);
  }
  digitalWrite(SENSOR_POWER_PIN, LOW);

  Serial.printf("C3: %d  C4: %d  W1: %d  R1n: %d  Reading#: %lu\n",
                v[0], v[1], v[2], v[3], readingCount);

  // ── OLED display ──
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);

  const char* labels[] = { "C3", "C4", "W1", "R1n" };
  for (int i = 0; i < 4; i++) {
    u8g2.setCursor(0, 12 + i * 12);
    u8g2.print(labels[i]);
    u8g2.print(": ");
    u8g2.print(v[i]);
  }

  u8g2.setCursor(0, 12 + 4 * 12);
  u8g2.print("Time: ");
  u8g2.print(timeStr);

  u8g2.sendBuffer();

  // ── Publish ──
  publishSensorData(v);

  readingCount++;
  digitalWrite(LED_BUILTIN, LOW);
}