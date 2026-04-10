#include "ui.h"

#include <SPI.h>
#include <TFT_eSPI.h>
#include <math.h>

namespace {
struct RxEntry {
  String text;
  int rssi;
  int snrTimes10;
  uint32_t timestampMs;
};

TFT_eSPI& tft = *([]() -> TFT_eSPI* {
  static TFT_eSPI instance;
  return &instance;
})();

constexpr int kMaxRxEntries = 7;
RxEntry rxEntries[kMaxRxEntries];
int rxCount = 0;

bool radioReady = false;
bool gpsHasFix = false;
int gpsSatellites = 0;
float gpsSpeed = 0.0f;

String statusText = "booting";
uint32_t lastRenderMs = 0;
uint32_t lastAgeRefreshMs = 0;
bool uiDirty = true;

String summarizePacket(const String& packet) {
  String s = packet;
  s.trim();

  if (s.length() > 44) {
    s = s.substring(0, 44) + "...";
  }

  return s;
}

void drawGradientBackground() {
  const int16_t w = tft.width();
  const int16_t h = tft.height();
  for (int y = 0; y < h; ++y) {
    const uint8_t blend = static_cast<uint8_t>((y * 255) / h);
    const uint8_t r = 8 + blend / 28;
    const uint8_t g = 14 + blend / 12;
    const uint8_t b = 28 + blend / 8;
    tft.drawFastHLine(0, y, w, tft.color565(r, g, b));
  }
}

void drawHeader() {
  tft.fillRoundRect(8, 8, tft.width() - 16, 52, 10, TFT_BLACK);
  tft.drawRoundRect(8, 8, tft.width() - 16, 52, 10, tft.color565(60, 150, 255));

  tft.setTextColor(tft.color565(160, 220, 255), TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(4);
  tft.drawString("DAISY APRS", 18, 14);

  tft.setTextFont(2);
  tft.setTextColor(tft.color565(200, 240, 255), TFT_BLACK);

  char line[80];
  snprintf(line, sizeof(line), "RADIO:%s  GPS:%s(%d)  %.1f km/h", radioReady ? "OK" : "WAIT",
           gpsHasFix ? "FIX" : "NOFIX", gpsSatellites, gpsSpeed);
  tft.drawString(line, 18, 40);
}

void drawStatusBar() {
  tft.fillRoundRect(8, 206, tft.width() - 16, 26, 8, TFT_BLACK);
  tft.drawRoundRect(8, 206, tft.width() - 16, 26, 8, tft.color565(70, 70, 90));

  tft.setTextFont(2);
  tft.setTextColor(tft.color565(245, 210, 120), TFT_BLACK);
  tft.drawString("STATUS: " + statusText, 14, 213);
}

void drawRxList() {
  tft.fillRoundRect(8, 68, tft.width() - 16, 132, 10, tft.color565(4, 10, 20));
  tft.drawRoundRect(8, 68, tft.width() - 16, 132, 10, tft.color565(45, 80, 120));

  tft.setTextFont(2);
  tft.setTextColor(tft.color565(160, 220, 255), tft.color565(4, 10, 20));
  tft.drawString("APRS RX", 16, 74);

  const uint32_t now = millis();
  int y = 92;
  for (int i = 0; i < rxCount; ++i) {
    const RxEntry& entry = rxEntries[i];
    const uint32_t ageSec = (now - entry.timestampMs) / 1000;

    char meta[32];
    snprintf(meta, sizeof(meta), "%lus  %ddBm  %0.1fdB", static_cast<unsigned long>(ageSec),
             entry.rssi, static_cast<float>(entry.snrTimes10) / 10.0f);

    tft.setTextColor(tft.color565(115, 145, 190), tft.color565(4, 10, 20));
    tft.drawString(meta, 16, y);

    tft.setTextColor(TFT_WHITE, tft.color565(4, 10, 20));
    tft.drawString(entry.text, 16, y + 14);

    y += 18;
    if (y > 188) {
      break;
    }
  }

  if (rxCount == 0) {
    tft.setTextColor(tft.color565(150, 170, 200), tft.color565(4, 10, 20));
    tft.drawString("No packets received yet", 16, 116);
    tft.drawString("Listening on LoRa APRS...", 16, 132);
  }
}
}  // namespace

namespace UI {

void begin() {
  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);
}

void showSplash(const String& title, const String& subtitle) {
  drawGradientBackground();

  tft.fillRoundRect(28, 38, tft.width() - 56, 152, 16, TFT_BLACK);
  tft.drawRoundRect(28, 38, tft.width() - 56, 152, 16, tft.color565(80, 170, 255));

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(tft.color565(180, 228, 255), TFT_BLACK);
  tft.setTextFont(4);
  tft.drawString(title, tft.width() / 2, 78);

  tft.setTextFont(2);
  tft.setTextColor(tft.color565(210, 235, 255), TFT_BLACK);
  tft.drawString(subtitle, tft.width() / 2, 104);

  tft.setTextColor(tft.color565(160, 190, 220), TFT_BLACK);
  tft.drawString("LilyGO T-Deck APRS Terminal", tft.width() / 2, 122);

  const int barX = 52;
  const int barY = 156;
  const int barW = tft.width() - 104;
  const int barH = 12;

  tft.drawRoundRect(barX, barY, barW, barH, 6, tft.color565(90, 140, 190));

  for (int i = 0; i <= 100; i += 5) {
    const int fill = (barW - 4) * i / 100;
    tft.fillRoundRect(barX + 2, barY + 2, fill, barH - 4, 4, tft.color565(56, 190, 255));
    delay(28);
  }

  statusText = "ready";
  uiDirty = true;
  render(true);
}

void setRadioState(bool isReady) {
  if (radioReady != isReady) {
    radioReady = isReady;
    uiDirty = true;
  }
}

void setGpsState(bool hasFix, int satellites, float speedKmh) {
  const bool changed = (gpsHasFix != hasFix) || (gpsSatellites != satellites) ||
                       (fabsf(gpsSpeed - speedKmh) > 0.1f);
  gpsHasFix = hasFix;
  gpsSatellites = satellites;
  gpsSpeed = speedKmh;
  if (changed) {
    uiDirty = true;
  }
}

void noteRxPacket(const String& packet, float rssi, float snr) {
  if (rxCount < kMaxRxEntries) {
    ++rxCount;
  }

  for (int i = rxCount - 1; i > 0; --i) {
    rxEntries[i] = rxEntries[i - 1];
  }

  rxEntries[0].text = summarizePacket(packet);
  rxEntries[0].rssi = static_cast<int>(rssi);
  rxEntries[0].snrTimes10 = static_cast<int>(snr * 10.0f);
  rxEntries[0].timestampMs = millis();

  statusText = "packet received";
  uiDirty = true;
}

void noteTxPacket(const String& packet) {
  statusText = "tx " + summarizePacket(packet);
  uiDirty = true;
}

void noteStatus(const String& status) {
  if (statusText != status) {
    statusText = status;
    uiDirty = true;
  }
}

void render(bool force) {
  const uint32_t now = millis();
  const bool refreshAges = (rxCount > 0) && ((now - lastAgeRefreshMs) >= 1000);

  if (!force && !uiDirty && !refreshAges) {
    return;
  }

  if (!force && (now - lastRenderMs) < 80) {
    return;
  }

  if (refreshAges) {
    lastAgeRefreshMs = now;
  }

  uiDirty = false;
  lastRenderMs = now;

  drawGradientBackground();
  drawHeader();
  drawRxList();
  drawStatusBar();
}

}  // namespace UI
