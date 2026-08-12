#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <time.h>

#include "plastic_classifier_model.h"
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/version.h"

// Required Arduino libraries:
//   ArduinoJson, ESP32Servo, JPEGDEC, WebSockets, and a TensorFlow Lite Micro
//   library compatible with the installed ESP32-S3 Arduino core.

// ---------------------------------------------------------------------------
// Editable configuration
// ---------------------------------------------------------------------------
constexpr char AP_SSID[] = "ESP32-S3-N16R8";
constexpr char AP_PASSWORD[] = "change-this-password";
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

constexpr char API_BASE_URL[] = "https://api.mihashi.me";
constexpr char PAIR_ENDPOINT[] = "/api/bins/pair";
constexpr char TELEMETRY_ENDPOINT[] = "/api/telemetry";
constexpr char ESP32_CAM_CAPTURE_URL[] = "http://192.168.4.2/capture";

constexpr uint8_t PIR_PIN = 4;
constexpr uint8_t PERSON_TRIG_PIN = 5;
constexpr uint8_t PERSON_ECHO_PIN = 6;
constexpr float PERSON_TRIGGER_DISTANCE_CM = 50.0F;

constexpr uint8_t FILL_TRIG_PINS[4] = {7, 9, 11, 13};
constexpr uint8_t FILL_ECHO_PINS[4] = {8, 10, 12, 14};
// Replace these values with the measured internal depth of each compartment.
constexpr float COMPARTMENT_DEPTH_CM[4] = {40.0F, 40.0F, 40.0F, 40.0F};
constexpr float COMPARTMENT_CAPACITY_LITERS[4] = {60.0F, 60.0F, 60.0F, 60.0F};

constexpr uint8_t SERVO_PINS[4] = {15, 16, 17, 18};
constexpr int SERVO_CLOSED_ANGLE = 0;
constexpr int SERVO_OPEN_ANGLE = 150;
constexpr uint32_t SERVO_OPEN_DURATION_MS = 3000;

constexpr uint32_t PERSON_COOLDOWN_MS = 10000;
constexpr uint32_t TABLET_UPLOAD_TIMEOUT_MS = 10000;
constexpr uint32_t STA_RECONNECT_INTERVAL_MS = 15000;
constexpr uint32_t PAIR_RETRY_INTERVAL_MS = 30000;
constexpr uint32_t TELEMETRY_INTERVAL_MS = 300000;
constexpr uint32_t ULTRASONIC_TIMEOUT_US = 30000;
constexpr size_t MAX_JPEG_SIZE_BYTES = 1024 * 1024;
constexpr size_t MAX_DECODED_PIXELS = 1024 * 768;
constexpr size_t TENSOR_ARENA_SIZE = 3 * 1024 * 1024;

constexpr int MODEL_INPUT_WIDTH = 160;
constexpr int MODEL_INPUT_HEIGHT = 160;
constexpr int MODEL_INPUT_CHANNELS = 3;
constexpr int MODEL_CLASS_COUNT = 5;
constexpr float MODEL_INPUT_SCALE = 1.0F;
constexpr int MODEL_INPUT_ZERO_POINT = -128;
constexpr float MODEL_OUTPUT_SCALE = 0.00390625F;
constexpr int MODEL_OUTPUT_ZERO_POINT = -128;
constexpr float CONFIDENCE_THRESHOLD = 0.60F;

// WARNING: No authoritative training class-index mapping was present in the
// repositories or embedded TFLite metadata. This order MUST be checked against
// the training/export pipeline. Array position is the model output index.
const char* CLASS_NAMES[5] = {"PET", "HDPE", "LDPE", "PP", "OTHER"};

constexpr char FIRMWARE_VERSION[] = "2.0.0";
constexpr char MODEL_VERSION[] = "plastic-classifier-int8";

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
WebServer httpServer(80);
WebSocketsServer webSocketServer(81);
Preferences preferences;
Servo compartmentServos[4];

String deviceId;
String wifiSsid;
String wifiPassword;
String pairingCode;
String deviceKey;
bool paired = false;
bool tabletConnected = false;
uint8_t tabletClientId = 0;

uint32_t lastStaReconnectAt = 0;
uint32_t lastPairAttemptAt = 0;
uint32_t lastTelemetryAt = 0;
uint32_t lastPersonTriggerAt = 0;
uint32_t servoCloseAt[4] = {0, 0, 0, 0};

const tflite::Model* tfliteModel = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* modelInput = nullptr;
TfLiteTensor* modelOutput = nullptr;
uint8_t* tensorArena = nullptr;
bool modelReady = false;

struct DisposalRequest {
  bool active = false;
  bool awaitingUpload = false;
  bool fallbackPending = false;
  bool servoActuated = false;
  String requestId;
  int retryAttempt = 0;
  uint32_t uploadDeadline = 0;
};

DisposalRequest disposal;

struct UploadState {
  uint8_t* jpeg = nullptr;
  size_t length = 0;
  bool failed = false;
  String error;
};

UploadState uploadState;

struct DecodeState {
  uint8_t* rgb = nullptr;
  int width = 0;
  int height = 0;
};

DecodeState decodeState;

void clearUploadState();

// ---------------------------------------------------------------------------
// Utility helpers
// ---------------------------------------------------------------------------
bool deadlineReached(uint32_t deadline) {
  return static_cast<int32_t>(millis() - deadline) >= 0;
}

String makeBackendUrl(const char* endpoint) {
  return String(API_BASE_URL) + endpoint;
}

String makeDeviceId() {
  const uint64_t chipId = ESP.getEfuseMac();
  char value[32];
  snprintf(value, sizeof(value), "ESP32-%04X%08X",
           static_cast<uint16_t>(chipId >> 32),
           static_cast<uint32_t>(chipId));
  return String(value);
}

String makeRequestId() {
  char value[48];
  snprintf(value, sizeof(value), "%s-%lu", deviceId.c_str(),
           static_cast<unsigned long>(millis()));
  return String(value);
}

void sendTabletJson(const JsonDocument& document) {
  if (!tabletConnected) return;
  String message;
  serializeJson(document, message);
  webSocketServer.sendTXT(tabletClientId, message);
}

void sendCaptureMessage(const char* type, uint32_t delayMs, int retryAttempt) {
  DynamicJsonDocument document(256);
  document["type"] = type;
  document["requestId"] = disposal.requestId;
  document["delayMs"] = delayMs;
  document["retryAttempt"] = retryAttempt;
  sendTabletJson(document);
}

void sendClassificationResult(const char* className, float confidence) {
  DynamicJsonDocument document(256);
  document["type"] = "classification_result";
  document["requestId"] = disposal.requestId;
  document["class"] = className;
  document["confidence"] = confidence;
  sendTabletJson(document);
}

// ---------------------------------------------------------------------------
// Sensors and servos
// ---------------------------------------------------------------------------
float readUltrasonicDistanceCm(uint8_t triggerPin, uint8_t echoPin) {
  digitalWrite(triggerPin, LOW);
  delayMicroseconds(2);
  digitalWrite(triggerPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(triggerPin, LOW);

  const unsigned long duration = pulseIn(echoPin, HIGH, ULTRASONIC_TIMEOUT_US);
  if (duration == 0) return NAN;
  return static_cast<float>(duration) * 0.0343F / 2.0F;
}

float distanceToFillPercent(float distanceCm, float depthCm) {
  if (!isfinite(distanceCm) || depthCm <= 0.0F) return 0.0F;
  const float fill = (depthCm - distanceCm) * 100.0F / depthCm;
  return constrain(fill, 0.0F, 100.0F);
}

int classToCompartment(const char* className) {
  if (strcmp(className, "PET") == 0) return 0;
  if (strcmp(className, "HDPE") == 0) return 1;
  if (strcmp(className, "LDPE") == 0) return 2;
  if (strcmp(className, "PP") == 0) return 3;
  return -1;
}

void openClassifiedCompartment(const char* className) {
  if (disposal.servoActuated) return;
  disposal.servoActuated = true;

  const int compartment = classToCompartment(className);
  if (compartment < 0) {
    Serial.println("[Servo] OTHER selected; no compartment opened");
    return;
  }

  compartmentServos[compartment].write(SERVO_OPEN_ANGLE);
  servoCloseAt[compartment] = millis() + SERVO_OPEN_DURATION_MS;
  Serial.printf("[Servo] Opened compartment %d for %s\n", compartment + 1,
                className);
}

void serviceServos() {
  for (int index = 0; index < 4; ++index) {
    if (servoCloseAt[index] != 0 && deadlineReached(servoCloseAt[index])) {
      compartmentServos[index].write(SERVO_CLOSED_ANGLE);
      servoCloseAt[index] = 0;
      Serial.printf("[Servo] Closed compartment %d\n", index + 1);
    }
  }
}

// ---------------------------------------------------------------------------
// JPEG decoding and TensorFlow Lite Micro inference
// ---------------------------------------------------------------------------
int jpegBlockCallback(JPEGDRAW* draw) {
  if (!decodeState.rgb || !draw || !draw->pPixels) return 0;
  if (draw->iBpp != 16) return 0;
  const uint16_t* pixels = draw->pPixels;

  for (int row = 0; row < draw->iHeight; ++row) {
    const int y = draw->y + row;
    if (y < 0 || y >= decodeState.height) continue;
    for (int column = 0; column < draw->iWidth; ++column) {
      const int x = draw->x + column;
      if (x < 0 || x >= decodeState.width) continue;

      const size_t pixelOffset =
          static_cast<size_t>(row) * draw->iWidth + column;
      const size_t offset = (static_cast<size_t>(y) * decodeState.width + x) * 3;
      const uint16_t pixel = pixels[pixelOffset];
      decodeState.rgb[offset] =
          static_cast<uint8_t>(((pixel >> 11) & 0x1F) * 255 / 31);
      decodeState.rgb[offset + 1] =
          static_cast<uint8_t>(((pixel >> 5) & 0x3F) * 255 / 63);
      decodeState.rgb[offset + 2] =
          static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
    }
  }
  return 1;
}

bool decodeAndResizeIntoModelInput(const uint8_t* jpegBytes, size_t jpegLength) {
  if (!modelReady || !jpegBytes || jpegLength == 0) return false;

  JPEGDEC jpeg;
  if (!jpeg.openRAM(const_cast<uint8_t*>(jpegBytes), jpegLength,
                    jpegBlockCallback)) {
    Serial.printf("[JPEG] Open failed: %d\n", jpeg.getLastError());
    return false;
  }

  const int originalWidth = jpeg.getWidth();
  const int originalHeight = jpeg.getHeight();
  const int shortestSide = min(originalWidth, originalHeight);
  int decodeScale = 1;
  int decodeOption = 0;
  if (shortestSide >= MODEL_INPUT_WIDTH * 8) {
    decodeScale = 8;
    decodeOption = JPEG_SCALE_EIGHTH;
  } else if (shortestSide >= MODEL_INPUT_WIDTH * 4) {
    decodeScale = 4;
    decodeOption = JPEG_SCALE_QUARTER;
  } else if (shortestSide >= MODEL_INPUT_WIDTH * 2) {
    decodeScale = 2;
    decodeOption = JPEG_SCALE_HALF;
  }
  decodeState.width =
      max(1, (originalWidth + decodeScale - 1) / decodeScale);
  decodeState.height =
      max(1, (originalHeight + decodeScale - 1) / decodeScale);
  if (decodeState.width <= 0 || decodeState.height <= 0 ||
      static_cast<size_t>(decodeState.width) >
          MAX_DECODED_PIXELS / static_cast<size_t>(decodeState.height)) {
    Serial.printf("[JPEG] Unsupported dimensions: %dx%d\n", decodeState.width,
                  decodeState.height);
    jpeg.close();
    return false;
  }
  const size_t pixelCount =
      static_cast<size_t>(decodeState.width) * decodeState.height;
  if (pixelCount > MAX_DECODED_PIXELS) {
    Serial.printf("[JPEG] Unsupported dimensions: %dx%d\n", decodeState.width,
                  decodeState.height);
    jpeg.close();
    return false;
  }

  decodeState.rgb = static_cast<uint8_t*>(
      heap_caps_malloc(pixelCount * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!decodeState.rgb) {
    Serial.println("[JPEG] PSRAM allocation failed");
    jpeg.close();
    return false;
  }
  memset(decodeState.rgb, 0, pixelCount * 3);

  // JPEGDEC provides RGB565_LITTLE_ENDIAN as uint16_t callback pixels on the
  // little-endian ESP32-S3. The callback expands each pixel to RGB888.
  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  const bool decoded = jpeg.decode(0, 0, decodeOption) == 1;
  jpeg.close();
  if (!decoded) {
    Serial.printf("[JPEG] Decode failed: %d\n", jpeg.getLastError());
    heap_caps_free(decodeState.rgb);
    decodeState.rgb = nullptr;
    return false;
  }

  int8_t* destination = modelInput->data.int8;
  for (int targetY = 0; targetY < MODEL_INPUT_HEIGHT; ++targetY) {
    const int sourceY = targetY * decodeState.height / MODEL_INPUT_HEIGHT;
    for (int targetX = 0; targetX < MODEL_INPUT_WIDTH; ++targetX) {
      const int sourceX = targetX * decodeState.width / MODEL_INPUT_WIDTH;
      const size_t sourceOffset =
          (static_cast<size_t>(sourceY) * decodeState.width + sourceX) * 3;
      const size_t targetOffset =
          (static_cast<size_t>(targetY) * MODEL_INPUT_WIDTH + targetX) * 3;

      // Exact model preprocessing: raw uint8 RGB -> INT8 by subtracting 128.
      // Do not divide by 255 and do not normalize to -1..1.
      destination[targetOffset] =
          static_cast<int8_t>(static_cast<int>(decodeState.rgb[sourceOffset]) - 128);
      destination[targetOffset + 1] = static_cast<int8_t>(
          static_cast<int>(decodeState.rgb[sourceOffset + 1]) - 128);
      destination[targetOffset + 2] = static_cast<int8_t>(
          static_cast<int>(decodeState.rgb[sourceOffset + 2]) - 128);
    }
  }

  heap_caps_free(decodeState.rgb);
  decodeState.rgb = nullptr;
  return true;
}

bool validateModelTensorMetadata() {
  if (!modelInput || !modelOutput) return false;
  const bool inputShape = modelInput->dims->size == 4 &&
                          modelInput->dims->data[0] == 1 &&
                          modelInput->dims->data[1] == MODEL_INPUT_HEIGHT &&
                          modelInput->dims->data[2] == MODEL_INPUT_WIDTH &&
                          modelInput->dims->data[3] == MODEL_INPUT_CHANNELS;
  const bool outputShape = modelOutput->dims->size == 2 &&
                           modelOutput->dims->data[0] == 1 &&
                           modelOutput->dims->data[1] == MODEL_CLASS_COUNT;
  const bool types = modelInput->type == kTfLiteInt8 &&
                     modelOutput->type == kTfLiteInt8;
  const bool quantization =
      fabsf(modelInput->params.scale - MODEL_INPUT_SCALE) < 0.000001F &&
      modelInput->params.zero_point == MODEL_INPUT_ZERO_POINT &&
      fabsf(modelOutput->params.scale - MODEL_OUTPUT_SCALE) < 0.000001F &&
      modelOutput->params.zero_point == MODEL_OUTPUT_ZERO_POINT;

  if (!(inputShape && outputShape && types && quantization)) {
    Serial.println("[TFLM] Tensor metadata does not match the required model");
    return false;
  }
  return true;
}

bool initializeModel() {
  if (!psramFound()) {
    Serial.println("[TFLM] PSRAM is required but was not detected");
    return false;
  }

  tfliteModel = tflite::GetModel(plastic_classifier_model);
  if (tfliteModel->version() != TFLITE_SCHEMA_VERSION) {
    Serial.printf("[TFLM] Schema mismatch: model=%d runtime=%d\n",
                  tfliteModel->version(), TFLITE_SCHEMA_VERSION);
    return false;
  }

  tensorArena = static_cast<uint8_t*>(
      heap_caps_malloc(TENSOR_ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!tensorArena) {
    Serial.println("[TFLM] Tensor arena allocation failed");
    return false;
  }

  static tflite::AllOpsResolver resolver;
  interpreter = new tflite::MicroInterpreter(
      tfliteModel, resolver, tensorArena, TENSOR_ARENA_SIZE);
  if (!interpreter || interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("[TFLM] AllocateTensors failed");
    delete interpreter;
    interpreter = nullptr;
    heap_caps_free(tensorArena);
    tensorArena = nullptr;
    return false;
  }

  modelInput = interpreter->input(0);
  modelOutput = interpreter->output(0);
  if (!validateModelTensorMetadata()) {
    modelInput = nullptr;
    modelOutput = nullptr;
    delete interpreter;
    interpreter = nullptr;
    heap_caps_free(tensorArena);
    tensorArena = nullptr;
    return false;
  }

  Serial.printf("[TFLM] Model ready; arena used: %u bytes\n",
                static_cast<unsigned>(interpreter->arena_used_bytes()));
  return true;
}

bool runInference(const uint8_t* jpeg, size_t jpegLength, int& classIndex,
                  float& confidence) {
  if (!decodeAndResizeIntoModelInput(jpeg, jpegLength)) return false;
  if (interpreter->Invoke() != kTfLiteOk) {
    Serial.println("[TFLM] Invoke failed");
    return false;
  }

  classIndex = 0;
  confidence = -INFINITY;
  for (int index = 0; index < MODEL_CLASS_COUNT; ++index) {
    const int quantized = static_cast<int>(modelOutput->data.int8[index]);
    // Exact output dequantization: (q - (-128)) * 0.00390625.
    const float value =
        (quantized - MODEL_OUTPUT_ZERO_POINT) * MODEL_OUTPUT_SCALE;
    Serial.printf("[TFLM] %s: %.6f\n", CLASS_NAMES[index], value);
    if (value > confidence) {
      confidence = value;
      classIndex = index;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Disposal request state machine
// ---------------------------------------------------------------------------
void finishDisposal(const char* className, float confidence) {
  if (!disposal.active) return;
  sendClassificationResult(className, confidence);
  openClassifiedCompartment(className);
  Serial.printf("[Cycle] Result %s (%.4f) for %s\n", className, confidence,
                disposal.requestId.c_str());
  disposal.active = false;
  disposal.awaitingUpload = false;
  disposal.fallbackPending = false;
  lastPersonTriggerAt = millis();
  clearUploadState();
}

void requestImageForCurrentAttempt() {
  if (tabletConnected) {
    const bool retry = disposal.retryAttempt == 1;
    sendCaptureMessage(retry ? "retry_request" : "capture_request",
                       retry ? 1500 : 1000, disposal.retryAttempt);
    disposal.awaitingUpload = true;
    disposal.uploadDeadline = millis() + TABLET_UPLOAD_TIMEOUT_MS;
  } else {
    disposal.awaitingUpload = false;
    disposal.fallbackPending = true;
    clearUploadState();
  }
}

void handlePrediction(int classIndex, float confidence) {
  if (!disposal.active) return;
  if (confidence >= CONFIDENCE_THRESHOLD) {
    finishDisposal(CLASS_NAMES[classIndex], confidence);
    return;
  }

  if (disposal.retryAttempt == 0) {
    disposal.retryAttempt = 1;
    Serial.printf("[Cycle] Confidence %.4f below threshold; requesting one retry\n",
                  confidence);
    requestImageForCurrentAttempt();
    return;
  }

  Serial.printf("[Cycle] Second confidence %.4f below threshold; forcing OTHER\n",
                confidence);
  finishDisposal("OTHER", confidence);
}

void classifyJpeg(uint8_t* jpeg, size_t length, const char* source) {
  if (!disposal.active) {
    heap_caps_free(jpeg);
    return;
  }

  disposal.awaitingUpload = false;
  int classIndex = 0;
  float confidence = 0.0F;
  Serial.printf("[Cycle] Classifying %u-byte JPEG from %s\n",
                static_cast<unsigned>(length), source);
  const bool success = runInference(jpeg, length, classIndex, confidence);
  heap_caps_free(jpeg);

  if (!success) {
    Serial.println("[Cycle] Image processing/inference failed; returning OTHER");
    finishDisposal("OTHER", 0.0F);
    return;
  }
  handlePrediction(classIndex, confidence);
}

void startDisposalCycle() {
  if (disposal.active || !modelReady) return;
  disposal = DisposalRequest{};
  disposal.active = true;
  disposal.requestId = makeRequestId();
  lastPersonTriggerAt = millis();
  Serial.printf("[Cycle] Started %s\n", disposal.requestId.c_str());
  requestImageForCurrentAttempt();
}

void processPersonDetection() {
  if (disposal.active || !modelReady) return;
  if (millis() - lastPersonTriggerAt < PERSON_COOLDOWN_MS) return;
  if (digitalRead(PIR_PIN) != HIGH) return;

  const float distance =
      readUltrasonicDistanceCm(PERSON_TRIG_PIN, PERSON_ECHO_PIN);
  if (isfinite(distance) && distance <= PERSON_TRIGGER_DISTANCE_CM) {
    Serial.printf("[Person] PIR active, distance %.1f cm\n", distance);
    startDisposalCycle();
  }
}

bool downloadFallbackJpeg(uint8_t*& jpeg, size_t& length) {
  jpeg = nullptr;
  length = 0;
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(10000);
  if (!http.begin(client, ESP32_CAM_CAPTURE_URL)) return false;

  const int status = http.GET();
  const int contentLength = http.getSize();
  if (status != HTTP_CODE_OK || contentLength <= 0 ||
      static_cast<size_t>(contentLength) > MAX_JPEG_SIZE_BYTES) {
    Serial.printf("[CAM] Capture failed: HTTP %d, length %d\n", status,
                  contentLength);
    http.end();
    return false;
  }

  jpeg = static_cast<uint8_t*>(heap_caps_malloc(
      contentLength, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!jpeg) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  const size_t received = stream->readBytes(jpeg, contentLength);
  http.end();
  if (received != static_cast<size_t>(contentLength)) {
    heap_caps_free(jpeg);
    jpeg = nullptr;
    Serial.printf("[CAM] Incomplete JPEG: %u/%d bytes\n",
                  static_cast<unsigned>(received), contentLength);
    return false;
  }
  length = received;
  return true;
}

void serviceDisposalState() {
  if (!disposal.active) return;
  if (disposal.awaitingUpload && deadlineReached(disposal.uploadDeadline)) {
    Serial.println("[Cycle] Tablet upload timed out; using backup camera");
    disposal.awaitingUpload = false;
    disposal.fallbackPending = true;
    clearUploadState();
  }
  if (!disposal.fallbackPending) return;

  disposal.fallbackPending = false;
  uint8_t* jpeg = nullptr;
  size_t length = 0;
  if (!downloadFallbackJpeg(jpeg, length)) {
    Serial.println("[Cycle] Backup camera unavailable; returning OTHER");
    finishDisposal("OTHER", 0.0F);
    return;
  }
  classifyJpeg(jpeg, length, "ESP32-CAM");
}

// ---------------------------------------------------------------------------
// WebSocket protocol
// ---------------------------------------------------------------------------
void handleTabletMessage(uint8_t clientId, const uint8_t* payload, size_t length) {
  DynamicJsonDocument document(512);
  if (deserializeJson(document, payload, length) != DeserializationError::Ok) {
    Serial.println("[WS] Invalid JSON message");
    return;
  }

  const String type = document["type"] | "";
  const String requestId = document["requestId"] | "";
  if (type == "classification_feedback") {
    const bool correct = document["correct"] | false;
    Serial.printf("[WS] Feedback request=%s class=%s correct=%s\n",
                  requestId.c_str(), document["class"] | "",
                  correct ? "true" : "false");
    return;
  }

  if (!disposal.active || requestId != disposal.requestId) {
    Serial.printf("[WS] Ignoring stale %s for %s\n", type.c_str(),
                  requestId.c_str());
    return;
  }

  if (type == "capture_ack") {
    Serial.printf("[WS] Capture acknowledged, attempt %d\n",
                  document["retryAttempt"] | 0);
  } else if (type == "capture_failed") {
    Serial.printf("[WS] Tablet capture failed: %s\n",
                  document["reason"] | "unspecified");
    disposal.awaitingUpload = false;
    disposal.fallbackPending = true;
    clearUploadState();
  } else if (type == "open_compartment") {
    Serial.println("[WS] Ignoring tablet servo command; S3 owns actuation");
  }
}

void onWebSocketEvent(uint8_t clientId, WStype_t type, uint8_t* payload,
                      size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      tabletConnected = true;
      tabletClientId = clientId;
      Serial.printf("[WS] Tablet connected: client %u\n", clientId);
      break;
    case WStype_DISCONNECTED:
      if (tabletConnected && tabletClientId == clientId) {
        tabletConnected = false;
        Serial.println("[WS] Tablet disconnected");
        if (disposal.active && disposal.awaitingUpload) {
          disposal.awaitingUpload = false;
          disposal.fallbackPending = true;
          clearUploadState();
        }
      }
      break;
    case WStype_TEXT:
      handleTabletMessage(clientId, payload, length);
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Local HTTP server and multipart /infer upload
// ---------------------------------------------------------------------------
void clearUploadState() {
  if (uploadState.jpeg) heap_caps_free(uploadState.jpeg);
  uploadState = UploadState{};
}

void handleInferUpload() {
  HTTPUpload& upload = httpServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    clearUploadState();
    if (upload.name != "image") return;
    uploadState.jpeg = static_cast<uint8_t*>(heap_caps_malloc(
        MAX_JPEG_SIZE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!uploadState.jpeg) {
      uploadState.failed = true;
      uploadState.error = "jpeg_allocation_failed";
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!uploadState.jpeg || uploadState.failed) return;
    if (uploadState.length + upload.currentSize > MAX_JPEG_SIZE_BYTES) {
      uploadState.failed = true;
      uploadState.error = "jpeg_too_large";
      heap_caps_free(uploadState.jpeg);
      uploadState.jpeg = nullptr;
      return;
    }
    memcpy(uploadState.jpeg + uploadState.length, upload.buf,
           upload.currentSize);
    uploadState.length += upload.currentSize;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    clearUploadState();
    uploadState.failed = true;
    uploadState.error = "upload_aborted";
  }
}

void handleInferComplete() {
  if (uploadState.failed || !uploadState.jpeg || uploadState.length == 0) {
    const String error = uploadState.error.length() ? uploadState.error
                                                    : "missing_jpeg";
    clearUploadState();
    httpServer.send(400, "application/json",
                    String("{\"error\":\"") + error + "\"}");
    return;
  }

  const String requestId = httpServer.arg("requestId");
  const int retryAttempt = httpServer.arg("retryAttempt").toInt();
  if (!disposal.active || requestId != disposal.requestId ||
      retryAttempt != disposal.retryAttempt) {
    clearUploadState();
    httpServer.send(409, "application/json", "{\"error\":\"stale_request\"}");
    return;
  }

  Serial.printf("[HTTP] /infer request=%s attempt=%d lat=%s lon=%s\n",
                requestId.c_str(), retryAttempt,
                httpServer.arg("latitude").c_str(),
                httpServer.arg("longitude").c_str());
  uint8_t* jpeg = uploadState.jpeg;
  const size_t length = uploadState.length;
  uploadState.jpeg = nullptr;
  uploadState.length = 0;
  classifyJpeg(jpeg, length, "tablet");
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

String healthJson() {
  DynamicJsonDocument document(512);
  document["ok"] = true;
  document["deviceId"] = deviceId;
  document["apIp"] = WiFi.softAPIP().toString();
  document["staConnected"] = WiFi.status() == WL_CONNECTED;
  document["staIp"] = WiFi.localIP().toString();
  document["tabletConnected"] = tabletConnected;
  document["modelReady"] = modelReady;
  document["paired"] = paired;
  document["requestActive"] = disposal.active;
  String response;
  serializeJson(document, response);
  return response;
}

void saveProvisioning(const String& ssid, const String& password,
                      const String& code) {
  preferences.begin("smartbin", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", password);
  preferences.putString("pairCode", code);
  preferences.remove("devKey");
  preferences.putBool("paired", false);
  preferences.end();
  wifiSsid = ssid;
  wifiPassword = password;
  pairingCode = code;
}

void handleProvisioning() {
  const String ssid = httpServer.arg("ssid");
  const String password = httpServer.arg("password");
  const String code = httpServer.arg("code");
  if (ssid.isEmpty() || code.length() != 6) {
    httpServer.send(400, "application/json",
                    "{\"error\":\"ssid_and_6_digit_code_required\"}");
    return;
  }
  saveProvisioning(ssid, password, code);
  paired = false;
  deviceKey = "";
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

void beginLocalServers() {
  httpServer.on("/", HTTP_GET,
                []() { httpServer.send(200, "application/json", healthJson()); });
  httpServer.on("/save", HTTP_POST, handleProvisioning);
  httpServer.on("/infer", HTTP_POST, handleInferComplete, handleInferUpload);
  httpServer.onNotFound([]() {
    httpServer.send(404, "application/json", "{\"error\":\"not_found\"}");
  });
  httpServer.begin();

  webSocketServer.begin();
  webSocketServer.onEvent(onWebSocketEvent);
  Serial.println("[HTTP] Listening on port 80");
  Serial.println("[WS] Listening on port 81");
}

// ---------------------------------------------------------------------------
// STA Wi-Fi, backend pairing, and real telemetry
// ---------------------------------------------------------------------------
void loadPreferences() {
  preferences.begin("smartbin", true);
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("pass", "");
  pairingCode = preferences.getString("pairCode", "");
  deviceKey = preferences.getString("devKey", "");
  paired = preferences.getBool("paired", false) && !deviceKey.isEmpty();
  preferences.end();
}

void saveDeviceKey(const String& key) {
  preferences.begin("smartbin", false);
  preferences.putString("devKey", key);
  preferences.putBool("paired", true);
  preferences.remove("pairCode");
  preferences.end();
  deviceKey = key;
  pairingCode = "";
  paired = true;
}

void clearDevicePairing() {
  preferences.begin("smartbin", false);
  preferences.remove("devKey");
  preferences.putBool("paired", false);
  preferences.end();
  deviceKey = "";
  paired = false;
}

void serviceStaConnection() {
  if (WiFi.status() == WL_CONNECTED || wifiSsid.isEmpty()) return;
  if (millis() - lastStaReconnectAt < STA_RECONNECT_INTERVAL_MS) return;
  lastStaReconnectAt = millis();
  Serial.printf("[WiFi] Connecting STA to %s\n", wifiSsid.c_str());
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
}

bool pairWithBackend() {
  if (WiFi.status() != WL_CONNECTED || pairingCode.length() != 6) return false;
  WiFiClientSecure client;
  client.setInsecure();  // Replace with the backend CA certificate for production.
  HTTPClient http;
  if (!http.begin(client, makeBackendUrl(PAIR_ENDPOINT))) return false;
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument document(512);
  document["code"] = pairingCode;
  document["esp32ChipId"] = deviceId;
  document["firmwareVersion"] = FIRMWARE_VERSION;
  document["cnnModelVersion"] = MODEL_VERSION;
  String body;
  serializeJson(document, body);

  const int status = http.POST(body);
  const String response = http.getString();
  http.end();
  if (status != HTTP_CODE_OK) {
    Serial.printf("[Backend] Pair failed: HTTP %d\n", status);
    return false;
  }

  DynamicJsonDocument result(512);
  if (deserializeJson(result, response) != DeserializationError::Ok) return false;
  const String key = result["data"]["key"] | "";
  if (key.isEmpty()) return false;
  saveDeviceKey(key);
  Serial.println("[Backend] Pairing successful");
  return true;
}

void servicePairing() {
  if (paired || WiFi.status() != WL_CONNECTED || pairingCode.isEmpty()) return;
  if (millis() - lastPairAttemptAt < PAIR_RETRY_INTERVAL_MS) return;
  lastPairAttemptAt = millis();
  pairWithBackend();
}

String isoTimestamp() {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 50)) return "";
  char value[30];
  strftime(value, sizeof(value), "%Y-%m-%dT%H:%M:%S.000Z", &timeInfo);
  return String(value);
}

bool sendTelemetry() {
  if (!paired || deviceKey.isEmpty() || WiFi.status() != WL_CONNECTED) return false;

  DynamicJsonDocument document(2048);
  const String timestamp = isoTimestamp();
  if (!timestamp.isEmpty()) document["timestamp"] = timestamp;
  JsonArray compartments = document.createNestedArray("compartments");
  for (int index = 0; index < 4; ++index) {
    const float distance =
        readUltrasonicDistanceCm(FILL_TRIG_PINS[index], FILL_ECHO_PINS[index]);
    JsonObject compartment = compartments.createNestedObject();
    compartment["slot"] = index + 1;
    const float fillPercent =
        distanceToFillPercent(distance, COMPARTMENT_DEPTH_CM[index]);
    compartment["fillPercent"] = fillPercent;
    compartment["fillLiters"] =
        COMPARTMENT_CAPACITY_LITERS[index] * fillPercent / 100.0F;
    JsonObject raw = compartment.createNestedObject("raw");
    if (isfinite(distance)) raw["distance"] = distance;
    raw["sensorId"] = String("HC-SR04-") + (index + 1);
  }

  JsonObject connectivity = document.createNestedObject("connectivity");
  connectivity["rssi"] = WiFi.RSSI();
  connectivity["ip"] = WiFi.localIP().toString();
  connectivity["network"] = WiFi.SSID();

  String body;
  serializeJson(document, body);
  WiFiClientSecure client;
  client.setInsecure();  // Replace with the backend CA certificate for production.
  HTTPClient http;
  if (!http.begin(client, makeBackendUrl(TELEMETRY_ENDPOINT))) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-id", deviceId);
  http.addHeader("x-device-key", deviceKey);
  const int status = http.POST(body);
  http.end();
  Serial.printf("[Backend] Telemetry HTTP %d\n", status);
  if (status == HTTP_CODE_UNAUTHORIZED || status == HTTP_CODE_FORBIDDEN) {
    clearDevicePairing();
  }
  return status >= 200 && status < 300;
}

void serviceTelemetry() {
  if (!paired || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastTelemetryAt < TELEMETRY_INTERVAL_MS) return;
  lastTelemetryAt = millis();
  sendTelemetry();
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void configureHardware() {
  pinMode(PIR_PIN, INPUT);
  pinMode(PERSON_TRIG_PIN, OUTPUT);
  pinMode(PERSON_ECHO_PIN, INPUT);
  for (int index = 0; index < 4; ++index) {
    pinMode(FILL_TRIG_PINS[index], OUTPUT);
    pinMode(FILL_ECHO_PINS[index], INPUT);
    compartmentServos[index].setPeriodHertz(50);
    compartmentServos[index].attach(SERVO_PINS[index], 500, 2500);
    compartmentServos[index].write(SERVO_CLOSED_ANGLE);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32-S3 Smart Bin ===");
  deviceId = makeDeviceId();
  loadPreferences();
  configureHardware();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    Serial.println("[WiFi] Failed to start local AP");
  } else {
    Serial.printf("[WiFi] AP %s at %s\n", AP_SSID,
                  WiFi.softAPIP().toString().c_str());
  }
  if (!wifiSsid.isEmpty()) WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  beginLocalServers();
  modelReady = initializeModel();
  lastPersonTriggerAt = millis() - PERSON_COOLDOWN_MS;
  lastStaReconnectAt = millis() - STA_RECONNECT_INTERVAL_MS;
  lastPairAttemptAt = millis() - PAIR_RETRY_INTERVAL_MS;
  lastTelemetryAt = millis() - TELEMETRY_INTERVAL_MS;
}

void loop() {
  httpServer.handleClient();
  webSocketServer.loop();
  serviceServos();
  serviceStaConnection();
  servicePairing();
  serviceTelemetry();
  serviceDisposalState();
  processPersonDetection();
  delay(2);
}
