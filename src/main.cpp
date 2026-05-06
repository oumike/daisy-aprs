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
// Global singletons for radio/GPS peripherals on this board.
SPIClass radioSpi(HSPI);
SX1262 radio = new Module(BoardPins::RADIO_CS, BoardPins::RADIO_DIO1, BoardPins::RADIO_RST,
                          BoardPins::RADIO_BUSY, radioSpi);
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
constexpr uint32_t kTouchButtonLockoutMs = 5000;
constexpr uint16_t kTouchReadBytes = 16;
constexpr int16_t kUiScreenWidth = 320;
constexpr int16_t kUiScreenHeight = 240;
constexpr int16_t kMainPanelW = kUiScreenWidth - 16;
constexpr int16_t kScreenToggleButtonW = 84;
constexpr int16_t kScreenToggleButtonH = 24;
constexpr int16_t kScreenToggleButtonX = kUiScreenWidth - kScreenToggleButtonW - 14;
constexpr int16_t kScreenToggleButtonY = 34;
constexpr int16_t kActionButtonGap = 4;
constexpr int16_t kActionButtonW = ((kMainPanelW - 16) - (4 * kActionButtonGap)) / 5;
constexpr int16_t kBeaconButtonX = 16;
constexpr int16_t kBeaconButtonY = 173;
constexpr int16_t kBeaconButtonW = kActionButtonW;
constexpr int16_t kBeaconButtonH = 22;
constexpr int16_t kTestButtonX = kBeaconButtonX + kBeaconButtonW + kActionButtonGap;
constexpr int16_t kTestButtonY = kBeaconButtonY;
constexpr int16_t kTestButtonW = kActionButtonW;
constexpr int16_t kTestButtonH = kBeaconButtonH;
constexpr int16_t kAprsphButtonX = kTestButtonX + kTestButtonW + kActionButtonGap;
constexpr int16_t kAprsphButtonY = kBeaconButtonY;
constexpr int16_t kAprsphButtonW = kActionButtonW;
constexpr int16_t kAprsphButtonH = kBeaconButtonH;
constexpr int16_t kThursButtonX = kAprsphButtonX + kAprsphButtonW + kActionButtonGap;
constexpr int16_t kThursButtonY = kBeaconButtonY;
constexpr int16_t kThursButtonW = kActionButtonW;
constexpr int16_t kThursButtonH = kBeaconButtonH;
constexpr int16_t kWxButtonX = kThursButtonX + kThursButtonW + kActionButtonGap;
constexpr int16_t kWxButtonY = kBeaconButtonY;
constexpr int16_t kWxButtonW = kActionButtonW;
constexpr int16_t kWxButtonH = kBeaconButtonH;
constexpr uint32_t kBatterySampleIntervalMs = 10000;

TwoWire touchWire(1);
uint32_t lastTouchPollMs = 0;
uint32_t lastTouchActionMs = 0;
uint32_t touchButtonLockoutUntilMs = 0;
bool touchOnline = false;
bool touchWasDown = false;
bool radioReady = false;
uint32_t lastUserActivityMs = 0;
bool displaySleeping = false;
bool wakeButtonLastRawLevel = false;
bool wakeButtonStableLevel = false;
bool wakeButtonIdleLevel = false;
uint32_t wakeButtonLastRawChangeMs = 0;
uint32_t lastWakeButtonActionMs = 0;

constexpr uint32_t kWakeButtonDebounceMs = 180;

bool readWakeButtonLevel() {
  return digitalRead(BoardPins::WAKE_BUTTON) == HIGH;
}

bool detectWakeButtonIdleLevel() {
  int highCount = 0;
  int lowCount = 0;
  constexpr int kSamples = 16;
  for (int i = 0; i < kSamples; ++i) {
    if (readWakeButtonLevel()) {
      ++highCount;
    } else {
      ++lowCount;
    }
    delay(2);
  }
  return highCount >= lowCount;
}

RuntimeConfig gRuntimeCfg;

String serialLine;

struct BatteryReading {
  float adcVoltage;
  float voltageBeforeCalibration;
  float voltage;
  float avgMilliVolts;
  float avgRawAdc;
  int milliVoltSampleCount;
  bool usedRawFallback;
  int percent;
  int linearPercent;
  float curveLowVoltage;
  float curveHighVoltage;
  int curveLowPercent;
  int curveHighPercent;
  float curveInterpolation;
  bool valid;
};

bool sendBeaconNow();
bool sendBeaconWithPosition(double lat, double lon, double courseDeg, double speedKnots,
                            long altitudeFeet);
void sendTestPacket();
void sendAprsphPacket();
void sendThursPacket();
void sendWxBotPacket();
void openConversationsFromMain();
void openMainFromConversations();
bool handleLogScreenTouch(int16_t x, int16_t y);
void transmitAprs(const String& aprsText);
void applyRuntimeRadioConfig();
void onWebConfigSaved();
BatteryReading readBattery();
void printBatteryStatus(bool detailed = false);
int readBatteryPercent();
void noteUserActivity();
void maybeHandleScreenTimeout();
void initWakeButton();
void pollWakeButton();
void updateWebSelfPosition();

void showMainScreen(const char* logMessage = nullptr) {
  UI::showScreen(UI::Screen::Main);
  UI::render(true);
  if (logMessage) {
    Serial.println(logMessage);
  }
}

void showMessagesScreen(const char* logMessage = nullptr) {
  UI::showScreen(UI::Screen::Conversations);
  UI::render(true);
  if (logMessage) {
    Serial.println(logMessage);
  }
}

void applyMessageScroll(void (*scrollFn)(), const char* logMessage) {
  scrollFn();
  UI::render(true);
  Serial.println(logMessage);
}

void setDisplayBacklight(bool on) {
  if (BoardPins::TFT_BACKLIGHT_PIN >= 0) {
    digitalWrite(BoardPins::TFT_BACKLIGHT_PIN, on ? HIGH : LOW);
  }
}

void noteUserActivity() {
  lastUserActivityMs = millis();

  if (!displaySleeping) {
    return;
  }

  setDisplayBacklight(true);
  displaySleeping = false;
  UI::render(true);
  Serial.println("[UI] display wake");
}

void maybeHandleScreenTimeout() {
  if (displaySleeping || gRuntimeCfg.screenTimeoutSec == 0) {
    return;
  }

  const uint32_t timeoutMs = static_cast<uint32_t>(gRuntimeCfg.screenTimeoutSec) * 1000UL;
  const uint32_t now = millis();
  if ((now - lastUserActivityMs) < timeoutMs) {
    return;
  }

  setDisplayBacklight(false);
  displaySleeping = true;
  Serial.printf("[UI] display sleep after %us inactivity\n",
                static_cast<unsigned int>(gRuntimeCfg.screenTimeoutSec));
}

void initWakeButton() {
  if (BoardPins::WAKE_BUTTON < 0) {
    return;
  }

  pinMode(BoardPins::WAKE_BUTTON, INPUT_PULLUP);

  wakeButtonIdleLevel = detectWakeButtonIdleLevel();
  wakeButtonStableLevel = wakeButtonIdleLevel;
  wakeButtonLastRawLevel = wakeButtonIdleLevel;
  wakeButtonLastRawChangeMs = millis();

  Serial.printf("[UI] wake button pin=%d idle=%s\n", BoardPins::WAKE_BUTTON,
                wakeButtonIdleLevel ? "HIGH" : "LOW");
}

void pollWakeButton() {
  if (BoardPins::WAKE_BUTTON < 0) {
    return;
  }

  const uint32_t now = millis();
  const bool rawLevel = readWakeButtonLevel();

  if (rawLevel != wakeButtonLastRawLevel) {
    wakeButtonLastRawLevel = rawLevel;
    wakeButtonLastRawChangeMs = now;
    return;
  }

  if ((now - wakeButtonLastRawChangeMs) < kWakeButtonDebounceMs) {
    return;
  }

  if (wakeButtonStableLevel == rawLevel) {
    return;
  }

  wakeButtonStableLevel = rawLevel;
  const bool pressed = (wakeButtonStableLevel != wakeButtonIdleLevel);
  if (!pressed) {
    return;
  }

  if ((now - lastWakeButtonActionMs) < kWakeButtonDebounceMs) {
    return;
  }

  lastWakeButtonActionMs = now;
  if (displaySleeping) {
    noteUserActivity();
  } else {
    setDisplayBacklight(false);
    displaySleeping = true;
    Serial.println("[UI] display sleep (button)");
  }
}

void restoreDisplaySpiAfterRadioFailure() {
#if defined(TFT_SCLK) && defined(TFT_MISO) && defined(TFT_MOSI) && defined(TFT_CS)
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  Serial.printf("[TFT] Restored SPI pins after radio init failure (SCK=%d MISO=%d MOSI=%d CS=%d)\n",
                TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
#else
  Serial.println("[TFT] TFT pin macros not available; skipped SPI restore after radio failure");
#endif
}

bool ipIsZero(const IPAddress& ip) {
  return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
}

bool ipIsApDefault(const IPAddress& ip) {
  return ip[0] == 192 && ip[1] == 168 && ip[2] == 4 && ip[3] == 1;
}

void updateWifiUiStateFromSystem() {
  const wifi_mode_t mode = WiFi.getMode();
  const wl_status_t status = WiFi.status();
  const IPAddress staIp = WiFi.localIP();

  const bool webRunning = webConfigRunning();
  const bool webApMode = webRunning && webConfigIsApMode();

  const bool apMode = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
  const bool staConnected = (status == WL_CONNECTED);
  const bool hasRealStaIp = !ipIsZero(staIp) && !ipIsApDefault(staIp);

  const bool uiApMode = webRunning ? webApMode : apMode;
  const bool uiConnected = webRunning ? !webApMode : (apMode || staConnected || hasRealStaIp);

  UI::setWifiState(uiConnected, uiApMode);
}

void updateWebSelfPosition() {
  if (gps.location.isValid()) {
    webConfigSetSelfPosition(true, gps.location.lat(), gps.location.lng());
    return;
  }

  if (gRuntimeCfg.allowManualPosition) {
    const double lat = static_cast<double>(gRuntimeCfg.manualLatE7) / 1e7;
    const double lon = static_cast<double>(gRuntimeCfg.manualLonE7) / 1e7;
    webConfigSetSelfPosition(true, lat, lon);
    return;
  }

  webConfigSetSelfPosition(false, 0.0, 0.0);
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

float readBatteryAdcVoltage(float* outAvgMilliVolts,
                           int* outMilliVoltSampleCount,
                           bool* outUsedRawFallback,
                           float* outAvgRawAdc) {
  if (outAvgMilliVolts) {
    *outAvgMilliVolts = 0.0f;
  }
  if (outMilliVoltSampleCount) {
    *outMilliVoltSampleCount = 0;
  }
  if (outUsedRawFallback) {
    *outUsedRawFallback = false;
  }
  if (outAvgRawAdc) {
    *outAvgRawAdc = 0.0f;
  }

  if (BoardPins::BATTERY_ADC < 0) {
    return 0.0f;
  }

  if (BoardPins::BATTERY_ADC_CTRL >= 0) {
    pinMode(BoardPins::BATTERY_ADC_CTRL, OUTPUT);
    digitalWrite(BoardPins::BATTERY_ADC_CTRL, BoardPins::BATTERY_ADC_CTRL_ACTIVE);
    delayMicroseconds(200);
  }

  constexpr int kSamples = 8;
  uint32_t sumMv = 0;
  int validMvSamples = 0;
  for (int i = 0; i < kSamples; ++i) {
    const uint32_t mv = analogReadMilliVolts(BoardPins::BATTERY_ADC);
    if (mv > 0) {
      sumMv += mv;
      ++validMvSamples;
    }
    delayMicroseconds(200);
  }

  if (validMvSamples > 0) {
    const float avgMv = static_cast<float>(sumMv) / static_cast<float>(validMvSamples);
    if (outAvgMilliVolts) {
      *outAvgMilliVolts = avgMv;
    }
    if (outMilliVoltSampleCount) {
      *outMilliVoltSampleCount = validMvSamples;
    }
    return avgMv / 1000.0f;
  }

  // Fallback for boards/cores where calibrated millivolt reads are unavailable.
  if (outUsedRawFallback) {
    *outUsedRawFallback = true;
  }
  uint32_t sumRaw = 0;
  for (int i = 0; i < kSamples; ++i) {
    sumRaw += static_cast<uint32_t>(analogRead(BoardPins::BATTERY_ADC));
    delayMicroseconds(200);
  }

  const float raw = static_cast<float>(sumRaw) / static_cast<float>(kSamples);
  if (outAvgRawAdc) {
    *outAvgRawAdc = raw;
  }
  return raw * (3.1f / 4095.0f);
}

int linearPercentFromVoltage(float vbat) {
  const float denom = BoardPins::BATTERY_VMAX - BoardPins::BATTERY_VMIN;
  if (denom <= 0.001f) {
    return 0;
  }

  const float ratio = (vbat - BoardPins::BATTERY_VMIN) / denom;
  int percent = static_cast<int>(roundf(ratio * 100.0f));
  if (percent < 0) {
    percent = 0;
  }
  if (percent > 100) {
    percent = 100;
  }
  return percent;
}

int batteryPercentFromVoltage(float vbat,
                             float* outLowVoltage,
                             float* outHighVoltage,
                             int* outLowPercent,
                             int* outHighPercent,
                             float* outInterpolation) {
  struct SocPoint {
    float voltage;
    int percent;
  };

  // Li-ion discharge curve approximation (single cell under light load).
  static constexpr SocPoint kSocCurve[] = {
      {4.20f, 100}, {4.15f, 95}, {4.11f, 90}, {4.08f, 85}, {4.02f, 80},
      {3.98f, 75},  {3.95f, 70}, {3.92f, 65}, {3.89f, 60}, {3.87f, 55},
      {3.85f, 50},  {3.84f, 45}, {3.82f, 40}, {3.80f, 35}, {3.79f, 30},
      {3.77f, 25},  {3.75f, 20}, {3.73f, 15}, {3.71f, 10}, {3.69f, 5},
      {3.27f, 0},
  };

  if (outLowVoltage) {
    *outLowVoltage = kSocCurve[0].voltage;
  }
  if (outHighVoltage) {
    *outHighVoltage = kSocCurve[0].voltage;
  }
  if (outLowPercent) {
    *outLowPercent = kSocCurve[0].percent;
  }
  if (outHighPercent) {
    *outHighPercent = kSocCurve[0].percent;
  }
  if (outInterpolation) {
    *outInterpolation = 1.0f;
  }

  if (vbat >= kSocCurve[0].voltage) {
    return 100;
  }

  constexpr size_t kCount = sizeof(kSocCurve) / sizeof(kSocCurve[0]);
  for (size_t i = 1; i < kCount; ++i) {
    if (vbat >= kSocCurve[i].voltage) {
      const float vHigh = kSocCurve[i - 1].voltage;
      const float vLow = kSocCurve[i].voltage;
      const int pHigh = kSocCurve[i - 1].percent;
      const int pLow = kSocCurve[i].percent;

      if (outLowVoltage) {
        *outLowVoltage = vLow;
      }
      if (outHighVoltage) {
        *outHighVoltage = vHigh;
      }
      if (outLowPercent) {
        *outLowPercent = pLow;
      }
      if (outHighPercent) {
        *outHighPercent = pHigh;
      }

      const float span = vHigh - vLow;
      if (span <= 0.0f) {
        if (outInterpolation) {
          *outInterpolation = 0.0f;
        }
        return pLow;
      }

      const float t = (vbat - vLow) / span;
      if (outInterpolation) {
        *outInterpolation = t;
      }
      int percent = static_cast<int>(roundf(static_cast<float>(pLow) +
                                            t * static_cast<float>(pHigh - pLow)));
      if (percent < 0) {
        percent = 0;
      }
      if (percent > 100) {
        percent = 100;
      }
      return percent;
    }
  }

  if (outLowVoltage) {
    *outLowVoltage = kSocCurve[kCount - 1].voltage;
  }
  if (outHighVoltage) {
    *outHighVoltage = kSocCurve[kCount - 1].voltage;
  }
  if (outLowPercent) {
    *outLowPercent = kSocCurve[kCount - 1].percent;
  }
  if (outHighPercent) {
    *outHighPercent = kSocCurve[kCount - 1].percent;
  }
  if (outInterpolation) {
    *outInterpolation = 0.0f;
  }

  return 0;
}

BatteryReading readBattery() {
  if (BoardPins::BATTERY_ADC < 0) {
    return {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, false, -1, -1, 0.0f, 0.0f, 0, 0, 0.0f, false};
  }

  float avgMv = 0.0f;
  float avgRawAdc = 0.0f;
  int mvSampleCount = 0;
  bool usedRawFallback = false;
  const float vadc = readBatteryAdcVoltage(&avgMv, &mvSampleCount, &usedRawFallback, &avgRawAdc);
  if (vadc <= 0.01f) {
    return {vadc, 0.0f, 0.0f, avgMv, avgRawAdc, mvSampleCount, usedRawFallback,
            -1,   -1,   0.0f, 0.0f, 0,   0,   0.0f, false};
  }

  const float vbatBeforeCalibration = vadc * BoardPins::BATTERY_DIVIDER;
  const float vbat = (vbatBeforeCalibration * BoardPins::BATTERY_CALIBRATION_GAIN) +
                     BoardPins::BATTERY_CALIBRATION_OFFSET;

  float curveLowVoltage = 0.0f;
  float curveHighVoltage = 0.0f;
  int curveLowPercent = 0;
  int curveHighPercent = 0;
  float curveInterpolation = 0.0f;
  const int percent = batteryPercentFromVoltage(vbat, &curveLowVoltage, &curveHighVoltage,
                                                &curveLowPercent, &curveHighPercent,
                                                &curveInterpolation);
  const int linearPercent = linearPercentFromVoltage(vbat);

  return {vadc,
          vbatBeforeCalibration,
          vbat,
          avgMv,
          avgRawAdc,
          mvSampleCount,
          usedRawFallback,
          percent,
          linearPercent,
          curveLowVoltage,
          curveHighVoltage,
          curveLowPercent,
          curveHighPercent,
          curveInterpolation,
          true};
}

int readBatteryPercent() {
  const BatteryReading reading = readBattery();
  return reading.valid ? reading.percent : -1;
}

void printBatteryStatus(bool detailed) {
  const BatteryReading reading = readBattery();
  if (!reading.valid) {
    Serial.println("[BATTERY] unavailable");
    return;
  }

  Serial.printf("[BATTERY] vbat=%.3fV percent=%d%% (gain=%.3f offset=%.3fV)\n", reading.voltage,
                reading.percent, BoardPins::BATTERY_CALIBRATION_GAIN,
                BoardPins::BATTERY_CALIBRATION_OFFSET);

  if (!detailed) {
    return;
  }

  if (reading.usedRawFallback) {
    Serial.printf("[BATTERY] adc path=raw avg_count=%.1f vadc=%.3fV\n", reading.avgRawAdc,
                  reading.adcVoltage);
  } else {
    Serial.printf("[BATTERY] adc path=millivolts samples=%d avg=%.1fmV vadc=%.3fV\n",
                  reading.milliVoltSampleCount, reading.avgMilliVolts, reading.adcVoltage);
  }

  Serial.printf("[BATTERY] divider=%.3f pre_cal=%.3fV post_cal=%.3fV\n",
                BoardPins::BATTERY_DIVIDER, reading.voltageBeforeCalibration, reading.voltage);
  Serial.printf("[BATTERY] adc_pin=%d adc_ctrl=%d\n", BoardPins::BATTERY_ADC,
                BoardPins::BATTERY_ADC_CTRL);
  Serial.printf("[BATTERY] curve segment: %.2fV(%d%%) .. %.2fV(%d%%), t=%.3f\n",
                reading.curveLowVoltage, reading.curveLowPercent, reading.curveHighVoltage,
                reading.curveHighPercent, reading.curveInterpolation);
  Serial.printf("[BATTERY] percent curve=%d%% linear=%d%%\n", reading.percent,
                reading.linearPercent);
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

bool isBeaconButtonTouch(int16_t x, int16_t y) {
  return x >= kBeaconButtonX && x < (kBeaconButtonX + kBeaconButtonW) && y >= kBeaconButtonY &&
         y < (kBeaconButtonY + kBeaconButtonH);
}

bool isScreenToggleButtonTouch(int16_t x, int16_t y) {
  return x >= kScreenToggleButtonX && x < (kScreenToggleButtonX + kScreenToggleButtonW) &&
         y >= kScreenToggleButtonY && y < (kScreenToggleButtonY + kScreenToggleButtonH);
}

bool isTestButtonTouch(int16_t x, int16_t y) {
  return x >= kTestButtonX && x < (kTestButtonX + kTestButtonW) && y >= kTestButtonY &&
         y < (kTestButtonY + kTestButtonH);
}

bool isAprsphButtonTouch(int16_t x, int16_t y) {
  return x >= kAprsphButtonX && x < (kAprsphButtonX + kAprsphButtonW) &&
         y >= kAprsphButtonY && y < (kAprsphButtonY + kAprsphButtonH);
}

bool isThursButtonTouch(int16_t x, int16_t y) {
  return x >= kThursButtonX && x < (kThursButtonX + kThursButtonW) &&
         y >= kThursButtonY && y < (kThursButtonY + kThursButtonH);
}

bool isWxButtonTouch(int16_t x, int16_t y) {
  return x >= kWxButtonX && x < (kWxButtonX + kWxButtonW) && y >= kWxButtonY &&
         y < (kWxButtonY + kWxButtonH);
}

bool handleScreenToggleTouch(int16_t x, int16_t y) {
  if (!isScreenToggleButtonTouch(x, y)) {
    return false;
  }

  UI::nextScreen();
  Serial.println("[UI] touch: toggle main/messages");
  UI::render();
  return true;
}

bool handleMainScreenTouch(int16_t x, int16_t y) {
  if (UI::isMainDetailActive()) {
    if (UI::handleMainDetailTouch(x, y)) {
      Serial.println("[UI] touch: close main detail");
      UI::render();
    }
    return false;
  }

  if (handleScreenToggleTouch(x, y)) {
    return false;
  }

  if (UI::openMainDetailAt(x, y)) {
    Serial.println("[UI] touch: open main detail");
    UI::render();
    return false;
  }

  if (isBeaconButtonTouch(x, y)) {
    UI::flashButton(UI::TouchButton::Beacon);
    UI::render(true);
    sendBeaconNow();
    Serial.println("[UI] touch: beacon send");
    UI::render();
    return true;
  }

  if (isTestButtonTouch(x, y)) {
    UI::flashButton(UI::TouchButton::Test);
    UI::render(true);
    sendTestPacket();
    Serial.println("[UI] touch: test send");
    UI::render();
    return true;
  }

  if (isAprsphButtonTouch(x, y)) {
    UI::flashButton(UI::TouchButton::Aprsph);
    UI::render(true);
    sendAprsphPacket();
    Serial.println("[UI] touch: APRSPH send");
    UI::render();
    return true;
  }

  if (isThursButtonTouch(x, y)) {
    UI::flashButton(UI::TouchButton::Thurs);
    UI::render(true);
    sendThursPacket();
    Serial.println("[UI] touch: THURS send");
    UI::render();
    return true;
  }

  if (isWxButtonTouch(x, y)) {
    UI::flashButton(UI::TouchButton::Wx);
    UI::render(true);
    sendWxBotPacket();
    Serial.println("[UI] touch: WX send");
    UI::render();
    return true;
  }

  return false;
}

bool handleLogScreenTouch(int16_t x, int16_t y) {
  if (UI::isLogDetailActive()) {
    if (UI::handleLogDetailTouch(x, y)) {
      Serial.println("[UI] touch: close message detail");
      UI::render();
      return false;
    }
    return false;
  }

  if (handleScreenToggleTouch(x, y)) {
    return false;
  }

  if (UI::handleLogScrollButtonTouch(x, y)) {
    Serial.println("[UI] touch: message scroll button");
    UI::render();
    return false;
  }

  if (UI::openLogDetailAt(x, y)) {
    Serial.println("[UI] touch: open message detail");
    UI::render();
    return false;
  }

  if (y > (kUiScreenHeight - 36)) {
    if (x < (kUiScreenWidth / 2)) {
      UI::scrollLogPageNewer();
      Serial.println("[UI] touch: message page newer");
    } else {
      UI::scrollLogPageOlder();
      Serial.println("[UI] touch: message page older");
    }
    UI::render();
    return false;
  }

  if (x < (kUiScreenWidth / 2)) {
    UI::scrollLogNewer();
    Serial.println("[UI] touch: message newer");
  } else {
    UI::scrollLogOlder();
    Serial.println("[UI] touch: message older");
  }
  UI::render();
  return false;
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

  if (displaySleeping) {
    noteUserActivity();
    return;
  }

  if ((now - lastTouchActionMs) < kTouchDebounceMs) {
    return;
  }

  if (static_cast<int32_t>(touchButtonLockoutUntilMs - now) > 0) {
    return;
  }

  int16_t x = 0;
  int16_t y = 0;
  if (!mapTouchToUi(rawX, rawY, &x, &y)) {
    return;
  }

  noteUserActivity();
  lastTouchActionMs = now;
  if (UI::currentScreen() == UI::Screen::Conversations) {
    handleLogScreenTouch(x, y);
    return;
  }

  const bool buttonHandled = handleMainScreenTouch(x, y);
  if (buttonHandled) {
    touchButtonLockoutUntilMs = now + kTouchButtonLockoutMs;
  }
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

  showMessagesScreen("[UI] opened MESSAGES screen");
}

void openMainFromConversations() {
  if (UI::currentScreen() != UI::Screen::Conversations) {
    return;
  }

  showMainScreen("[UI] MESSAGES -> Main screen");
}

bool initRadio() {
  radioSpi.begin(BoardPins::RADIO_SCLK, BoardPins::RADIO_MISO, BoardPins::RADIO_MOSI,
                 BoardPins::RADIO_CS);

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

  // Ignore any stale IRQ raised by previous radio events before a new TX.
  radioIrqFired = false;

  String wire = addWirePrefix(aprsText);

  Serial.printf("[TX] %s\n", aprsText.c_str());
  UI::noteTxPacket(aprsText);
  int state = radio.transmit(wire);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[TX] failed, code %d\n", state);
    UI::noteStatus("tx failed");
  }

  // TX done can also toggle DIO1; clear it before we resume RX handling.
  radioIrqFired = false;

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

  if (payload.length() == 0) {
    radio.startReceive();
    return;
  }

  const float rssi = radio.getRSSI();
  const float snr = radio.getSNR();

  if (hasWirePrefix(payload)) {
    const String aprs = payload.substring(3);
    Serial.printf("[RX] RSSI %.1f SNR %.1f | %s\n", rssi, snr, aprs.c_str());
    UI::noteRxPacket(aprs, rssi, snr);
    webConfigNoteHeardPacket(aprs, rssi, snr);
  } else {
    Serial.printf("[RX] RSSI %.1f SNR %.1f | raw(%d bytes)\n", rssi, snr, payload.length());
    UI::noteRxPacket(String("raw(") + payload.length() + " bytes)", rssi, snr);
  }

  radio.startReceive();
}

String buildAprsMessagePacket(const String& addressee, const String& messageText) {
  String to = AprsCodec::normalizeCallsign(addressee);
  if (to.length() > 9) {
    to = to.substring(0, 9);
  }
  while (to.length() < 9) {
    to += ' ';
  }

  String path = String(gRuntimeCfg.path);
  path.trim();
  if (path.length() == 0) {
    path = "WIDE1-1";
  }

  String packet;
  packet.reserve(180);
  packet += AprsCodec::normalizeCallsign(String(gRuntimeCfg.callsign));
  packet += '>';
  packet += "APRS";
  packet += ',';
  packet += path;

  packet += "::";
  packet += to;
  packet += ':';
  packet += messageText;
  return packet;
}

void sendTestPacket() {
  transmitAprs(buildAprsMessagePacket("APRSPH", "test"));
  UI::noteStatus("test sent to APRSPH");
}

void sendAprsphPacket() {
  String message = String(gRuntimeCfg.aprsphMessage);
  message.trim();
  if (message.length() == 0) {
    message = String(AppConfig::kAprsphMessage);
  }

  String upper = message;
  upper.toUpperCase();
  if (upper == "CQ") {
    message = "";
  } else if (upper.startsWith("CQ ")) {
    message = message.substring(3);
    message.trim();
  }

  const String outbound = message.length() > 0 ? String("CQ ") + message : String("CQ");

  transmitAprs(buildAprsMessagePacket("APRSPH", outbound));
  UI::noteStatus("APRSPH message sent");
}

void sendThursPacket() {
  String message = String(gRuntimeCfg.hotgMessage);
  message.trim();
  if (message.length() == 0) {
    message = String(AppConfig::kHotgMessage);
  }

  transmitAprs(buildAprsMessagePacket("ANSRVR", String("CQ HOTG ") + message));
  UI::noteStatus("THURS message sent");
}

void sendWxBotPacket() {
  double lat = 0.0;
  double lon = 0.0;

  if (gps.location.isValid()) {
    lat = gps.location.lat();
    lon = gps.location.lng();
  } else if (gRuntimeCfg.allowManualPosition) {
    lat = static_cast<double>(gRuntimeCfg.manualLatE7) / 1e7;
    lon = static_cast<double>(gRuntimeCfg.manualLonE7) / 1e7;
  } else {
    Serial.println("[WX] no GPS/manual position available, request not sent");
    UI::noteStatus("wx: no position");
    return;
  }

  char where[40];
  snprintf(where, sizeof(where), "%.3f/%.3f", lat, lon);
  transmitAprs(buildAprsMessagePacket("WXBOT", String(where)));
  UI::noteStatus("wx request sent");
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
  Serial.printf("[STATUS] aprs=%s>%s,%s freq=%.3f sf=%d bw=%.1f cr=%d pwr=%d bcn=%lumin\n",
                gRuntimeCfg.callsign, gRuntimeCfg.destination, gRuntimeCfg.path,
                gRuntimeCfg.frequencyMhz, gRuntimeCfg.spreadingFactor, gRuntimeCfg.bandwidthKhz,
                gRuntimeCfg.codingRate, gRuntimeCfg.txPowerDbm,
                static_cast<unsigned long>(gRuntimeCfg.beaconIntervalMs / 60000UL));
  printBatteryStatus();

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
    Serial.println("  battery              Show battery voltage and percentage");
    Serial.println("  battery debug        Show detailed battery math and curve interpolation");
    Serial.println("  web                  Show web config URL and mode");
    Serial.println("  beacon               Send APRS beacon now (if GPS fix is valid)");
    Serial.println("  c                    Toggle Main and MESSAGES screens");
    Serial.println("  screen next|prev     Switch between Main and MESSAGES");
    Serial.println("  screen main|messages Jump to a specific screen");
    Serial.println("  enter                On Main screen: send beacon now");
    Serial.println("  tx <TNC2_PACKET>     Send a raw APRS packet");
    Serial.println("  scroll newer|older   Scroll MESSAGES by one entry");
    Serial.println("  scroll pageup|pagedown  Scroll MESSAGES by one page");
    Serial.println("  scroll top           Jump MESSAGES to newest entry");
    return;
  }

  if (line == "status") {
    printStatus();
    return;
  }

  if (line == "battery") {
    printBatteryStatus();
    return;
  }

  if (line == "battery debug") {
    printBatteryStatus(true);
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
    showMainScreen("[UI] switched to Main screen");
    return;
  }

  if (line == "screen log" || line == "screen logs" || line == "screen message" ||
      line == "screen messages" || line == "screen convos" ||
      line == "screen conversations") {
    showMessagesScreen("[UI] switched to MESSAGES screen");
    return;
  }

  if (line == "enter") {
    if (UI::currentScreen() == UI::Screen::Main) {
      sendBeaconNow();
    } else {
      Serial.println("[UI] enter has no action on MESSAGES screen");
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
    applyMessageScroll(UI::scrollLogNewer, "[UI] MESSAGES scrolled newer");
    return;
  }

  if (line == "scroll older" || line == "scroll down") {
    applyMessageScroll(UI::scrollLogOlder, "[UI] MESSAGES scrolled older");
    return;
  }

  if (line == "scroll pageup") {
    applyMessageScroll(UI::scrollLogPageNewer, "[UI] MESSAGES page newer");
    return;
  }

  if (line == "scroll pagedown") {
    applyMessageScroll(UI::scrollLogPageOlder, "[UI] MESSAGES page older");
    return;
  }

  if (line == "scroll top") {
    applyMessageScroll(UI::resetLogScroll, "[UI] MESSAGES reset to newest");
    return;
  }

  if (line.length() > 0) {
    Serial.println("[CMD] unknown command, use 'help'");
  }
}

void pollSerialCommands() {
  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());
    noteUserActivity();
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
  initWakeButton();
  analogReadResolution(12);
  if (BoardPins::BATTERY_ADC >= 0) {
    analogSetPinAttenuation(BoardPins::BATTERY_ADC, ADC_11db);
  }
  if (BoardPins::BATTERY_ADC_CTRL >= 0) {
    pinMode(BoardPins::BATTERY_ADC_CTRL, OUTPUT);
    digitalWrite(BoardPins::BATTERY_ADC_CTRL, BoardPins::BATTERY_ADC_CTRL_ACTIVE);
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
      Serial.printf("[WEB] AP fallback running (WPA2): SSID=%s URL=http://%s\n", webConfigSsid(),
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

  UI::render(true);
  setDisplayBacklight(true);
  displaySleeping = false;
  lastUserActivityMs = millis();
}

void loop() {
  feedGpsParser();
  handleRadioRx();
  maybeSendBeacon();
  webConfigLoop();
  pollTouchInput();
  pollWakeButton();
  pollSerialCommands();

  const uint32_t now = millis();
  if (now - lastUiGpsUpdateMs >= 1000) {
    const bool gpsValid = gps.location.isValid();
    const int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
    const float speed = gps.speed.isValid() ? gps.speed.kmph() : 0.0f;
    UI::setGpsState(gpsValid, sats, speed);
    updateWebSelfPosition();
    updateWifiUiStateFromSystem();
    lastUiGpsUpdateMs = now;
  }

  if (now - lastBatterySampleMs >= kBatterySampleIntervalMs) {
    UI::setBatteryPercent(readBatteryPercent());
    lastBatterySampleMs = now;
  }

  maybeHandleScreenTimeout();
  if (!displaySleeping) {
    UI::render();
  }
}
