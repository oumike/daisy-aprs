# Daisy APRS (LilyGO T-Deck)

Lean PlatformIO APRS tracker app for LilyGO T-Deck (ESP32-S3 + SX1262), built for fast boot and responsive runtime.

## What It Does

- Receives LoRa APRS packets and prints decoded payloads to serial monitor.
- Sends periodic APRS GPS beacons when a valid GPS fix exists.
- Supports manual APRS packet TX from serial monitor.
- Uses LoRa APRS wire compatibility prefix: `0x3C 0xFF 0x01` + APRS text payload.

## Configure

Edit constants in `include/app_config.h`:

- Callsign and APRS routing
- Frequency and LoRa modem settings
- Beacon interval
- GPS baud rate

## Build and Flash

Build:

- `pio run -e tdeck`

Upload:

- `pio run -e tdeck -t upload`

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
- `beacon`
- `tx <TNC2_PACKET>`

Example manual TX:

- `tx N0CALL-7>APLRT1,WIDE1-1:>test from tdeck`
