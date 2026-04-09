#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>

// =========================
// CONFIG
// =========================
const char* AP_SSID = "Bin-Setup";
const char* AP_PASSWORD = "12345678";

const char* API_BASE_URL = "https://api.mihashi.me";
const char* PAIR_ENDPOINT = "/api/bins/pair";
const char* TELEMETRY_ENDPOINT = "/api/telemetry";

String firmwareVersion = "1.0.0";
String cnnModelVersion = "cnn-v1";

// exactly 5 minutes
const unsigned long TELEMETRY_INTERVAL_MS = 300000;
unsigned long lastTelemetrySent = 0;

// NTP
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";
const long GMT_OFFSET_SEC = 0;
const int DAYLIGHT_OFFSET_SEC = 0;

// =========================
// GLOBALS
// =========================
WebServer server(80);
Preferences prefs;
WiFiClientSecure secureClient;

String esp32ChipId;

String wifiSsid = "";
String wifiPassword = "";
String deviceKey = "";

bool isPaired = false;
bool wifiConfigured = false;
bool timeSynced = false;

// =========================
// HELPERS
// =========================
String getEsp32ChipId() {
  uint64_t chipid = ESP.getEfuseMac();
  char chipIdStr[32];
  snprintf(
    chipIdStr,
    sizeof(chipIdStr),
    "ESP32-%04X%08X",
    (uint16_t)(chipid >> 32),
    (uint32_t)chipid
  );
  return String(chipIdStr);
}

String makeUrl(const char* endpoint) {
  return String(API_BASE_URL) + String(endpoint);
}

void loadPreferences() {
  prefs.begin("smartbin", true);
  wifiSsid = prefs.getString("ssid", "");
  wifiPassword = prefs.getString("pass", "");
  deviceKey = prefs.getString("devKey", "");
  isPaired = prefs.getBool("paired", false);
  prefs.end();

  wifiConfigured = wifiSsid.length() > 0;

  Serial.println("=== Loaded Preferences ===");
  Serial.println("SSID: " + wifiSsid);
  Serial.println("Paired: " + String(isPaired ? "true" : "false"));
  Serial.println("Device key exists: " + String(deviceKey.length() > 0 ? "true" : "false"));
}

void saveWiFiAndPairingData(const String& ssid, const String& pass, const String& code) {
  prefs.begin("smartbin", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("pairCode", code);
  prefs.end();

  wifiSsid = ssid;
  wifiPassword = pass;
  wifiConfigured = true;
}

void saveDeviceKey(const String& key) {
  prefs.begin("smartbin", false);
  prefs.putString("devKey", key);
  prefs.putBool("paired", true);
  prefs.remove("pairCode");
  prefs.end();

  deviceKey = key;
  isPaired = true;
}

void clearPairing() {
  prefs.begin("smartbin", false);
  prefs.remove("devKey");
  prefs.remove("pairCode");
  prefs.putBool("paired", false);
  prefs.end();

  deviceKey = "";
  isPaired = false;
}

void clearAllConfig() {
  prefs.begin("smartbin", false);
  prefs.clear();
  prefs.end();

  wifiSsid = "";
  wifiPassword = "";
  deviceKey = "";
  isPaired = false;
  wifiConfigured = false;
}

bool connectToWiFi() {
  if (!wifiConfigured) return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

  Serial.println("[WiFi] Connecting...");
  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Connected");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("[WiFi] Failed");
  return false;
}

bool syncTimeUTC() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);

  struct tm timeinfo;
  Serial.println("[Time] Syncing NTP...");

  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&timeinfo, 1000)) {
      Serial.println("[Time] Synced");
      timeSynced = true;
      return true;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  Serial.println("[Time] Failed to sync");
  timeSynced = false;
  return false;
}

String getISO8601Timestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "1970-01-01T00:00:00.000Z";
  }

  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  return String(buf) + ".000Z";
}

// =========================
// RANDOM TELEMETRY GENERATORS
// =========================
float randFloat(float minVal, float maxVal) {
  long scaledMin = (long)(minVal * 10);
  long scaledMax = (long)(maxVal * 10);
  long value = random(scaledMin, scaledMax + 1);
  return value / 10.0f;
}

int randInt(int minVal, int maxVal) {
  return random(minVal, maxVal + 1);
}

// For mock data
const float COMPARTMENT_CAPACITY_LITERS[4] = {20.0, 20.0, 20.0, 20.0};

void appendRandomCompartments(JsonArray arr) {
  for (int i = 0; i < 4; i++) {
    int fillPercent = randInt(15, 95);
    float fillLiters = (COMPARTMENT_CAPACITY_LITERS[i] * fillPercent) / 100.0f;
    float distance = randFloat(4.0, 28.0);

    JsonObject compartment = arr.add<JsonObject>();
    compartment["slot"] = i + 1;
    compartment["fillPercent"] = fillPercent;
    compartment["fillLiters"] = fillLiters;

    JsonObject raw = compartment.createNestedObject("raw");
    raw["distance"] = distance;

    String sensorId = "HC-SR04-" + String(i + 1);
    raw["sensorId"] = sensorId;
  }
}

// =========================
// PAIRING API CALL
// =========================
bool verifyPairingCodeWithServer() {
  if (WiFi.status() != WL_CONNECTED) return false;

  prefs.begin("smartbin", true);
  String storedCode = prefs.getString("pairCode", "");
  prefs.end();

  if (storedCode.length() == 0) {
    Serial.println("[Pairing] No pairing code stored");
    return false;
  }

  HTTPClient http;
  String url = makeUrl(PAIR_ENDPOINT);

  secureClient.setInsecure(); // use cert validation later in production
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument reqDoc(512);
  reqDoc["code"] = storedCode;
  reqDoc["esp32ChipId"] = esp32ChipId;
  reqDoc["firmwareVersion"] = firmwareVersion;
  reqDoc["cnnModelVersion"] = cnnModelVersion;

  String requestBody;
  serializeJson(reqDoc, requestBody);

  Serial.println("[Pairing] Request:");
  Serial.println(requestBody);

  int httpCode = http.POST(requestBody);
  String response = http.getString();

  Serial.print("[Pairing] HTTP Code: ");
  Serial.println(httpCode);
  Serial.print("[Pairing] Response: ");
  Serial.println(response);

  if (httpCode != 200) {
    http.end();
    return false;
  }

  DynamicJsonDocument resDoc(512);
  DeserializationError err = deserializeJson(resDoc, response);
  if (err) {
    Serial.print("[Pairing] JSON parse error: ");
    Serial.println(err.c_str());
    http.end();
    return false;
  }

  String key = resDoc["data"]["key"] | "";
  if (key.length() == 0) {
    Serial.println("[Pairing] Key missing");
    http.end();
    return false;
  }

  saveDeviceKey(key);
  Serial.println("[Pairing] Success. Key stored.");
  http.end();
  return true;
}

// =========================
// TELEMETRY
// =========================
bool sendTelemetry() {
  if (WiFi.status() != WL_CONNECTED || !isPaired || deviceKey.length() == 0) {
    return false;
  }

  HTTPClient http;
  String url = makeUrl(TELEMETRY_ENDPOINT);

  secureClient.setInsecure();
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-id", esp32ChipId);
  http.addHeader("x-device-key", deviceKey);

  DynamicJsonDocument doc(2048);

  doc["timestamp"] = getISO8601Timestamp();

  JsonArray compartments = doc.createNestedArray("compartments");
  appendRandomCompartments(compartments);

  JsonObject power = doc.createNestedObject("power");
  power["batteryPercent"] = randInt(35, 100);
  power["isCharging"] = (randInt(0, 1) == 1);

  JsonObject connectivity = doc.createNestedObject("connectivity");
  connectivity["rssi"] = WiFi.RSSI();
  connectivity["ip"] = WiFi.localIP().toString();
  connectivity["network"] = WiFi.SSID();

  JsonObject env = doc.createNestedObject("env");
  env["temperatureC"] = randFloat(27.0, 36.5);
  env["humidity"] = randFloat(55.0, 82.0);

  String body;
  serializeJson(doc, body);

  Serial.println("[Telemetry] Sending:");
  Serial.println(body);

  int httpCode = http.POST(body);
  String response = http.getString();

  Serial.print("[Telemetry] HTTP Code: ");
  Serial.println(httpCode);
  Serial.print("[Telemetry] Response: ");
  Serial.println(response);

  if (httpCode == 200 || httpCode == 201 || httpCode == 204) {
    http.end();
    return true;
  }

  if (httpCode == 401 || httpCode == 403) {
    Serial.println("[Telemetry] Unauthorized. Clearing pairing.");
    clearPairing();
    http.end();
    return false;
  }

  http.end();
  return false;
}

// =========================
// AP WEB INTERFACE
// =========================
String htmlPage(const String& message = "") {
  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Mihashi Bin Setup</title>
  <style>
    body { font-family: Arial, sans-serif; padding: 24px; background:#f7f7f7; }
    .card { max-width: 420px; margin:auto; background:#fff; padding:24px; border-radius:16px; box-shadow:0 4px 20px rgba(0,0,0,0.08); }
    h2 { margin-top:0; }
    input { width:100%; padding:12px; margin:8px 0 16px 0; border:1px solid #ccc; border-radius:10px; }
    button { width:100%; padding:12px; border:none; border-radius:10px; background:#00bc7d; color:#fff; font-weight:bold; cursor:pointer; }
    .msg { margin-bottom:16px; color:#d14; font-size:14px; }
    .info { font-size:13px; color:#555; margin-top:12px; }
  </style>
</head>
<body>
  <div class="card">
    <h2>Mihashi Smart Bin Setup</h2>
    <div class="msg">)rawliteral";

  page += message;

  page += R"rawliteral(</div>
    <form method="POST" action="/save">
      <label>Wi-Fi SSID</label>
      <input name="ssid" type="text" required>

      <label>Wi-Fi Password</label>
      <input name="password" type="password" required>

      <label>6-digit Pairing Code</label>
      <input name="code" type="text" maxlength="6" required>

      <button type="submit">Save & Pair</button>
    </form>
    <div class="info">
      Device ID: )rawliteral";

  page += esp32ChipId;

  page += R"rawliteral(
    </div>
  </div>
</body>
</html>
)rawliteral";

  return page;
}

void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

void handleSave() {
  if (!server.hasArg("ssid") || !server.hasArg("password") || !server.hasArg("code")) {
    server.send(400, "text/html", htmlPage("Missing required fields"));
    return;
  }

  String ssid = server.arg("ssid");
  String password = server.arg("password");
  String code = server.arg("code");

  saveWiFiAndPairingData(ssid, password, code);
  server.send(200, "text/html", htmlPage("Saved. Device will now connect and pair."));

  delay(1500);
  server.stop();
  WiFi.softAPdisconnect(true);

  if (!connectToWiFi()) {
    Serial.println("[Setup] WiFi failed after form submit");
    return;
  }

  syncTimeUTC();

  if (!verifyPairingCodeWithServer()) {
    Serial.println("[Setup] Pairing failed");
    clearPairing();
  }
}

void handleReset() {
  clearAllConfig();
  server.send(200, "text/plain", "All configuration cleared. Restarting...");
  delay(1000);
  ESP.restart();
}

void startAccessPointMode() {
  Serial.println("[AP] Starting AP mode...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  IPAddress ip = WiFi.softAPIP();
  Serial.print("[AP] IP address: ");
  Serial.println(ip);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reset", HTTP_GET, handleReset);
  server.begin();

  Serial.println("[AP] Web server started");
}

// =========================
// SETUP
// =========================
void setup() {
  Serial.begin(115200);
  delay(1000);

  randomSeed(micros());
  esp32ChipId = getEsp32ChipId();

  Serial.println("==================================");
  Serial.println("Mihashi Smart Bin Boot");
  Serial.println("ESP32 Chip ID: " + esp32ChipId);
  Serial.println("==================================");

  loadPreferences();

  if (wifiConfigured) {
    if (connectToWiFi()) {
      syncTimeUTC();

      if (!isPaired) {
        bool paired = verifyPairingCodeWithServer();
        if (!paired) {
          Serial.println("[Boot] Pairing failed. Starting AP mode.");
          startAccessPointMode();
        }
      }
    } else {
      Serial.println("[Boot] WiFi connection failed. Starting AP mode.");
      startAccessPointMode();
    }
  } else {
    Serial.println("[Boot] No WiFi configured. Starting AP mode.");
    startAccessPointMode();
  }
}

// =========================
// LOOP
// =========================
void loop() {
  if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    server.handleClient();
  }

  if (WiFi.status() == WL_CONNECTED && isPaired) {
    if (millis() - lastTelemetrySent >= TELEMETRY_INTERVAL_MS) {
      lastTelemetrySent = millis();
      sendTelemetry();
    }
  }

  delay(50);
}