#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <TinyGPS++.h>

#include "app_config.h"
#include "aprs_codec.h"
#include "board_pins.h"
#include "ui.h"

namespace {
SX1262 radio = new Module(BoardPins::RADIO_CS, BoardPins::RADIO_DIO1, BoardPins::RADIO_RST,
                          BoardPins::RADIO_BUSY);
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);

volatile bool radioIrqFired = false;
uint32_t lastBeaconMs = 0;
uint32_t lastNoFixLogMs = 0;
uint32_t lastUiGpsUpdateMs = 0;

String serialLine;

void onRadioIrq() { radioIrqFired = true; }

void prepareBoardPower() {
  pinMode(BoardPins::BOARD_POWERON, OUTPUT);
  digitalWrite(BoardPins::BOARD_POWERON, HIGH);

  // Deselect all devices on the shared SPI bus before display init.
  pinMode(BoardPins::BOARD_SDCARD_CS, OUTPUT);
  pinMode(BoardPins::RADIO_CS, OUTPUT);
  pinMode(BoardPins::TFT_CS_PIN, OUTPUT);
  digitalWrite(BoardPins::BOARD_SDCARD_CS, HIGH);
  digitalWrite(BoardPins::RADIO_CS, HIGH);
  digitalWrite(BoardPins::TFT_CS_PIN, HIGH);

  delay(500);

  pinMode(BoardPins::TFT_BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BoardPins::TFT_BACKLIGHT_PIN, HIGH);
}

bool initRadio() {
  SPI.begin(BoardPins::RADIO_SCLK, BoardPins::RADIO_MISO, BoardPins::RADIO_MOSI,
            BoardPins::RADIO_CS);

  int state = radio.begin(AppConfig::kFrequencyMhz);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] init failed, code %d\n", state);
    UI::noteStatus("radio init failed");
    return false;
  }

  state = radio.setSpreadingFactor(AppConfig::kSpreadingFactor);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] setSpreadingFactor failed, code %d\n", state);
    UI::noteStatus("radio sf error");
    return false;
  }

  state = radio.setBandwidth(AppConfig::kBandwidthKhz);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] setBandwidth failed, code %d\n", state);
    UI::noteStatus("radio bw error");
    return false;
  }

  state = radio.setCodingRate(AppConfig::kCodingRate);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] setCodingRate failed, code %d\n", state);
    UI::noteStatus("radio cr error");
    return false;
  }

  state = radio.setOutputPower(AppConfig::kTxPowerDbm);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] setOutputPower failed, code %d\n", state);
    UI::noteStatus("radio power error");
    return false;
  }

  radio.setCRC(true);
  radio.setCurrentLimit(140.0f);
  radio.setRxBoostedGainMode(true);

  radio.setDio1Action(onRadioIrq);
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] startReceive failed, code %d\n", state);
    UI::noteStatus("radio rx start error");
    return false;
  }

  Serial.println("[LORA] ready");
  UI::setRadioState(true);
  UI::noteStatus("radio ready");
  return true;
}

void feedGpsParser() {
  while (gpsSerial.available() > 0) {
    gps.encode(static_cast<char>(gpsSerial.read()));
  }
}

String addWirePrefix(const String& aprsText) {
  String wire;
  wire.reserve(aprsText.length() + 3);
  wire += static_cast<char>(AppConfig::kLoRaAprsPrefix0);
  wire += static_cast<char>(AppConfig::kLoRaAprsPrefix1);
  wire += static_cast<char>(AppConfig::kLoRaAprsPrefix2);
  wire += aprsText;
  return wire;
}

bool hasWirePrefix(const String& payload) {
  if (payload.length() < 3) {
    return false;
  }

  return static_cast<uint8_t>(payload[0]) == AppConfig::kLoRaAprsPrefix0 &&
         static_cast<uint8_t>(payload[1]) == AppConfig::kLoRaAprsPrefix1 &&
         static_cast<uint8_t>(payload[2]) == AppConfig::kLoRaAprsPrefix2;
}

void transmitAprs(const String& aprsText) {
  String wire = addWirePrefix(aprsText);

  Serial.printf("[TX] %s\n", aprsText.c_str());
  UI::noteTxPacket(aprsText);
  int state = radio.transmit(wire);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[TX] failed, code %d\n", state);
    UI::noteStatus("tx failed");
  }

  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[RX] restart failed, code %d\n", state);
    UI::noteStatus("rx restart failed");
  }
}

void handleRadioRx() {
  if (!radioIrqFired) {
    return;
  }

  radioIrqFired = false;

  String payload;
  int state = radio.readData(payload);
  if (state != RADIOLIB_ERR_NONE) {
    if (state != RADIOLIB_ERR_RX_TIMEOUT) {
      Serial.printf("[RX] read failed, code %d\n", state);
    }
    radio.startReceive();
    return;
  }

  const float rssi = radio.getRSSI();
  const float snr = radio.getSNR();

  if (hasWirePrefix(payload)) {
    const String aprs = payload.substring(3);
    Serial.printf("[RX] RSSI %.1f SNR %.1f | %s\n", rssi, snr, aprs.c_str());
    UI::noteRxPacket(aprs, rssi, snr);
  } else {
    Serial.printf("[RX] RSSI %.1f SNR %.1f | raw(%d bytes)\n", rssi, snr, payload.length());
    UI::noteRxPacket(String("raw(") + payload.length() + " bytes)", rssi, snr);
  }

  radio.startReceive();
}

String buildCurrentBeacon() {
  return AprsCodec::buildPositionPacket(AppConfig::kCallsign, AppConfig::kDestination,
                                        AppConfig::kPath, gps.location.lat(),
                                        gps.location.lng(), gps.course.deg(),
                                        gps.speed.knots(), static_cast<long>(gps.altitude.feet()),
                                        AppConfig::kSymbolTable, AppConfig::kSymbol,
                                        AppConfig::kComment);
}

void maybeSendBeacon() {
  if (!gps.location.isValid()) {
    const uint32_t now = millis();
    if (now - lastNoFixLogMs >= AppConfig::kNoFixLogIntervalMs) {
      Serial.println("[GPS] waiting for valid fix");
      UI::noteStatus("waiting for gps fix");
      lastNoFixLogMs = now;
    }
    return;
  }

  const uint32_t now = millis();
  if (now - lastBeaconMs < AppConfig::kBeaconIntervalMs) {
    return;
  }

  lastBeaconMs = now;
  transmitAprs(buildCurrentBeacon());
}

void printStatus() {
  Serial.printf("[STATUS] gps.valid=%d sats=%d lat=%.6f lon=%.6f speed=%.1fkmh\n",
                gps.location.isValid() ? 1 : 0, gps.satellites.value(), gps.location.lat(),
                gps.location.lng(), gps.speed.kmph());
}

void handleSerialLine(const String& line) {
  if (line == "help") {
    Serial.println("Commands:");
    Serial.println("  help                 Show this help");
    Serial.println("  status               Show GPS and runtime status");
    Serial.println("  beacon               Send APRS beacon now (if GPS fix is valid)");
    Serial.println("  tx <TNC2_PACKET>     Send a raw APRS packet");
    return;
  }

  if (line == "status") {
    printStatus();
    return;
  }

  if (line == "beacon") {
    if (!gps.location.isValid()) {
      Serial.println("[GPS] no fix, beacon not sent");
      return;
    }
    transmitAprs(buildCurrentBeacon());
    return;
  }

  if (line.startsWith("tx ")) {
    const String packet = line.substring(3);
    if (packet.length() == 0) {
      Serial.println("[TX] empty packet ignored");
      return;
    }
    transmitAprs(packet);
    return;
  }

  if (line.length() > 0) {
    Serial.println("[CMD] unknown command, use 'help'");
  }
}

void pollSerialCommands() {
  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') {
      continue;
    }

    if (ch == '\n') {
      handleSerialLine(serialLine);
      serialLine = "";
      continue;
    }

    if (serialLine.length() < 240) {
      serialLine += ch;
    }
  }
}
}  // namespace

void setup() {
  Serial.begin(AppConfig::kSerialBaud);
  delay(300);

  prepareBoardPower();
  UI::begin();
  UI::showSplash("DAISY APRS", "Starting services");

  gpsSerial.begin(AppConfig::kGpsBaud, SERIAL_8N1, BoardPins::GPS_TX, BoardPins::GPS_RX);
  UI::setGpsState(false, 0, 0.0f);

  Serial.println();
  Serial.println("T-Deck APRS tracker (lean build)");
  Serial.println("Type 'help' in serial monitor for commands.");

  if (!initRadio()) {
    Serial.println("[FATAL] radio init failed");
    UI::noteStatus("fatal radio error");
    UI::render(true);
    while (true) {
      delay(1000);
    }
  }

  UI::noteStatus("listening");
  UI::render(true);
}

void loop() {
  feedGpsParser();
  handleRadioRx();
  maybeSendBeacon();
  pollSerialCommands();

  const uint32_t now = millis();
  if (now - lastUiGpsUpdateMs >= 1000) {
    const bool gpsValid = gps.location.isValid();
    const int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
    const float speed = gps.speed.isValid() ? gps.speed.kmph() : 0.0f;
    UI::setGpsState(gpsValid, sats, speed);
    lastUiGpsUpdateMs = now;
  }

  UI::render();
}
