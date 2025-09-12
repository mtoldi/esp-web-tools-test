/*
  Inkplate6FLICK OpenMeteo Weather Station — SERIAL provisioning (Improv)
  - Wi-Fi credsi preko USB/serial (ESP Web Tools Improv UI)
  - Ostale postavke (username/city/TZ/lat/lon/metric) preko HTTP /provision
  - FIX: WebServer se pokreće tek nakon što je Wi-Fi spojen (izbjegava LWIP assert)
  - Clean reboot nakon /provision radi konzistentnog rendera (font/GUI)
*/

#ifndef ARDUINO_INKPLATE6FLICK
#error "Wrong board selection for this example, please select Soldered Inkplate 6 FLICK."
#endif

#include "src/includes.h"        // Inkplate lib + Network/Gui/WeatherData
#include <WiFi.h>
#include <WebServer.h>           // ESP32 core
#include <Preferences.h>         // NVS storage
#include <ArduinoJson.h>         // ArduinoJson
#include <ImprovWiFiLibrary.h>   // Improv (jnthas/Improv-WiFi-Library API)

// ---------- Defaults (fallback; prepisuju se iz NVS-a ili /provision) ----------
String myUsername = "Username";
String myCity     = "Osijek";
int    timeZone   = 2;            // UTC offset hours
float  latitude   = 45.5550;
float  longitude  = 18.6955;
bool   metricUnits = true;

// Wi-Fi (ako nema u NVS-u, Improv će ih postaviti)
String wifiSsid   = "";
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
bool serverStarted = false;
volatile bool provisionedNow = false;
volatile bool wifiChangedNow = false;

// ---------- Improv ----------
ImprovWiFi improvSerial(&Serial); // ova lib traži Stream*
String device_url;                // npr. "http://192.168.1.123"

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

void saveConfigKV(const char* key, const String& val) {
  prefs.begin("app", false);
  prefs.putString(key, val);
  prefs.end();
}

void saveConfig(const JsonVariantConst& v) {
  prefs.begin("app", false);
  if (v.containsKey("username"))    prefs.putString("username", String((const char*)v["username"]));
  if (v.containsKey("city"))        prefs.putString("city",     String((const char*)v["city"]));
  if (v.containsKey("timeZone"))    prefs.putInt("tz",          (int)v["timeZone"]);
  if (v.containsKey("latitude"))    prefs.putDouble("lat",      (double)v["latitude"]);
  if (v.containsKey("longitude"))   prefs.putDouble("lon",      (double)v["longitude"]);
  if (v.containsKey("metricUnits")) prefs.putBool("metric",     (bool)v["metricUnits"]);
  if (v.containsKey("ssid"))        prefs.putString("ssid",     String((const char*)v["ssid"]));
  if (v.containsKey("password"))    prefs.putString("pass",     String((const char*)v["password"]));
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
  doc["ip"]         = WiFi.isConnected() ? WiFi.localIP().toString() : "";
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

  // Kratko pričekaj da browser primi odgovor pa clean reboot (fix GFX/font)
  delay(400);
  ESP.restart();
}

// =================== HTTP server control ===================
void startHttpServer() {
  if (serverStarted) return;
  server.on("/info",       HTTP_GET,     handleInfo);
  server.on("/provision",  HTTP_OPTIONS, handleProvision);
  server.on("/provision",  HTTP_POST,    handleProvision);
  server.begin();
  serverStarted = true;
  Serial.println("[HTTP] Server started");
}

// =================== Wi-Fi ===================
bool connectWiFiSTA(const String& ssid, const String& pass, uint32_t waitMs = 20000) {
  if (ssid.isEmpty()) return false;
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (millis() - t0 < waitMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(200);
  }
  return false;
}

// Po želji: custom connect za Improv (da mi upravljamo spajanjem)
bool myImprovConnect(const char* ssid, const char* pass) {
  bool ok = connectWiFiSTA(String(ssid), String(pass), 20000);
  if (ok) {
    device_url = String("http://") + WiFi.localIP().toString();
    startHttpServer(); // server tek kad je Wi-Fi gore
  }
  return ok;
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

// =================== Improv callback (Wi-Fi credsi preko serijskog) ===================
void onImprovConnectedCb(const char* ssid, const char* password) {
  // spremi u NVS
  saveConfigKV("ssid", String(ssid));
  saveConfigKV("pass", String(password));
  wifiSsid = String(ssid);
  wifiPass = String(password);

  bool ok = connectWiFiSTA(wifiSsid, wifiPass, 20000);
  if (ok) {
    device_url = String("http://") + WiFi.localIP().toString();
    startHttpServer(); // pokreni HTTP tek sad
    Serial.print("[Improv] Connected. URL: "); Serial.println(device_url);
  } else {
    Serial.println("[Improv] Unable to connect to Wi-Fi.");
  }
}

// =================== SETUP ===================
void setup() {
  Serial.begin(115200);
  Serial.println("[FW] Inkplate6 FLICK — SERIAL provisioning " __DATE__ " " __TIME__);

  // FIX: inicijaliziraj netif rano (sprječava LWIP assert)
  WiFi.mode(WIFI_STA);

  inkplate.begin();
  inkplate.clearDisplay();

  loadConfig();

  // ---- Improv init (serial Wi-Fi) ----
  improvSerial.setDeviceInfo(
    ImprovTypes::ChipFamily::CF_ESP32, // chip family
    "Inkplate OpenMeteo",              // firmware name
    "1.0.0",                           // version
    "Soldered Inkplate 6F",            // device name (UI)
    "http://{LOCAL_IPV4}"              // URL koji će ESP Web Tools prikazati
  );

  improvSerial.onImprovConnected(onImprovConnectedCb);
  improvSerial.setCustomConnectWiFi(myImprovConnect);

  // ---- Ako već imamo Wi-Fi u NVS-u, pokušaj odmah ----
  bool wifiOK = false;
  if (wifiSsid.length()) {
    wifiOK = connectWiFiSTA(wifiSsid, wifiPass, 20000);
    if (wifiOK) {
      device_url = String("http://") + WiFi.localIP().toString();
      startHttpServer(); // TEK sad pokreni server
      Serial.print("[BOOT] Wi-Fi OK. URL: "); Serial.println(device_url);
    } else {
      Serial.println("[BOOT] Waiting for Improv Wi-Fi credentials over Serial...");
    }
  } else {
    Serial.println("[BOOT] No Wi-Fi in NVS. Waiting for Improv credentials...");
  }

  // Ako smo online, odmah render
  if (wifiOK) {
    renderOnce();
  } else {
    // Ako offline, kratki hint na ekranu (nije AP; samo informacija)
    inkplate.setTextSize(2);
    inkplate.setTextColor(BLACK);
    inkplate.setCursor(10, 30);
    inkplate.print("Use installer (USB)");
    inkplate.setCursor(10, 60);
    inkplate.print("to send Wi-Fi");
    inkplate.display();  // full refresh
  }

  // "Config window": ~30 s za Improv + eventualni /provision (ako smo online)
  const uint32_t windowMs = 30000;
  uint32_t t0 = millis();
  while (millis() - t0 < windowMs) {
    if (serverStarted && WiFi.isConnected()) server.handleClient();
    improvSerial.handleSerial(); // servisiraj Improv protokol
    delay(10);
  }

  // Deep sleep do sljedećeg ciklusa
  esp_sleep_enable_timer_wakeup((uint64_t)TIME_TO_SLEEP * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

void loop() {
  // ne koristimo loop jer idemo u deep sleep
}
