# Daisy APRS (Heltec V4 Expansion)

Lean PlatformIO APRS tracker app for Heltec WiFi LoRa 32 V4 Expansion Kit (ESP32-S3 + SX1262), built for fast boot and responsive runtime.

## What It Does

- Receives LoRa APRS packets and prints decoded payloads to serial monitor.
- Sends periodic APRS GPS beacons when a valid GPS fix exists.
- Supports manual APRS packet TX from serial monitor.
- Hosts a web config page for APRS + Wi-Fi settings.
- Tries saved home Wi-Fi first, then falls back to device AP mode if unreachable.
- Uses LoRa APRS wire compatibility prefix: `0x3C 0xFF 0x01` + APRS text payload.
- Uses the expansion touchscreen in touch-only mode (no keyboard required).

## Display + Touch

- Expansion kit touchscreen resolution is `320x240` (landscape UI on a `240x320` ST7789 panel).
- Touch controller is CHSC6x over I2C.

## Configure

- Runtime config is stored on-device (NVS) and edited via web UI.
- `include/app_config.h` remains the default fallback values used before first save.

## Web Config

- On boot, the device tries to join saved home Wi-Fi credentials.
- If home Wi-Fi is not available, it starts its own AP automatically.
- AP fallback credentials:
	- SSID: `DAISY-APRS-XXXXXX` (printed in serial log)
	- Password: `daisyaprs`
- Open the printed URL in a browser and save APRS + Wi-Fi settings.
- Wi-Fi credential changes are saved immediately and applied on reboot.

## Build and Flash

Build:

- `pio run -e heltec-v4-tft`

Upload:

- `pio run -e heltec-v4-tft -t upload`

Monitor:

- `pio device monitor -b 115200`

Convenience script:

- `./build-upload-monitor.sh build`
- `./build-upload-monitor.sh upload`
- `./build-upload-monitor.sh monitor`
- `./build-upload-monitor.sh upload-monitor`
- `./build-upload-monitor.sh clean-upload-monitor`

## Serial Commands

- `help`
- `status`
- `web` (prints web config mode and URL)
- `beacon`
- `c` (toggles Main and LOG screens)
- `screen next` / `screen prev`
- `screen main` / `screen log`
- `enter` (sends beacon when Main screen is active)
- `tx <TNC2_PACKET>`
- `scroll newer` / `scroll older`
- `scroll pageup` / `scroll pagedown`
- `scroll top`

## Hardware Navigation

- Main screen touch zones:
	- Top-right: open LOG screen.
	- Bottom beacon button: send beacon now.
- LOG screen touch zones:
	- Top-left: return to Main screen.
	- Lower-left / lower-right: page newer / older log entries.
	- Middle-left / middle-right: scroll one entry newer / older.
- Serial fallback remains: Enter on an empty serial command line while on Main sends a beacon.

Example manual TX:

- `tx N0CALL-7>APLRT1,WIDE1-1:>test from heltec-v4`
