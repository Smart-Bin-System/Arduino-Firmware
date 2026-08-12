#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include "esp_camera.h"

// ---------------------------------------------------------------------------
// Editable network configuration
// ---------------------------------------------------------------------------
constexpr char S3_AP_SSID[] = "ESP32-S3-N16R8";
// This value must remain identical to AP_PASSWORD in ESP32_S3_Smart_Bin.ino.
constexpr char S3_AP_PASSWORD[] = "change-this-password";

const IPAddress CAMERA_IP(192, 168, 4, 2);
const IPAddress S3_GATEWAY(192, 168, 4, 1);
const IPAddress NETWORK_SUBNET(255, 255, 255, 0);

constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 5000;

// ---------------------------------------------------------------------------
// Standard AI Thinker ESP32-CAM pin mapping
// ---------------------------------------------------------------------------
constexpr int PWDN_GPIO_NUM = 32;
constexpr int RESET_GPIO_NUM = -1;
constexpr int XCLK_GPIO_NUM = 0;
constexpr int SIOD_GPIO_NUM = 26;
constexpr int SIOC_GPIO_NUM = 27;

constexpr int Y9_GPIO_NUM = 35;
constexpr int Y8_GPIO_NUM = 34;
constexpr int Y7_GPIO_NUM = 39;
constexpr int Y6_GPIO_NUM = 36;
constexpr int Y5_GPIO_NUM = 21;
constexpr int Y4_GPIO_NUM = 19;
constexpr int Y3_GPIO_NUM = 18;
constexpr int Y2_GPIO_NUM = 5;
constexpr int VSYNC_GPIO_NUM = 25;
constexpr int HREF_GPIO_NUM = 23;
constexpr int PCLK_GPIO_NUM = 22;

WebServer httpServer(80);
uint32_t lastReconnectAttemptAt = 0;
bool wifiWasConnected = false;

bool deadlineReached(uint32_t deadline) {
  return static_cast<int32_t>(millis() - deadline) >= 0;
}

bool initializeCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // QVGA provides enough detail for a 160x160 model while keeping transfers
  // and S3 decode memory modest. Lower values mean higher JPEG quality.
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.fb_count = 2;
    Serial.println("[Camera] PSRAM detected; using two PSRAM frame buffers");
  } else {
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
    Serial.println("[Camera] PSRAM unavailable; using one DRAM frame buffer");
  }

  const esp_err_t result = esp_camera_init(&config);
  if (result != ESP_OK) {
    Serial.printf("[Camera] Initialization failed: 0x%X\n", result);
    return false;
  }

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor) {
    sensor->set_framesize(sensor, FRAMESIZE_QVGA);
    sensor->set_quality(sensor, 12);
  }

  Serial.println("[Camera] AI Thinker camera ready at 320x240 JPEG");
  return true;
}

void connectToS3AccessPoint() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  if (!WiFi.config(CAMERA_IP, S3_GATEWAY, NETWORK_SUBNET)) {
    Serial.println("[WiFi] Static IP configuration failed");
  }

  Serial.printf("[WiFi] Connecting to %s\n", S3_AP_SSID);
  WiFi.begin(S3_AP_SSID, S3_AP_PASSWORD);
  lastReconnectAttemptAt = millis();
}

void serviceWiFiReconnect() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.printf("[WiFi] Connected; camera URL: http://%s/capture\n",
                    WiFi.localIP().toString().c_str());
    }
    return;
  }
  wifiWasConnected = false;
  if (!deadlineReached(lastReconnectAttemptAt + WIFI_RECONNECT_INTERVAL_MS)) {
    return;
  }

  lastReconnectAttemptAt = millis();
  Serial.println("[WiFi] Connection lost; reconnecting to ESP32-S3 AP");
  WiFi.disconnect();
  WiFi.begin(S3_AP_SSID, S3_AP_PASSWORD);
}

void handleCapture() {
  camera_fb_t* frame = esp_camera_fb_get();
  if (!frame) {
    Serial.println("[HTTP] Camera capture failed");
    httpServer.send(503, "text/plain", "Camera capture failed");
    return;
  }

  if (frame->format != PIXFORMAT_JPEG || frame->len == 0) {
    Serial.println("[HTTP] Camera returned an invalid frame");
    esp_camera_fb_return(frame);
    httpServer.send(500, "text/plain", "Invalid camera frame");
    return;
  }

  httpServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  httpServer.sendHeader("Content-Disposition", "inline; filename=capture.jpg");
  httpServer.setContentLength(frame->len);
  httpServer.send(200, "image/jpeg", "");

  WiFiClient client = httpServer.client();
  size_t sent = 0;
  while (sent < frame->len && client.connected()) {
    const size_t written = client.write(frame->buf + sent, frame->len - sent);
    if (written == 0) break;
    sent += written;
  }

  Serial.printf("[HTTP] Captured %u bytes; sent %u bytes\n",
                static_cast<unsigned>(frame->len),
                static_cast<unsigned>(sent));
  esp_camera_fb_return(frame);
}

void startHttpServer() {
  httpServer.on("/", HTTP_GET, []() {
    httpServer.send(200, "text/plain", "ESP32-CAM ready");
  });
  httpServer.on("/capture", HTTP_GET, handleCapture);
  httpServer.onNotFound([]() {
    httpServer.send(404, "text/plain", "Not found");
  });
  httpServer.begin();
  Serial.println("[HTTP] Server listening on port 80");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32-CAM Backup Image Source ===");

  if (!initializeCamera()) {
    Serial.println("[Fatal] Camera unavailable; restarting in 5 seconds");
    delay(5000);
    ESP.restart();
  }

  connectToS3AccessPoint();
  startHttpServer();
}

void loop() {
  httpServer.handleClient();
  serviceWiFiReconnect();
  delay(2);
}
