# Daisy APRS (Heltec V4 Expansion)

A small APRS app for the expansion kit that will allow passive monitoring and some basic functionality: daily APRSPH check-in, #APRSThursday check-in and weather query.

## Web Config

- On boot, the device tries to join saved home Wi-Fi credentials.
- If home Wi-Fi is not available, it starts its own AP automatically.
- AP fallback credentials:
	- SSID: `DAISY-APRS-AP`
	- Password: `daisyaprs`
- If in AP mode, open a browser to http://192.168.4.1 otherwise use the IP address of your address.

## UI 

### Main Screen

- Action buttons (bottom row):
	- `BEACON`: send position beacon now.
	- `TEST`: send APRSPH test message.
	- `APRSPH`: send your APRSPH check-in message.
	- `WX`: request weather from WXBOT using GPS/manual position.
- Status badges (bottom bar):
	- `GPS`: `NO FIX` or satellite count.
	- `Wi-Fi`: `OFF`, `AP`, or `WIFI`.
	- Battery: percentage with color indication.

### LOG Screen

- Shows recent RX/TX entries in reverse chronological order.
- Allows tapping on a message to see full details

## Build and Flash

Build:

- `pio run -e heltec-v4-tft`

Upload:

- `pio run -e heltec-v4-tft -t upload`

Monitor:

- `pio device monitor`

Convenience script:

- `./build-upload-monitor.sh`
- `./build-upload-monitor.sh --erase`

## Use of AI

Hello!  I've been a developer professionally since about 2001 working on a large list of technologies.  I've created this project in my spare time so I could contribute to one of my favorite hobbies, radio and try out coding with an AI partner (Claude).  Lots of this code has been touched by AI but as I go through the process I'm reviewing the code.  AI is tool, and like any other tool can be used well or used poorly.

This project is a bit more than a proof of concept but not something that has any commercial value.  I'm doing this for fun and to learn.  Feel free to contribute, use or ignore.

## License

GNU General Public License v3.0 (GPLv3)