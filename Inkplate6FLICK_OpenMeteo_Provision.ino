/*
  Inkplate6FLICK OpenMeteo Weather Station (provision-enabled)
  Compatible with Soldered Inkplate 6 FLICK
*/

#ifndef ARDUINO_INKPLATE6FLICK
#error "Wrong board selection for this example, please select Soldered Inkplate 6 FLICK."
#endif

#include "src/includes.h"   // Inkplate lib + Network/Gui/WeatherData
#include <WiFi.h>
#include <WebServer.h>      // ESP32 core
#include <Preferences.h>    // NVS storage
#include <ArduinoJson.h>    // Library: "ArduinoJson"

// ---------- Defaults (fallback; korisnik će ih promijeniti preko /provision) ----------
String myUsername = "Username";
String myCity     = "Osijek";
int    timeZone   = 2;            // UTC offset hours
float  latitude   = 45.5550;
float  longitude  = 18.6955;
bool   metricUnits = true;

String wifiSsid   = "";           // prazno = nema spremljeno
String wifiPass   = "";

const char* ntpServer = "pool.ntp.org";

// ---------- Inkplate & app objekti ----------
Inkplate inkplate(INKPLATE_3BIT);
Network network;
Network::UserInfo userInfo;
WeatherData weatherData;
Gui gui(inkplate);

// ---------- Web & storage ----------
Preferences prefs;
WebServer server(80);
volatile bool provisionedNow = false;
volatile bool wifiChangedNow = false;

// ---------- Sleep ----------
#define uS_TO_S_FACTOR 1000000ULL
#define TIME_TO_SLEEP  1800      // 30 min

// =================== NVS helpers ===================
void loadConfig() {
  prefs.begin("app", true);
  myUsername = prefs.getString("username", myUsername);
  myCity     = prefs.getString("city", myCity);
  timeZone   = prefs.getInt("tz", timeZone);
  latitude   = prefs.getDouble("lat", latitude);
  longitude  = prefs.getDouble("lon", longitude);
  metricUnits= prefs.getBool("metric", metricUnits);
  wifiSsid   = prefs.getString("ssid", wifiSsid);
  wifiPass   = prefs.getString("pass", wifiPass);
  prefs.end();
}

void saveConfig(const JsonVariantConst& v) {
  prefs.begin("app", false);
  if (v.containsKey("username"))   prefs.putString("username", String((const char*)v["username"]));
  if (v.containsKey("city"))       prefs.putString("city",     String((const char*)v["city"]));
  if (v.containsKey("timeZone"))   prefs.putInt("tz",          (int)v["timeZone"]);
  if (v.containsKey("latitude"))   prefs.putDouble("lat",      (double)v["latitude"]);
  if (v.containsKey("longitude"))  prefs.putDouble("lon",      (double)v["longitude"]);
  if (v.containsKey("metricUnits"))prefs.putBool("metric",     (bool)v["metricUnits"]);
  if (v.containsKey("ssid"))       prefs.putString("ssid",     String((const char*)v["ssid"]));
  if (v.containsKey("password"))   prefs.putString("pass",     String((const char*)v["password"]));
  prefs.end();
}

// =================== CORS & endpoints ===================
void allowCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
}

void handleInfo() {
  allowCORS();
  StaticJsonDocument<384> doc;
  doc["username"]   = myUsername;
  doc["city"]       = myCity;
  doc["timeZone"]   = timeZone;
  doc["latitude"]   = latitude;
  doc["longitude"]  = longitude;
  doc["metricUnits"]= metricUnits;
  doc["ssid"]       = wifiSsid;
  doc["connected"]  = WiFi.isConnected();
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleProvision() {
  if (server.method() == HTTP_OPTIONS) { allowCORS(); server.send(204); return; }
  if (server.method() != HTTP_POST || !server.hasArg("plain")) {
    allowCORS(); server.send(400, "application/json", "{\"ok\":false,\"err\":\"bad_request\"}"); return;
  }

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) { allowCORS(); server.send(400, "application/json", "{\"ok\":false,\"err\":\"json\"}"); return; }

  bool ssidChanged = doc.containsKey("ssid")     && String((const char*)doc["ssid"])     != wifiSsid;
  bool passChanged = doc.containsKey("password") && String((const char*)doc["password"]) != wifiPass;

  saveConfig(doc.as<JsonVariantConst>());
  loadConfig();               // refresh RAM
  provisionedNow = true;
  wifiChangedNow = (ssidChanged || passChanged);

  allowCORS();
  server.send(200, "application/json", "{\"ok\":true}");
}

// =================== Wi-Fi connect / SoftAP ===================
bool connectWiFiSTA(const String& ssid, const String& pass, uint32_t waitMs = 20000) {
  if (ssid.isEmpty()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (millis() - t0 < waitMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(200);
  }
  return false;
}

String startSoftAP() {
  WiFi.mode(WIFI_AP_STA);
  String apSsid = "Inkplate-Setup-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
  WiFi.softAP(apSsid.c_str(), "inkplate123");
  return apSsid;
}

// =================== Crtanje ===================
void renderOnce() {
  gui.voltage = inkplate.readBattery();
  userInfo.city = myCity;
  userInfo.username = myUsername;
  userInfo.useMetric = metricUnits;

  configTime(timeZone * 3600, 0, ntpServer);
  network.fetchWeatherData(&weatherData, &userInfo, &latitude, &longitude);

  if (userInfo.apiError) {
    gui.apiError();
  } else {
    gui.drawBackground();
    gui.displayWeatherData(&weatherData, &userInfo);
  }
}

// =================== SETUP ===================
void setup() {
  Serial.begin(115200);
  Serial.println("[FW] Inkplate6 FLICK provision-enabled " __DATE__ " " __TIME__);

  inkplate.begin();
  inkplate.clearDisplay();

  loadConfig();

  // 1) Pokušaj STA ako imamo spremljene podatke
  bool wifiOK = connectWiFiSTA(wifiSsid, wifiPass, 20000);
  String apName;
  if (!wifiOK) {
    // 2) Digni SoftAP za konfiguraciju
    apName = startSoftAP();
    Serial.print("SoftAP: "); Serial.println(apName);
    Serial.print("AP IP: ");  Serial.println(WiFi.softAPIP());
  } else {
    Serial.print("WiFi STA OK, IP: "); Serial.println(WiFi.localIP());
  }

  // Web server
  server.on("/info",       HTTP_GET,     handleInfo);
  server.on("/provision",  HTTP_OPTIONS, handleProvision);
  server.on("/provision",  HTTP_POST,    handleProvision);
  server.begin();

  // Ako imamo Wi-Fi, odmah iscrtavanje
  if (wifiOK) {
    renderOnce();
  } else {
    // Nema Wi-Fi → pokaži kratke upute na ekranu
    inkplate.setTextSize(2);
    inkplate.setTextColor(BLACK);
    inkplate.setCursor(10, 30);
    inkplate.print("Config AP:");
    inkplate.setCursor(10, 60);
    inkplate.print(apName);
    inkplate.setCursor(10, 90);
    inkplate.print("Pass: inkplate123");
    inkplate.setCursor(10, 120);
    inkplate.print(WiFi.softAPIP().toString());
    inkplate.display();  // full refresh
  }

  // "Config prozor": 60 s slušaj /provision; ako stigne i Wi-Fi promijenjen → reconnect + render
  const uint32_t windowMs = 60000;
  uint32_t t0 = millis();
  while (millis() - t0 < windowMs) {
    server.handleClient();

    if (provisionedNow) {
      provisionedNow = false;

      if (wifiChangedNow) {
        wifiChangedNow = false;
        WiFi.disconnect(true, true);
        delay(500);
        wifiOK = connectWiFiSTA(wifiSsid, wifiPass, 20000);
        if (wifiOK) {
          Serial.print("WiFi STA OK, IP: "); Serial.println(WiFi.localIP());
        } else {
          Serial.println("WiFi STA connect failed after provision.");
        }
      }

      if (wifiOK) {
        renderOnce();
      } else {
        // i dalje bez Wi-Fi: osvježi “Config AP” ekran
        inkplate.clearDisplay();
        inkplate.setTextSize(2);
        inkplate.setTextColor(BLACK);
        inkplate.setCursor(10, 30);
        inkplate.print("Config AP:");
        inkplate.setCursor(10, 60);
        inkplate.print(apName);
        inkplate.setCursor(10, 90);
        inkplate.print("Pass: inkplate123");
        inkplate.setCursor(10, 120);
        inkplate.print(WiFi.softAPIP().toString());
        inkplate.display();
      }

      // produlji prozor još malo nakon uspješne konfiguracije
      t0 = millis();
    }

    delay(10);
  }

  // Deep sleep do sljedećeg ciklusa
  esp_sleep_enable_timer_wakeup((uint64_t)TIME_TO_SLEEP * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

void loop() {}
