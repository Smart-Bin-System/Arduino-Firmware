# Smart Bin ESP32 Firmware

Firmware for an ESP32-based smart bin device that supports Wi-Fi onboarding, cloud pairing, and scheduled telemetry upload.

## Overview

This project runs on ESP32 and provides:

- Device onboarding through an access point (AP) setup portal
- Secure pairing with a backend service using a 6-digit code
- Persistent storage of Wi-Fi and pairing data in ESP32 Preferences
- Periodic telemetry transmission every 5 minutes
- Automatic fallback to setup mode when configuration or pairing fails

The current implementation includes generated/mock sensor values for compartment fill and environmental readings, making it suitable for integration testing before connecting real sensors.

## Repository Structure

```text
.
|-- README.md
`-- ESP32 - Codes/
		`-- ESP32 Syncing.ino
```

## Firmware Capabilities

### 1) AP Setup Mode

When no Wi-Fi credentials are stored, or connection/pairing fails, the device starts AP mode and hosts a setup page.

- AP SSID: `Bin-Setup`
- AP Password: `12345678`
- Setup page path: `/`
- Save endpoint: `/save`
- Reset endpoint: `/reset`

The setup page collects:

- Wi-Fi SSID
- Wi-Fi password
- 6-digit pairing code

### 2) Cloud Pairing

After credentials are saved, the device attempts to connect and pair with:

- Base URL: `https://api.mihashi.me`
- Pair endpoint: `/api/bins/pair`

On successful pairing, the returned device key is stored and used for authenticated telemetry requests.

### 3) Telemetry Upload

Once paired, the firmware sends telemetry every 300000 ms (5 minutes) to:

- Endpoint: `/api/telemetry`
- Headers:
	- `x-device-id`: ESP32 chip-based ID
	- `x-device-key`: key obtained during pairing

Payload includes:

- ISO8601 UTC timestamp
- Compartment data (4 slots): fill percentage, liters, raw distance and sensor ID
- Power data: battery percent and charging status
- Connectivity data: RSSI, IP, network name
- Environment data: temperature and humidity

## Runtime Flow

1. Boot and load persisted preferences.
2. If Wi-Fi is available, connect and sync time (NTP).
3. If not paired, verify pairing code with server.
4. If any step fails, start AP setup mode.
5. If connected and paired, send telemetry every 5 minutes.

## Tech Stack and Libraries

The sketch uses:

- `WiFi.h`
- `WebServer.h`
- `HTTPClient.h`
- `WiFiClientSecure.h`
- `Preferences.h`
- `ArduinoJson.h`
- `time.h`

## Prerequisites

- ESP32 development board
- Arduino IDE 2.x (or PlatformIO)
- ESP32 board package installed in Arduino IDE
- ArduinoJson library installed
- USB cable and serial access (115200 baud)

## Build and Upload (Arduino IDE)

1. Open `ESP32 - Codes/ESP32 Syncing.ino`.
2. Select your ESP32 board and correct COM port.
3. Install required libraries if prompted (especially ArduinoJson).
4. Compile and upload.
5. Open Serial Monitor at `115200` baud.

## First-Time Setup

1. Power on the ESP32.
2. Connect a phone/laptop to Wi-Fi network `Bin-Setup`.
3. Open the AP portal (typically `192.168.4.1`).
4. Enter:
	 - Local Wi-Fi SSID/password
	 - Valid 6-digit pairing code
5. Submit the form and wait for pairing confirmation in Serial Monitor logs.

## Configuration Constants

Key constants currently defined in firmware:

- `AP_SSID`, `AP_PASSWORD`
- `API_BASE_URL`
- `PAIR_ENDPOINT`
- `TELEMETRY_ENDPOINT`
- `TELEMETRY_INTERVAL_MS`
- `NTP_SERVER_1`, `NTP_SERVER_2`

## Security Notes

- The firmware currently uses `secureClient.setInsecure()` for TLS without certificate validation.
- For production, replace this with proper server certificate or CA pinning.
- AP setup credentials are static by default and should be customized for deployment.

## Troubleshooting

- Device stays in AP mode:
	- Verify Wi-Fi credentials are correct.
	- Confirm signal strength at installation site.
- Pairing fails:
	- Check backend availability and pairing code validity.
	- Inspect serial logs for HTTP status and JSON parse errors.
- No telemetry received:
	- Confirm pairing is complete (`paired = true` in preferences).
	- Validate backend accepts provided headers and payload shape.

## Versioning

Firmware metadata currently reports:

- Firmware version: `1.0.0`
- CNN model version: `cnn-v1`

Update these values in the sketch when releasing new firmware builds.
