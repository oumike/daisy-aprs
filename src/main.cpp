#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <TinyGPS++.h>
#include <WiFi.h>
#include <Wire.h>

#include "app_config.h"
#include "aprs_codec.h"
#include "board_pins.h"
#include "runtime_config.h"
#include "ui.h"
#include "web_config.h"

namespace {
SX1262 radio = new Module(BoardPins::RADIO_CS, BoardPins::RADIO_DIO1, BoardPins::RADIO_RST,
                          BoardPins::RADIO_BUSY);
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);

volatile bool radioIrqFired = false;
uint32_t lastBeaconMs = 0;
uint32_t lastNoFixLogMs = 0;
uint32_t lastUiGpsUpdateMs = 0;
uint32_t lastGpsNoDataLogMs = 0;
uint32_t lastBatterySampleMs = 0;

constexpr uint8_t kTouchI2cAddress = 0x2E;
constexpr uint32_t kTouchPollIntervalMs = 25;
constexpr uint32_t kTouchDebounceMs = 180;
constexpr uint16_t kTouchReadBytes = 16;
constexpr int16_t kUiScreenWidth = 320;
constexpr int16_t kUiScreenHeight = 240;
constexpr uint32_t kBatterySampleIntervalMs = 10000;

TwoWire touchWire(1);
uint32_t lastTouchPollMs = 0;
uint32_t lastTouchActionMs = 0;
bool touchOnline = false;
bool touchWasDown = false;
bool radioReady = false;

RuntimeConfig gRuntimeCfg;

String serialLine;

bool sendBeaconNow();
bool sendBeaconWithPosition(double lat, double lon, double courseDeg, double speedKnots,
                            long altitudeFeet);
void openConversationsFromMain();
void openMainFromConversations();
void transmitAprs(const String& aprsText);
void applyRuntimeRadioConfig();
void onWebConfigSaved();
int readBatteryPercent();

void restoreDisplaySpiAfterRadioFailure() {
#if defined(TFT_SCLK) && defined(TFT_MISO) && defined(TFT_MOSI) && defined(TFT_CS)
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  Serial.printf("[TFT] Restored SPI pins after radio init failure (SCK=%d MISO=%d MOSI=%d CS=%d)\n",
                TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
#else
  Serial.println("[TFT] TFT pin macros not available; skipped SPI restore after radio failure");
#endif
}

const char* wifiModeName(wifi_mode_t mode) {
  switch (mode) {
    case WIFI_MODE_NULL:
      return "NULL";
    case WIFI_MODE_STA:
      return "STA";
    case WIFI_MODE_AP:
      return "AP";
    case WIFI_MODE_APSTA:
      return "APSTA";
    default:
      return "UNKNOWN";
  }
}

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "NO_SSID";
    case WL_SCAN_COMPLETED:
      return "SCAN_DONE";
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_CONNECT_FAILED:
      return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    default:
      return "UNKNOWN";
  }
}

bool ipEquals(const IPAddress& a, const IPAddress& b) {
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

bool ipIsZero(const IPAddress& ip) {
  return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
}

bool ipIsApDefault(const IPAddress& ip) {
  return ip[0] == 192 && ip[1] == 168 && ip[2] == 4 && ip[3] == 1;
}

void logWifiDecision(const char* reason,
                     wifi_mode_t mode,
                     wl_status_t status,
                     const IPAddress& staIp,
                     const IPAddress& apIp,
                     bool webRunning,
                     bool webApMode,
                     bool uiConnected,
                     bool uiApMode,
                     bool force = false) {
  static bool initialized = false;
  static wifi_mode_t lastMode = WIFI_MODE_NULL;
  static wl_status_t lastStatus = WL_IDLE_STATUS;
  static IPAddress lastStaIp(0, 0, 0, 0);
  static IPAddress lastApIp(0, 0, 0, 0);
  static bool lastWebRunning = false;
  static bool lastWebApMode = false;
  static bool lastUiConnected = false;
  static bool lastUiApMode = false;
  static uint32_t lastLogMs = 0;

  const uint32_t now = millis();
  const bool changed = !initialized || lastMode != mode || lastStatus != status ||
                       !ipEquals(lastStaIp, staIp) || !ipEquals(lastApIp, apIp) ||
                       lastWebRunning != webRunning || lastWebApMode != webApMode ||
                       lastUiConnected != uiConnected || lastUiApMode != uiApMode;
  const bool heartbeat = initialized && (now - lastLogMs >= 10000);

  if (!force && !changed && !heartbeat) {
    return;
  }

  Serial.printf(
      "[WIFI-DBG] reason=%s mode=%s(%d) status=%s(%d) staIp=%u.%u.%u.%u apIp=%u.%u.%u.%u "
      "webRunning=%d webAp=%d uiConnected=%d uiAp=%d\n",
      reason, wifiModeName(mode), static_cast<int>(mode), wifiStatusName(status),
      static_cast<int>(status), staIp[0], staIp[1], staIp[2], staIp[3], apIp[0], apIp[1], apIp[2],
      apIp[3], webRunning ? 1 : 0, webApMode ? 1 : 0, uiConnected ? 1 : 0,
      uiApMode ? 1 : 0);

  initialized = true;
  lastMode = mode;
  lastStatus = status;
  lastStaIp = staIp;
  lastApIp = apIp;
  lastWebRunning = webRunning;
  lastWebApMode = webApMode;
  lastUiConnected = uiConnected;
  lastUiApMode = uiApMode;
  lastLogMs = now;
}

void updateWifiUiStateFromSystem() {
  const wifi_mode_t mode = WiFi.getMode();
  const wl_status_t status = WiFi.status();
  const IPAddress staIp = WiFi.localIP();
  const IPAddress apIp = WiFi.softAPIP();

  const bool webRunning = webConfigRunning();
  const bool webApMode = webRunning && webConfigIsApMode();

  const bool apMode = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
  const bool staConnected = (status == WL_CONNECTED);
  const bool hasRealStaIp = !ipIsZero(staIp) && !ipIsApDefault(staIp);

  const bool uiApMode = webRunning ? webApMode : apMode;
  const bool uiConnected = webRunning ? true : (apMode || staConnected || hasRealStaIp);

  UI::setWifiState(uiConnected, uiApMode);
  logWifiDecision("update", mode, status, staIp, apIp, webRunning, webApMode, uiConnected,
                  uiApMode);
}

int readTouchRaw(uint8_t* buffer, uint8_t length) {
  touchWire.beginTransmission(kTouchI2cAddress);
  touchWire.write(static_cast<uint8_t>(0x00));
  if (touchWire.endTransmission(false) != 0) {
    return -1;
  }

  const int requested = touchWire.requestFrom(static_cast<int>(kTouchI2cAddress),
                                              static_cast<int>(length), true);
  if (requested != length) {
    return -1;
  }

  int count = 0;
  while (touchWire.available() > 0 && count < length) {
    buffer[count++] = static_cast<uint8_t>(touchWire.read());
  }
  return count;
}

bool readTouchPointRaw(uint16_t* rawX, uint16_t* rawY) {
  uint8_t rdBuf[kTouchReadBytes] = {0};
  if (readTouchRaw(rdBuf, kTouchReadBytes) != kTouchReadBytes) {
    return false;
  }

  const uint8_t pointNum = rdBuf[2] & 0x07;
  if ((rdBuf[2] == 0 && rdBuf[3] == 0 && rdBuf[4] == 0 && rdBuf[6] == 0) ||
      (rdBuf[2] == 0xFF && rdBuf[3] == 0xFF && rdBuf[4] == 0xFF && rdBuf[6] == 0xFF)) {
    return false;
  }

  if (pointNum == 0 || (rdBuf[3] & 0xC0) == 0xC0) {
    return false;
  }

  *rawX = static_cast<uint16_t>((rdBuf[3] & 0x0F) << 8) | static_cast<uint16_t>(rdBuf[4]);
  *rawY = static_cast<uint16_t>((rdBuf[5] & 0x0F) << 8) | static_cast<uint16_t>(rdBuf[6]);
  return true;
}

bool mapTouchToUi(uint16_t rawX, uint16_t rawY, int16_t* x, int16_t* y) {
  // Flipped landscape mapping. Raw CHSC6x is typically 240x320.
  int16_t mappedX = static_cast<int16_t>(kUiScreenWidth - 1 - rawY);
  int16_t mappedY = static_cast<int16_t>(rawX);

  // Fallback if a panel reports non-standard coordinate orientation.
  if (rawX > 239 || rawY > 319) {
    mappedX = static_cast<int16_t>(rawX);
    mappedY = static_cast<int16_t>(rawY);
  }

  if (mappedX < 0 || mappedX >= kUiScreenWidth || mappedY < 0 || mappedY >= kUiScreenHeight) {
    return false;
  }

  *x = mappedX;
  *y = mappedY;
  return true;
}

void prepareGpsPower() {
  if (BoardPins::GPS_ENABLE >= 0) {
    pinMode(BoardPins::GPS_ENABLE, OUTPUT);
    digitalWrite(BoardPins::GPS_ENABLE, BoardPins::GPS_ENABLE_ACTIVE);
  }

  if (BoardPins::GPS_RESET >= 0) {
    pinMode(BoardPins::GPS_RESET, OUTPUT);
    digitalWrite(BoardPins::GPS_RESET, !BoardPins::GPS_RESET_ACTIVE);
  }

  if (BoardPins::GPS_STANDBY >= 0) {
    pinMode(BoardPins::GPS_STANDBY, OUTPUT);
    digitalWrite(BoardPins::GPS_STANDBY, HIGH);
  }

  delay(30);
}

int readBatteryPercent() {
  if (BoardPins::BATTERY_ADC < 0) {
    return -1;
  }

  constexpr int kSamples = 8;
  uint32_t sum = 0;
  for (int i = 0; i < kSamples; ++i) {
    sum += static_cast<uint32_t>(analogRead(BoardPins::BATTERY_ADC));
    delayMicroseconds(200);
  }

  const float raw = static_cast<float>(sum) / static_cast<float>(kSamples);
  const float vadc = raw * (3.3f / 4095.0f);
  const float vbat = vadc * BoardPins::BATTERY_DIVIDER;
  const float ratio = (vbat - BoardPins::BATTERY_VMIN) /
                      (BoardPins::BATTERY_VMAX - BoardPins::BATTERY_VMIN);
  int percent = static_cast<int>(roundf(ratio * 100.0f));
  if (percent < 0) {
    percent = 0;
  }
  if (percent > 100) {
    percent = 100;
  }
  return percent;
}

void initTouchInput() {
  if (BoardPins::TOUCH_RST >= 0) {
    pinMode(BoardPins::TOUCH_RST, OUTPUT);
    digitalWrite(BoardPins::TOUCH_RST, HIGH);
    delay(2);
    digitalWrite(BoardPins::TOUCH_RST, LOW);
    delay(20);
    digitalWrite(BoardPins::TOUCH_RST, HIGH);
    delay(30);
  }

  touchWire.begin(BoardPins::TOUCH_SDA, BoardPins::TOUCH_SCL, 400000U);

  uint8_t probe[kTouchReadBytes] = {0};
  touchOnline = readTouchRaw(probe, kTouchReadBytes) == kTouchReadBytes;
  if (touchOnline) {
    Serial.println("[TOUCH] CHSC6x online");
  } else {
    Serial.println("[TOUCH] CHSC6x not detected; touch actions disabled");
  }
}

void handleMainScreenTouch(int16_t x, int16_t y) {
  if (y < 56) {
    if (x < (kUiScreenWidth / 2)) {
      sendBeaconNow();
      Serial.println("[UI] touch: beacon send");
    } else {
      openConversationsFromMain();
      Serial.println("[UI] touch: open conversations");
    }
    UI::render(true);
    return;
  }

  if (y > (kUiScreenHeight - 36)) {
    if (x < (kUiScreenWidth / 2)) {
      UI::scrollRxPageNewer();
      Serial.println("[UI] touch: rx page newer");
    } else {
      UI::scrollRxPageOlder();
      Serial.println("[UI] touch: rx page older");
    }
    UI::render(true);
    return;
  }

  if (x < (kUiScreenWidth / 2)) {
    UI::scrollRxNewer();
    Serial.println("[UI] touch: rx newer");
  } else {
    UI::scrollRxOlder();
    Serial.println("[UI] touch: rx older");
  }
  UI::render(true);
}

void pollTouchInput() {
  if (!touchOnline) {
    return;
  }

  const uint32_t now = millis();
  if ((now - lastTouchPollMs) < kTouchPollIntervalMs) {
    return;
  }
  lastTouchPollMs = now;

  uint16_t rawX = 0;
  uint16_t rawY = 0;
  const bool touchDown = readTouchPointRaw(&rawX, &rawY);

  if (!touchDown) {
    touchWasDown = false;
    return;
  }

  if (touchWasDown) {
    return;
  }
  touchWasDown = true;

  if ((now - lastTouchActionMs) < kTouchDebounceMs) {
    return;
  }

  int16_t x = 0;
  int16_t y = 0;
  if (!mapTouchToUi(rawX, rawY, &x, &y)) {
    return;
  }

  lastTouchActionMs = now;
  if (UI::currentScreen() == UI::Screen::Conversations) {
    openMainFromConversations();
    Serial.println("[UI] touch: back to Main");
    return;
  }

  handleMainScreenTouch(x, y);
}

void onRadioIrq() { radioIrqFired = true; }

void prepareBoardPower() {
  if (BoardPins::BOARD_POWERON >= 0) {
    pinMode(BoardPins::BOARD_POWERON, OUTPUT);
    digitalWrite(BoardPins::BOARD_POWERON, BoardPins::BOARD_POWERON_ACTIVE);
  }

  // Deselect shared SPI devices before display init.
  pinMode(BoardPins::RADIO_CS, OUTPUT);
  pinMode(BoardPins::TFT_CS_PIN, OUTPUT);
  digitalWrite(BoardPins::RADIO_CS, HIGH);
  digitalWrite(BoardPins::TFT_CS_PIN, HIGH);

  if (BoardPins::BOARD_SDCARD_CS >= 0) {
    pinMode(BoardPins::BOARD_SDCARD_CS, OUTPUT);
    digitalWrite(BoardPins::BOARD_SDCARD_CS, HIGH);
  }

  delay(300);

  pinMode(BoardPins::TFT_BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BoardPins::TFT_BACKLIGHT_PIN, HIGH);
}

void openConversationsFromMain() {
  if (UI::currentScreen() != UI::Screen::Main) {
    return;
  }

  UI::showScreen(UI::Screen::Conversations);
  UI::render(true);
  Serial.println("[UI] opened Conversations screen");
}

void openMainFromConversations() {
  if (UI::currentScreen() != UI::Screen::Conversations) {
    return;
  }

  UI::showScreen(UI::Screen::Main);
  UI::render(true);
  Serial.println("[UI] touch back: Main screen");
}

bool initRadio() {
  int state = radio.begin(gRuntimeCfg.frequencyMhz);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] init failed, code %d\n", state);
    UI::noteStatus("radio init failed");
    return false;
  }

  applyRuntimeRadioConfig();

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
  radioReady = true;
  return true;
}

void applyRuntimeRadioConfig() {
  int state = radio.setSpreadingFactor(gRuntimeCfg.spreadingFactor);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] setSpreadingFactor failed, code %d\n", state);
    UI::noteStatus("radio sf error");
  }

  state = radio.setBandwidth(gRuntimeCfg.bandwidthKhz);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] setBandwidth failed, code %d\n", state);
    UI::noteStatus("radio bw error");
  }

  state = radio.setCodingRate(gRuntimeCfg.codingRate);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] setCodingRate failed, code %d\n", state);
    UI::noteStatus("radio cr error");
  }

  state = radio.setOutputPower(gRuntimeCfg.txPowerDbm);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] setOutputPower failed, code %d\n", state);
    UI::noteStatus("radio power error");
  }

  state = radio.setFrequency(gRuntimeCfg.frequencyMhz);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] setFrequency failed, code %d\n", state);
    UI::noteStatus("radio freq error");
  }
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
  if (!radioReady) {
    Serial.println("[TX] radio offline, packet not sent");
    UI::noteStatus("radio offline");
    return;
  }

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
  if (!radioReady) {
    return;
  }

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
  return AprsCodec::buildPositionPacket(
      String(gRuntimeCfg.callsign), String(gRuntimeCfg.destination), String(gRuntimeCfg.path),
      gps.location.lat(), gps.location.lng(), gps.course.deg(), gps.speed.knots(),
      static_cast<long>(gps.altitude.feet()), gRuntimeCfg.symbolTable, gRuntimeCfg.symbol,
      String(gRuntimeCfg.comment));
}

bool sendBeaconWithPosition(double lat,
                            double lon,
                            double courseDeg,
                            double speedKnots,
                            long altitudeFeet) {
  String packet = AprsCodec::buildPositionPacket(
      String(gRuntimeCfg.callsign), String(gRuntimeCfg.destination), String(gRuntimeCfg.path), lat,
      lon, courseDeg, speedKnots, altitudeFeet, gRuntimeCfg.symbolTable, gRuntimeCfg.symbol,
      String(gRuntimeCfg.comment));
  lastBeaconMs = millis();
  transmitAprs(packet);
  return true;
}

bool sendBeaconNow() {
  if (gps.location.isValid()) {
    return sendBeaconWithPosition(gps.location.lat(), gps.location.lng(), gps.course.deg(),
                                  gps.speed.knots(), static_cast<long>(gps.altitude.feet()));
  }

  if (gRuntimeCfg.allowManualPosition) {
    const double lat = static_cast<double>(gRuntimeCfg.manualLatE7) / 1e7;
    const double lon = static_cast<double>(gRuntimeCfg.manualLonE7) / 1e7;
    const long altFeet = static_cast<long>(roundf(gRuntimeCfg.manualAltMeters * 3.28084f));
    UI::noteStatus("using manual position");
    return sendBeaconWithPosition(lat, lon, 0.0, 0.0, altFeet);
  }

  Serial.println("[GPS] no fix, beacon not sent");
  UI::noteStatus("no gps fix");
  return false;
}

void maybeSendBeacon() {
  if (!radioReady) {
    return;
  }

  if (!gps.location.isValid()) {
    const uint32_t now = millis();

    if (now > 30000 && gps.charsProcessed() < 10 && (now - lastGpsNoDataLogMs) >= 10000) {
      Serial.println("[GPS] no serial data from module (check wiring/pin order)");
      UI::noteStatus("gps no serial data");
      lastGpsNoDataLogMs = now;
    }

    if (now - lastNoFixLogMs >= gRuntimeCfg.noFixLogIntervalMs) {
      Serial.println("[GPS] waiting for valid fix");
      UI::noteStatus("waiting for gps fix");
      lastNoFixLogMs = now;
    }
    return;
  }

  const uint32_t now = millis();
  if (now - lastBeaconMs < gRuntimeCfg.beaconIntervalMs) {
    return;
  }

  sendBeaconNow();
}

void printStatus() {
  Serial.printf("[STATUS] gps.valid=%d sats=%d lat=%.6f lon=%.6f speed=%.1fkmh\n",
                gps.location.isValid() ? 1 : 0, gps.satellites.value(), gps.location.lat(),
                gps.location.lng(), gps.speed.kmph());
  Serial.printf("[STATUS] aprs=%s>%s,%s freq=%.3f sf=%d bw=%.1f cr=%d pwr=%d bcn=%lus\n",
                gRuntimeCfg.callsign, gRuntimeCfg.destination, gRuntimeCfg.path,
                gRuntimeCfg.frequencyMhz, gRuntimeCfg.spreadingFactor, gRuntimeCfg.bandwidthKhz,
                gRuntimeCfg.codingRate, gRuntimeCfg.txPowerDbm,
                static_cast<unsigned long>(gRuntimeCfg.beaconIntervalMs / 1000UL));

  if (webConfigRunning()) {
    Serial.printf("[STATUS] web mode=%s ip=%s ssid=%s\n",
                  webConfigIsApMode() ? "ap-fallback" : "sta", webConfigIP(),
                  webConfigIsApMode() ? webConfigSsid() : gRuntimeCfg.wifiSsid);
  }
}

void onWebConfigSaved() {
  UI::setCallsign(String(gRuntimeCfg.callsign));

  if (radioReady) {
    applyRuntimeRadioConfig();
    radio.startReceive();
  }

  UI::noteStatus("config saved");
  UI::render(true);
  Serial.println("[WEB] config saved and applied");
}

void handleSerialLine(const String& line) {
  if (line == "help") {
    Serial.println("Commands:");
    Serial.println("  help                 Show this help");
    Serial.println("  status               Show GPS and runtime status");
    Serial.println("  wifi                 Show detailed Wi-Fi state");
    Serial.println("  web                  Show web config URL and mode");
    Serial.println("  beacon               Send APRS beacon now (if GPS fix is valid)");
    Serial.println("  c                    Toggle Main and Conversations screens");
    Serial.println("  screen next|prev     Switch between Main and Conversations");
    Serial.println("  screen main|convos   Jump to a specific screen");
    Serial.println("  enter                On Main screen: send beacon now");
    Serial.println("  tx <TNC2_PACKET>     Send a raw APRS packet");
    Serial.println("  scroll newer|older   Scroll Main RX list by one entry");
    Serial.println("  scroll pageup|pagedown  Scroll Main RX list by one page");
    Serial.println("  scroll top           Jump Main RX list to newest entry");
    return;
  }

  if (line == "status") {
    printStatus();
    return;
  }

  if (line == "wifi") {
    const wifi_mode_t mode = WiFi.getMode();
    const wl_status_t status = WiFi.status();
    const IPAddress staIp = WiFi.localIP();
    const IPAddress apIp = WiFi.softAPIP();
    const bool webRunning = webConfigRunning();
    const bool webApMode = webRunning && webConfigIsApMode();
    const bool apMode = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
    const bool staConnected = (status == WL_CONNECTED);
    const bool hasRealStaIp = !ipIsZero(staIp) && !ipIsApDefault(staIp);
    const bool uiApMode = webRunning ? webApMode : apMode;
    const bool uiConnected = webRunning ? true : (apMode || staConnected || hasRealStaIp);

    logWifiDecision("cmd", mode, status, staIp, apIp, webRunning, webApMode, uiConnected,
                    uiApMode, true);
    return;
  }

  if (line == "web") {
    if (webConfigRunning()) {
      if (webConfigIsApMode()) {
        Serial.printf("[WEB] AP SSID=%s IP=http://%s\n", webConfigSsid(), webConfigIP());
      } else {
        Serial.printf("[WEB] STA IP=http://%s\n", webConfigIP());
      }
    } else {
      Serial.println("[WEB] web config not running");
    }
    return;
  }

  if (line == "beacon") {
    sendBeaconNow();
    return;
  }

  if (line == "c") {
    if (UI::currentScreen() == UI::Screen::Main) {
      openConversationsFromMain();
    } else if (UI::currentScreen() == UI::Screen::Conversations) {
      openMainFromConversations();
    } else {
      Serial.println("[UI] c command not available on this screen");
    }
    return;
  }

  if (line == "screen next") {
    UI::nextScreen();
    UI::render(true);
    Serial.println("[UI] switched to next screen");
    return;
  }

  if (line == "screen prev" || line == "screen previous") {
    UI::previousScreen();
    UI::render(true);
    Serial.println("[UI] switched to previous screen");
    return;
  }

  if (line == "screen main") {
    UI::showScreen(UI::Screen::Main);
    UI::render(true);
    Serial.println("[UI] switched to Main screen");
    return;
  }

  if (line == "screen convos" || line == "screen conversations") {
    UI::showScreen(UI::Screen::Conversations);
    UI::render(true);
    Serial.println("[UI] switched to Conversations screen");
    return;
  }

  if (line == "enter") {
    if (UI::currentScreen() == UI::Screen::Main) {
      sendBeaconNow();
    } else {
      Serial.println("[UI] enter has no action on Conversations screen");
    }
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

  if (line == "scroll newer" || line == "scroll up") {
    UI::scrollRxNewer();
    UI::render(true);
    Serial.println("[UI] rx list scrolled newer");
    return;
  }

  if (line == "scroll older" || line == "scroll down") {
    UI::scrollRxOlder();
    UI::render(true);
    Serial.println("[UI] rx list scrolled older");
    return;
  }

  if (line == "scroll pageup") {
    UI::scrollRxPageNewer();
    UI::render(true);
    Serial.println("[UI] rx list page up");
    return;
  }

  if (line == "scroll pagedown") {
    UI::scrollRxPageOlder();
    UI::render(true);
    Serial.println("[UI] rx list page down");
    return;
  }

  if (line == "scroll top") {
    UI::resetRxScroll();
    UI::render(true);
    Serial.println("[UI] rx list reset to newest");
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
      if (serialLine.length() == 0 && UI::currentScreen() == UI::Screen::Main) {
        sendBeaconNow();
        UI::render(true);
        Serial.println("[UI] enter: beacon send");
        continue;
      }

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

  runtimeConfigLoad(gRuntimeCfg);

  prepareBoardPower();
  analogReadResolution(12);
  if (BoardPins::BATTERY_ADC >= 0) {
    analogSetPinAttenuation(BoardPins::BATTERY_ADC, ADC_11db);
  }
  initTouchInput();
  prepareGpsPower();
  UI::begin();
  UI::setCallsign(String(gRuntimeCfg.callsign));
  UI::setBatteryPercent(readBatteryPercent());
  UI::showSplash("DAISY APRS", "Starting services");

  // ESP32 HardwareSerial::begin takes (rxPin, txPin).
  gpsSerial.begin(AppConfig::kGpsBaud, SERIAL_8N1, BoardPins::GPS_RX, BoardPins::GPS_TX);
  UI::setGpsState(false, 0, 0.0f);

  Serial.printf("[GPS] Serial1 configured: baud=%lu RX=%d TX=%d\n",
                static_cast<unsigned long>(AppConfig::kGpsBaud), BoardPins::GPS_RX,
                BoardPins::GPS_TX);

  Serial.println();
  Serial.println("Heltec V4 Expansion APRS tracker (lean build)");
  Serial.println("Type 'help' in serial monitor for commands.");

  if (!initRadio()) {
    Serial.println("[WARN] radio init failed, continuing without radio");
    UI::noteStatus("radio init failed");
    UI::setRadioState(false);
    restoreDisplaySpiAfterRadioFailure();
  }

  if (webConfigBegin(&gRuntimeCfg, onWebConfigSaved)) {
    if (webConfigIsApMode()) {
      Serial.printf("[WEB] AP fallback running (OPEN): SSID=%s URL=http://%s\n", webConfigSsid(),
                    webConfigIP());
      UI::noteStatus("web cfg ap mode");
    } else {
      Serial.printf("[WEB] Connected to home Wi-Fi. URL=http://%s\n", webConfigIP());
      UI::noteStatus("web cfg wifi mode");
    }
  } else {
    Serial.println("[WEB] failed to start web config server");
  }

  updateWifiUiStateFromSystem();
  {
    const wifi_mode_t mode = WiFi.getMode();
    const wl_status_t status = WiFi.status();
    logWifiDecision("setup", mode, status, WiFi.localIP(), WiFi.softAPIP(), webConfigRunning(),
                    webConfigRunning() && webConfigIsApMode(), true,
                    webConfigRunning() && webConfigIsApMode(), true);
  }

  UI::render(true);
}

void loop() {
  feedGpsParser();
  handleRadioRx();
  maybeSendBeacon();
  webConfigLoop();
  pollTouchInput();
  pollSerialCommands();

  const uint32_t now = millis();
  if (now - lastUiGpsUpdateMs >= 1000) {
    const bool gpsValid = gps.location.isValid();
    const int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
    const float speed = gps.speed.isValid() ? gps.speed.kmph() : 0.0f;
    UI::setGpsState(gpsValid, sats, speed);
    updateWifiUiStateFromSystem();
    lastUiGpsUpdateMs = now;
  }

  if (now - lastBatterySampleMs >= kBatterySampleIntervalMs) {
    UI::setBatteryPercent(readBatteryPercent());
    lastBatterySampleMs = now;
  }

  UI::render();
}
