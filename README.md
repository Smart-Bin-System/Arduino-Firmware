# Smart Waste Segregation Firmware

Firmware for the integrated smart plastic-sorting prototype. The ESP32-S3-N16R8 is the primary physical controller, the Android tablet is the primary image source and user interface, and the AI Thinker ESP32-CAM is a fallback image source only.

## Final Architecture

```text
Android tablet -- JPEG + GPS over HTTP --> ESP32-S3-N16R8
Android tablet <---- commands/results over WebSocket ---- ESP32-S3-N16R8
ESP32-CAM -------- GET /capture fallback --------> ESP32-S3-N16R8
ESP32-S3-N16R8 -- pairing + telemetry over HTTPS --> Express Backend + MongoDB
React Frontend -------- JWT/Axios --------> Express Backend + MongoDB
```

Image inference and physical sorting remain local to the ESP32-S3. The React frontend communicates only with the Express backend; it does not connect directly to the ESP32-S3. Local classification continues to operate when internet/backend access is unavailable.

### ESP32-S3-N16R8

The S3 is the primary controller and is responsible for:

- Running the INT8 TensorFlow Lite Micro plastic classifier.
- Reading the PIR/person-detection ultrasonic sensor and four compartment ultrasonic sensors.
- Controlling all four sorting servos.
- Hosting the local HTTP server, including `POST /infer` on port 80.
- Hosting the local WebSocket server on port 81 for tablet commands and results.
- Operating its local access point while maintaining an upstream station connection.
- Pairing with the backend and storing its device key.
- Sending real ultrasonic telemetry to the backend every five minutes.
- Requesting `GET /capture` from the ESP32-CAM if the tablet image upload fails.

### Android tablet

The Android tablet provides:

- The primary camera.
- Hidden CameraX photograph capture without exposing a preview.
- GPS coordinates sent with each JPEG.
- The user interface, classification result screens, and Correct/Wrong feedback.

The tablet does not run the model or decide which servo opens. It sends JPEG and GPS data to the S3 and displays the S3 classification result.

### ESP32-CAM backup

The AI Thinker ESP32-CAM is a fallback camera only. It does not contain TensorFlow Lite, sensors, servo control, or backend communication.

- Static IP: `192.168.4.2`
- Gateway: `192.168.4.1`
- HTTP port: `80`
- Readiness endpoint: `GET /`
- Fresh JPEG endpoint: `GET /capture`
- Capture format: 320x240 QVGA JPEG, quality 12

It connects as a station to the S3 access point and automatically reconnects after a connection loss.

## Local Network

The ESP32-S3 uses `WIFI_AP_STA` so the local device network and upstream internet connection can operate together.

| Service | Address |
| --- | --- |
| S3 access point | `192.168.4.1/24` |
| S3 HTTP server | `http://192.168.4.1:80` |
| S3 WebSocket server | `ws://192.168.4.1:81/` |
| ESP32-CAM | `192.168.4.2/24` |
| ESP32-CAM capture | `http://192.168.4.2/capture` |

The AP SSID is `ESP32-S3-N16R8`. `AP_PASSWORD` in the S3 sketch and `S3_AP_PASSWORD` in the ESP32-CAM sketch must always have the same value. Replace the development password before deployment.

## Classifier

The bundled model has this tensor contract:

| Tensor | Shape | Type | Scale | Zero point |
| --- | --- | --- | --- | --- |
| Input | `[1, 160, 160, 3]` RGB | INT8 | `1.0` | `-128` |
| Output | `[1, 5]` | INT8 | `0.00390625` | `-128` |

Input preprocessing uses raw 8-bit RGB values directly:

```text
int8_value = raw_pixel - 128
```

For example, RGB values 0, 128, and 255 become -128, 0, and 127. Do not divide pixels by 255 or normalize them to `-1..1`.

The five application-facing classes are:

1. `PET`
2. `HDPE`
3. `LDPE`
4. `PP`
5. `OTHER`

`CLASS_NAMES` in the S3 sketch maps model output indices 0 through 4. That order must match the model's training/export class order; no authoritative training mapping was available in the repositories when the firmware was integrated.

The acceptance threshold is `0.60`. A result below the threshold triggers exactly one silent retry. If the retry also remains below threshold, the result is `OTHER`.

### Physical sorting map

| Result | Action |
| --- | --- |
| PET | Open servo 1 |
| HDPE | Open servo 2 |
| LDPE | Open servo 3 |
| PP | Open servo 4 |
| OTHER | Display only; open no servo |

## ESP32-S3 GPIO Map

All prototype GPIO assignments are centralized near the top of `ESP32_S3_Smart_Bin.ino`.

| Function | GPIO |
| --- | ---: |
| PIR input | 4 |
| Person ultrasonic TRIG | 5 |
| Person ultrasonic ECHO | 6 |
| PET fill ultrasonic TRIG / ECHO | 7 / 8 |
| HDPE fill ultrasonic TRIG / ECHO | 9 / 10 |
| LDPE fill ultrasonic TRIG / ECHO | 11 / 12 |
| PP fill ultrasonic TRIG / ECHO | 13 / 14 |
| PET servo 1 | 15 |
| HDPE servo 2 | 16 |
| LDPE servo 3 | 17 |
| PP servo 4 | 18 |

### Electrical requirements

HC-SR04 ECHO is a 5 V signal. Every ECHO connection must pass through a suitable voltage divider or level shifter before reaching an ESP32-S3 GPIO. Direct 5 V input can damage the ESP32-S3.

Do not power four servos from the ESP32-S3 board. Use a suitably rated external regulated 5 V supply for the servos and connect the servo-supply ground to the ESP32-S3 ground. Size the supply and wiring for servo stall current.

## Backend Integration

The S3 independently connects to the Express backend through its station interface.

- Pairing: `POST /api/bins/pair`
- Telemetry: `POST /api/telemetry`
- Telemetry authentication header: `x-device-id`
- Telemetry authentication header: `x-device-key`

Pairing sends the six-digit code, ESP32 chip ID, firmware version, and CNN model version. The returned `data.key` is stored in Preferences and used for later telemetry requests. Telemetry includes the four compartment readings and connectivity information. Backend availability is not required for local tablet capture, inference, or servo control.

## Repository Layout

```text
.
|-- plastic_classifier_int8.tflite
|-- README.md
|-- tools/
|   `-- tflite_to_header.py
`-- ESP32 - Codes/
    |-- ESP32 Syncing.ino
    |-- ESP32-S3 Smart Bin/
    |   |-- ESP32_S3_Smart_Bin.ino
    |   `-- plastic_classifier_model.h
    `-- ESP32-CAM Backup/
        `-- ESP32_CAM_Backup.ino
```

`ESP32 - Codes/ESP32 Syncing.ino` is the older legacy backend-sync prototype. It is preserved for reference and is not the final integrated ESP32-S3 firmware.

## Converting the Model

The conversion tool reads the `.tflite` file as binary and writes the bytes unchanged into a 16-byte-aligned Arduino header:

```powershell
python tools/tflite_to_header.py plastic_classifier_int8.tflite "ESP32 - Codes/ESP32-S3 Smart Bin/plastic_classifier_model.h"
```

Regenerate the header whenever the final model changes. Keep `CLASS_NAMES`, tensor metadata constants, and `MODEL_VERSION` synchronized with the exported model.

## Arduino Requirements

Install the Espressif ESP32 Arduino board package and these libraries for the S3 sketch:

- ArduinoJson
- ESP32Servo
- JPEGDEC
- WebSockets (the Links2004 `WebSocketsServer` library)
- A TensorFlow Lite Micro library compatible with the installed ESP32-S3 Arduino core

The ESP32 core provides `WiFi`, `WebServer`, `HTTPClient`, `WiFiClientSecure`, `Preferences`, and ESP32 camera support. The ESP32-CAM sketch uses the core-provided `esp_camera` driver and does not require TensorFlow Lite libraries.

Recommended ESP32-S3-N16R8 settings:

- Board: `ESP32S3 Dev Module` or the exact matching vendor board definition
- Flash size: `16MB`
- PSRAM: `OPI PSRAM` / enabled (`8MB`)
- CPU frequency: `240MHz`
- Partition scheme: choose one with enough application space for the firmware and embedded model
- Upload speed: use a stable speed supported by the board and USB connection
- Serial Monitor: `115200` baud

PSRAM is required by the integrated S3 firmware for the tensor arena and JPEG decode buffer. For the backup camera, select `AI Thinker ESP32-CAM`; enable PSRAM when the board menu exposes that option. The camera firmware prefers PSRAM but can fall back to one DRAM frame buffer.

Before uploading, review the editable network credentials, backend URL, measured compartment depths/capacities, GPIO assignments, servo angles, model version, and class order.
