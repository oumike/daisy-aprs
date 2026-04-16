#include "ui.h"

#include <SPI.h>
#include <TFT_eSPI.h>
#include <math.h>

#include "app_config.h"

namespace {
struct RxEntry {
  String text;
  int rssi;
  int snrTimes10;
  uint32_t timestampMs;
};

struct LogEntry {
  String text;
  bool isTx;
  bool hasRfMeta;
  int rssi;
  int snrTimes10;
  uint32_t timestampMs;
};

struct ConversationEntry {
  String peer;
  String preview;
  uint8_t unreadCount;
};

TFT_eSPI& tft = *([]() -> TFT_eSPI* {
  static TFT_eSPI instance;
  return &instance;
})();

constexpr int kMaxRxEntries = 64;
constexpr int kMaxConversations = 16;
constexpr int kRxPanelX = 8;
constexpr int kRxPanelY = 68;
constexpr int kRxPanelH = 132;
constexpr int kRxTitleY = 74;
constexpr int kLogContentY = 92;
constexpr int kLogContentBottomY = 182;
constexpr int kLogRowHeight = 22;
constexpr int kLogVisibleRows = (kLogContentBottomY - kLogContentY) / kLogRowHeight;
constexpr int kMaxLogEntries = 96;
constexpr int kLogDetailBackX = 16;
constexpr int kLogDetailBackY = 173;
constexpr int kLogDetailBackW = 124;
constexpr int kLogDetailBackH = 22;
constexpr int kScreenToggleButtonW = 84;
constexpr int kScreenToggleButtonH = 16;
constexpr int kScreenToggleButtonX = 320 - kScreenToggleButtonW - 14;
constexpr int kScreenToggleButtonY = 34;

RxEntry rxEntries[kMaxRxEntries];
int rxCount = 0;
String lastTxText;
uint32_t lastTxTimestampMs = 0;
bool hasLastTx = false;
LogEntry logEntries[kMaxLogEntries];
int logCount = 0;
int logScrollOffset = 0;
bool logDetailActive = false;
LogEntry logDetailEntry;
bool mainDetailActive = false;
LogEntry mainDetailEntry;

bool radioReady = false;
bool wifiConnected = false;
bool wifiApMode = false;
int batteryPercent = -1;
bool gpsHasFix = false;
int gpsSatellites = 0;
float gpsSpeed = 0.0f;
UI::Screen activeScreen = UI::Screen::Main;
ConversationEntry conversationEntries[kMaxConversations];
int conversationCount = 0;
bool composerActive = false;
bool composerEnteringMessage = false;
String composerCallsign;
String composerMessage;
String deviceCallsign = "N0CALL-7";
String firmwareVersion = AppConfig::kVersion;

String versionBadgeText() {
  String version = firmwareVersion;
  version.trim();
  if (version.length() == 0) {
    version = "dev";
  }
  return "(" + version + ")";
}

String statusText = "booting";
uint32_t lastRenderMs = 0;
bool uiDirty = true;
bool uiLayoutDirty = true;
UI::TouchButton flashedButton = UI::TouchButton::None;
uint32_t flashButtonUntilMs = 0;
constexpr uint32_t kTouchFlashDurationMs = 220;

bool isButtonFlashing(UI::TouchButton button, uint32_t now) {
  return flashedButton == button && static_cast<int32_t>(flashButtonUntilMs - now) > 0;
}

String summarizePacket(const String& packet) {
  String s = packet;
  s.trim();

  if (s.length() > 33) {
    s = s.substring(0, 33) + "...";
  }

  return s;
}

String normalizePacketText(const String& packet) {
  String s = packet;
  s.trim();
  if (s.length() == 0) {
    s = "(empty)";
  }
  return s;
}

String fitTextToWidth(const String& text, int maxWidthPx) {
  if (tft.textWidth(text) <= maxWidthPx) {
    return text;
  }

  String out = text;
  while (out.length() > 1) {
    const String candidate = out + "...";
    if (tft.textWidth(candidate) <= maxWidthPx) {
      return candidate;
    }
    out.remove(out.length() - 1);
  }

  return "...";
}

String summarizeConversationPreview(const String& text) {
  String s = text;
  s.trim();
  if (s.length() > 29) {
    s = s.substring(0, 29) + "...";
  }
  return s;
}

int maxRxScrollOffset() {
  if (logCount <= kLogVisibleRows) {
    return 0;
  }
  return logCount - kLogVisibleRows;
}

void setRxScrollOffset(int offset) {
  int next = offset;
  if (next < 0) {
    next = 0;
  }

  const int maxOffset = maxRxScrollOffset();
  if (next > maxOffset) {
    next = maxOffset;
  }

  if (next != logScrollOffset) {
    logScrollOffset = next;
    uiDirty = true;
  }
}

int logScrollTrackX() {
  const int panelW = tft.width() - 16;
  return kRxPanelX + panelW - 26;
}

int logScrollButtonX() { return logScrollTrackX() + 6; }
int logScrollButtonW() { return 16; }
int logScrollButtonH() { return 16; }
int logScrollUpButtonY() { return kLogContentBottomY - (2 * logScrollButtonH()) - 4; }
int logScrollDownButtonY() { return kLogContentBottomY - logScrollButtonH(); }

bool isLogScrollUpButtonTouch(int16_t x, int16_t y) {
  const int btnX = logScrollButtonX();
  const int btnY = logScrollUpButtonY();
  const int btnW = logScrollButtonW();
  const int btnH = logScrollButtonH();
  return x >= btnX && x < (btnX + btnW) && y >= btnY && y < (btnY + btnH);
}

bool isLogScrollDownButtonTouch(int16_t x, int16_t y) {
  const int btnX = logScrollButtonX();
  const int btnW = logScrollButtonW();
  const int btnH = logScrollButtonH();
  const int btnY = logScrollDownButtonY();
  return x >= btnX && x < (btnX + btnW) && y >= btnY && y < (btnY + btnH);
}

void drawRxScrollBar() {
  const uint32_t now = millis();
  const int trackX = logScrollTrackX();
  const int trackY = kLogContentY;
  const int trackH = kLogContentBottomY - kLogContentY;
  const bool canScroll = logCount > kLogVisibleRows;
  const int maxOffset = maxRxScrollOffset();

  tft.fillRoundRect(trackX, trackY, 4, trackH, 2, tft.color565(22, 42, 65));

  int thumbH = trackH;
  int thumbY = trackY;
  if (canScroll) {
    thumbH = (trackH * kLogVisibleRows) / logCount;
    if (thumbH < 10) {
      thumbH = 10;
    }

    thumbY = trackY + ((trackH - thumbH) * logScrollOffset) / maxOffset;
  }

  const uint16_t thumbColor = canScroll ? tft.color565(100, 195, 255) : tft.color565(60, 88, 120);
  tft.fillRoundRect(trackX, thumbY, 4, thumbH, 2, thumbColor);

  const int btnX = logScrollButtonX();
  const int btnW = logScrollButtonW();
  const int btnH = logScrollButtonH();
  const int upY = logScrollUpButtonY();
  const int downY = logScrollDownButtonY();

  const bool upPressed = isButtonFlashing(UI::TouchButton::LogScrollUp, now);
  const bool downPressed = isButtonFlashing(UI::TouchButton::LogScrollDown, now);
  const uint16_t upBg = upPressed ? tft.color565(170, 30, 30) : tft.color565(18, 46, 74);
  const uint16_t upBorder = upPressed ? tft.color565(255, 138, 138) : tft.color565(78, 144, 210);
  const uint16_t downBg = downPressed ? tft.color565(170, 30, 30) : tft.color565(18, 46, 74);
  const uint16_t downBorder =
      downPressed ? tft.color565(255, 138, 138) : tft.color565(78, 144, 210);

  tft.fillRoundRect(btnX, upY, btnW, btnH, 3, upBg);
  tft.drawRoundRect(btnX, upY, btnW, btnH, 3, upBorder);
  tft.fillRoundRect(btnX, downY, btnW, btnH, 3, downBg);
  tft.drawRoundRect(btnX, downY, btnW, btnH, 3, downBorder);

  const uint16_t arrowColor = canScroll ? TFT_WHITE : tft.color565(122, 142, 166);
  tft.fillTriangle(btnX + (btnW / 2), upY + 4, btnX + 3, upY + 12, btnX + btnW - 3, upY + 12,
                   arrowColor);
  tft.fillTriangle(btnX + 3, downY + 5, btnX + btnW - 3, downY + 5,
                   btnX + (btnW / 2), downY + 13, arrowColor);
}

String summarizeLogLine(const String& text) {
  String s = text;
  s.trim();
  if (s.length() > 44) {
    s = s.substring(0, 44) + "...";
  }
  return s;
}

bool isLogDetailBackTouch(int16_t x, int16_t y) {
  return x >= kLogDetailBackX && x < (kLogDetailBackX + kLogDetailBackW) &&
         y >= kLogDetailBackY && y < (kLogDetailBackY + kLogDetailBackH);
}

void drawWrappedLogDetailText(const String& text,
                              int x,
                              int y,
                              int maxWidth,
                              int maxLines,
                              uint16_t fg,
                              uint16_t bg) {
  tft.setTextFont(1);
  tft.setTextColor(fg, bg);

  String line;
  int lineIndex = 0;
  for (unsigned int i = 0; i < text.length(); ++i) {
    const char ch = text[i];
    if (ch == '\r') {
      continue;
    }

    if (ch == '\n') {
      tft.drawString(line, x, y + (lineIndex * 10));
      line = "";
      ++lineIndex;
      if (lineIndex >= maxLines) {
        return;
      }
      continue;
    }

    const String trial = line + ch;
    if (tft.textWidth(trial) > maxWidth && line.length() > 0) {
      tft.drawString(line, x, y + (lineIndex * 10));
      ++lineIndex;
      if (lineIndex >= maxLines) {
        tft.drawString("...", x + maxWidth - 18, y + ((maxLines - 1) * 10));
        return;
      }
      line = String(ch);
    } else {
      line = trial;
    }
  }

  if (line.length() > 0 && lineIndex < maxLines) {
    tft.drawString(line, x, y + (lineIndex * 10));
  }
}

void drawDetailScreen(const LogEntry& detailEntry, const char* title, const int panelW,
                      const uint32_t now) {
  const uint32_t ageSec = (now - detailEntry.timestampMs) / 1000;

  tft.setTextFont(2);
  tft.setTextColor(tft.color565(160, 220, 255), tft.color565(4, 10, 20));
  tft.drawString(title, 16, 74);

  char headMeta[24];
  snprintf(headMeta, sizeof(headMeta), "%s  %lus", detailEntry.isTx ? "TX" : "RX",
           static_cast<unsigned long>(ageSec));
  tft.setTextFont(1);
  if (detailEntry.isTx) {
    tft.setTextColor(tft.color565(130, 205, 255), tft.color565(4, 10, 20));
  } else {
    tft.setTextColor(tft.color565(118, 230, 152), tft.color565(4, 10, 20));
  }
  tft.drawString(headMeta, 16, 92);

  if (detailEntry.hasRfMeta) {
    char rfMeta[36];
    snprintf(rfMeta, sizeof(rfMeta), "%ddBm  %0.1fdB", detailEntry.rssi,
             static_cast<float>(detailEntry.snrTimes10) / 10.0f);
    tft.setTextColor(tft.color565(120, 150, 188), tft.color565(4, 10, 20));
    tft.drawString(rfMeta, 16, 102);
  }

  tft.setTextColor(tft.color565(120, 150, 188), tft.color565(4, 10, 20));
  tft.drawString("Message:", 16, 114);
  drawWrappedLogDetailText(detailEntry.text, 16, 124, panelW - 30, 4,
                           tft.color565(230, 236, 245), tft.color565(4, 10, 20));

  tft.fillRoundRect(kLogDetailBackX, kLogDetailBackY, kLogDetailBackW, kLogDetailBackH, 7,
                    tft.color565(30, 92, 145));
  tft.drawRoundRect(kLogDetailBackX, kLogDetailBackY, kLogDetailBackW, kLogDetailBackH, 7,
                    tft.color565(98, 184, 255));
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, tft.color565(30, 92, 145));
  tft.drawString("BACK", kLogDetailBackX + (kLogDetailBackW / 2),
                 kLogDetailBackY + (kLogDetailBackH / 2));
  tft.setTextDatum(TL_DATUM);

  tft.setTextFont(1);
  tft.setTextColor(tft.color565(120, 150, 188), tft.color565(4, 10, 20));
}

void pushLogEntry(bool isTx, const String& text, int rssi, int snrTimes10, bool hasRfMeta) {
  if (logCount < kMaxLogEntries) {
    ++logCount;
  }

  for (int i = logCount - 1; i > 0; --i) {
    logEntries[i] = logEntries[i - 1];
  }

  logEntries[0].text = normalizePacketText(text);
  logEntries[0].isTx = isTx;
  logEntries[0].hasRfMeta = hasRfMeta;
  logEntries[0].rssi = rssi;
  logEntries[0].snrTimes10 = snrTimes10;
  logEntries[0].timestampMs = millis();
  logScrollOffset = 0;
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
  constexpr const char* kTitle = "DAISY APRS";
  tft.drawString(kTitle, 18, 14);

  const String versionBadge = versionBadgeText();
  tft.setTextFont(1);
  tft.setTextColor(tft.color565(180, 208, 232), TFT_BLACK);
  int versionX = 18 + tft.textWidth(kTitle, 4) + 6;
  const int maxVersionX = tft.width() - 12 - tft.textWidth(versionBadge, 1);
  if (versionX > maxVersionX) {
    versionX = maxVersionX;
  }
  if (versionX < 18) {
    versionX = 18;
  }
  tft.drawString(versionBadge, versionX, 14);

  tft.setTextFont(2);
  tft.setTextColor(tft.color565(200, 240, 255), TFT_BLACK);
  tft.drawString(deviceCallsign, 18, 40);
}

void drawStatusBar() {
  tft.fillRoundRect(8, 206, tft.width() - 16, 26, 8, TFT_BLACK);
  tft.drawRoundRect(8, 206, tft.width() - 16, 26, 8, tft.color565(70, 70, 90));

  const int batteryBadgeW = 56;
  const int wifiBadgeW = 62;
  const int badgeH = 18;
  const int batteryBadgeX = tft.width() - batteryBadgeW - 14;
  const int wifiBadgeX = batteryBadgeX - wifiBadgeW - 8;
  const int badgeY = 210;
  const int gpsBadgeW = 72;
  const int gpsBadgeX = wifiBadgeX - gpsBadgeW - 8;

  const bool effectiveApMode = wifiApMode;
  const bool effectiveConnected = wifiConnected;

  uint16_t wifiColor = tft.color565(96, 108, 124);
  const char* wifiLabel = "OFF";
  if (effectiveConnected) {
    wifiColor = tft.color565(96, 224, 126);  // Green when connected to Wi-Fi.
    wifiLabel = "WIFI";
  } else if (effectiveApMode) {
    wifiColor = tft.color565(246, 214, 72);  // Yellow in AP mode.
    wifiLabel = "AP";
  }

  uint16_t gpsColor = tft.color565(224, 80, 80);
  char gpsLabel[12];
  if (gpsHasFix) {
    gpsColor = tft.color565(96, 224, 126);
    snprintf(gpsLabel, sizeof(gpsLabel), "%d SAT", gpsSatellites);
  } else {
    snprintf(gpsLabel, sizeof(gpsLabel), "NO FIX");
  }

  tft.fillRoundRect(gpsBadgeX, badgeY, gpsBadgeW, badgeH, 5, tft.color565(16, 24, 34));
  tft.drawRoundRect(gpsBadgeX, badgeY, gpsBadgeW, badgeH, 5, gpsColor);
  tft.fillCircle(gpsBadgeX + 9, badgeY + (badgeH / 2), 3, gpsColor);
  tft.setTextFont(1);
  tft.setTextColor(gpsColor, tft.color565(16, 24, 34));
  tft.drawString(gpsLabel, gpsBadgeX + 17, badgeY + 5);

  tft.fillRoundRect(wifiBadgeX, badgeY, wifiBadgeW, badgeH, 5, tft.color565(16, 24, 34));
  tft.drawRoundRect(wifiBadgeX, badgeY, wifiBadgeW, badgeH, 5, wifiColor);
  tft.fillCircle(wifiBadgeX + 9, badgeY + (badgeH / 2), 3, wifiColor);

  tft.setTextFont(1);
  tft.setTextColor(wifiColor, tft.color565(16, 24, 34));
  tft.drawString(wifiLabel, wifiBadgeX + 17, badgeY + 5);

  uint16_t batteryColor = tft.color565(165, 190, 210);
  char batteryLabel[8] = "--%";
  if (batteryPercent >= 0) {
    snprintf(batteryLabel, sizeof(batteryLabel), "%d%%", batteryPercent);
    if (batteryPercent <= 20) {
      batteryColor = tft.color565(224, 96, 96);
    } else if (batteryPercent <= 50) {
      batteryColor = tft.color565(246, 214, 72);
    } else {
      batteryColor = tft.color565(96, 224, 126);
    }
  }

  tft.fillRoundRect(batteryBadgeX, badgeY, batteryBadgeW, badgeH, 5, tft.color565(16, 24, 34));
  tft.drawRoundRect(batteryBadgeX, badgeY, batteryBadgeW, badgeH, 5, batteryColor);

  const int iconX = batteryBadgeX + 8;
  const int iconY = badgeY + 5;
  tft.drawRoundRect(iconX, iconY, 12, 8, 2, batteryColor);
  tft.fillRect(iconX + 12, iconY + 2, 2, 4, batteryColor);
  tft.fillRect(iconX + 2, iconY + 2, 8, 4, batteryColor);

  tft.setTextColor(batteryColor, tft.color565(16, 24, 34));
  tft.drawString(batteryLabel, batteryBadgeX + 24, badgeY + 5);

}

void drawScreenTag() {
  tft.setTextFont(2);
  tft.setTextColor(tft.color565(112, 165, 225), TFT_BLACK);
  tft.setTextDatum(TR_DATUM);
  if (activeScreen == UI::Screen::Conversations) {
    tft.drawString("LOG", tft.width() - 14, 14);
  } else {
    tft.drawString("MAIN", tft.width() - 14, 14);
  }
  tft.setTextDatum(TL_DATUM);

  tft.fillRoundRect(kScreenToggleButtonX, kScreenToggleButtonY, kScreenToggleButtonW,
                    kScreenToggleButtonH, 4, tft.color565(20, 44, 74));
  tft.drawRoundRect(kScreenToggleButtonX, kScreenToggleButtonY, kScreenToggleButtonW,
                    kScreenToggleButtonH, 4, tft.color565(90, 165, 234));
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(TFT_WHITE, tft.color565(20, 44, 74));
  tft.drawString(activeScreen == UI::Screen::Main ? "GO LOG" : "GO MAIN",
                 kScreenToggleButtonX + (kScreenToggleButtonW / 2),
                 kScreenToggleButtonY + (kScreenToggleButtonH / 2));
  tft.setTextDatum(TL_DATUM);
}

void drawMainScreen() {
  const int panelW = tft.width() - 16;
  const int messageX = 16;
  const int messageW = panelW - 30;
  tft.fillRoundRect(kRxPanelX, kRxPanelY, panelW, kRxPanelH, 10, tft.color565(4, 10, 20));
  tft.drawRoundRect(kRxPanelX, kRxPanelY, panelW, kRxPanelH, 10, tft.color565(45, 80, 120));

  const uint32_t now = millis();

  if (mainDetailActive) {
    drawDetailScreen(mainDetailEntry, "MESSAGE DETAIL", panelW, now);
    return;
  }

  tft.setTextFont(2);
  tft.setTextColor(tft.color565(160, 220, 255), tft.color565(4, 10, 20));
  tft.drawString("LAST RX", 16, kRxTitleY);

  tft.setTextFont(1);
  tft.setTextColor(tft.color565(120, 150, 188), tft.color565(4, 10, 20));
  tft.setTextDatum(TR_DATUM);
  tft.setTextDatum(TL_DATUM);

  if (rxCount > 0) {
    const RxEntry& entry = rxEntries[0];
    const uint32_t ageSec = (now - entry.timestampMs) / 1000;

    char meta[36];
    snprintf(meta, sizeof(meta), "%lus  %ddBm  %0.1fdB", static_cast<unsigned long>(ageSec),
             entry.rssi, static_cast<float>(entry.snrTimes10) / 10.0f);

    tft.setTextFont(1);
    tft.setTextColor(tft.color565(115, 145, 190), tft.color565(4, 10, 20));
    tft.drawString(meta, 16, 92);

    tft.setTextFont(1);
    tft.setTextColor(TFT_WHITE, tft.color565(4, 10, 20));
    const String rxLine = fitTextToWidth(entry.text, messageW);
    tft.drawString(rxLine, messageX, 105);
  } else {
    tft.setTextFont(1);
    tft.setTextColor(tft.color565(150, 170, 200), tft.color565(4, 10, 20));
    tft.drawString("No packets received yet", 16, 104);
  }

  tft.drawFastHLine(14, 120, panelW - 28, tft.color565(20, 38, 58));

  tft.setTextFont(2);
  tft.setTextColor(tft.color565(160, 220, 255), tft.color565(4, 10, 20));
  tft.drawString("LAST TX", 16, 126);

  if (hasLastTx) {
    const uint32_t ageSec = (now - lastTxTimestampMs) / 1000;
    char meta[24];
    snprintf(meta, sizeof(meta), "%lus ago", static_cast<unsigned long>(ageSec));

    tft.setTextFont(1);
    tft.setTextColor(tft.color565(115, 145, 190), tft.color565(4, 10, 20));
    tft.drawString(meta, 16, 144);

    tft.setTextFont(1);
    tft.setTextColor(TFT_WHITE, tft.color565(4, 10, 20));
    const String txLine = fitTextToWidth(lastTxText, messageW);
    tft.drawString(txLine, messageX, 157);
  } else {
    tft.setTextFont(1);
    tft.setTextColor(tft.color565(150, 170, 200), tft.color565(4, 10, 20));
    tft.drawString("No transmission sent yet", 16, 157);
  }

  const int buttonX = 16;
  const int buttonY = 173;
  const int buttonH = 22;
  const int buttonGap = 4;
  const int buttonW = ((panelW - 16) - (3 * buttonGap)) / 4;
  const int testButtonX = buttonX + buttonW + buttonGap;
  const int aprsphButtonX = testButtonX + buttonW + buttonGap;
  const int wxButtonX = aprsphButtonX + buttonW + buttonGap;

    const bool beaconPressed = isButtonFlashing(UI::TouchButton::Beacon, now);
    const bool testPressed = isButtonFlashing(UI::TouchButton::Test, now);
    const bool aprsphPressed = isButtonFlashing(UI::TouchButton::Aprsph, now);
    const bool wxPressed = isButtonFlashing(UI::TouchButton::Wx, now);

    const uint16_t beaconBg = beaconPressed ? tft.color565(170, 30, 30) : tft.color565(30, 92, 145);
    const uint16_t beaconBorder =
      beaconPressed ? tft.color565(255, 138, 138) : tft.color565(98, 184, 255);
    const uint16_t testBg = testPressed ? tft.color565(170, 30, 30) : tft.color565(98, 72, 26);
    const uint16_t testBorder =
      testPressed ? tft.color565(255, 138, 138) : tft.color565(255, 188, 90);
    const uint16_t aprsphBg = aprsphPressed ? tft.color565(170, 30, 30) : tft.color565(34, 88, 56);
    const uint16_t aprsphBorder =
      aprsphPressed ? tft.color565(255, 138, 138) : tft.color565(118, 214, 162);
    const uint16_t wxBg = wxPressed ? tft.color565(170, 30, 30) : tft.color565(78, 52, 24);
    const uint16_t wxBorder = wxPressed ? tft.color565(255, 138, 138) : tft.color565(226, 178, 112);

    tft.fillRoundRect(buttonX, buttonY, buttonW, buttonH, 7, beaconBg);
    tft.drawRoundRect(buttonX, buttonY, buttonW, buttonH, 7, beaconBorder);

    tft.fillRoundRect(testButtonX, buttonY, buttonW, buttonH, 7, testBg);
    tft.drawRoundRect(testButtonX, buttonY, buttonW, buttonH, 7, testBorder);

    tft.fillRoundRect(aprsphButtonX, buttonY, buttonW, buttonH, 7, aprsphBg);
    tft.drawRoundRect(aprsphButtonX, buttonY, buttonW, buttonH, 7, aprsphBorder);

    tft.fillRoundRect(wxButtonX, buttonY, buttonW, buttonH, 7, wxBg);
    tft.drawRoundRect(wxButtonX, buttonY, buttonW, buttonH, 7, wxBorder);

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(TFT_WHITE, beaconBg);
  tft.drawString("BEACON", buttonX + (buttonW / 2), buttonY + (buttonH / 2));
  tft.setTextColor(TFT_WHITE, testBg);
  tft.drawString("TEST", testButtonX + (buttonW / 2), buttonY + (buttonH / 2));
  tft.setTextColor(TFT_WHITE, aprsphBg);
  tft.drawString("APRSPH", aprsphButtonX + (buttonW / 2), buttonY + (buttonH / 2));
  tft.setTextColor(TFT_WHITE, wxBg);
  tft.drawString("WX", wxButtonX + (buttonW / 2), buttonY + (buttonH / 2));
  tft.setTextDatum(TL_DATUM);
}

void drawConversationsScreen() {
  const int panelW = tft.width() - 16;
  const uint32_t now = millis();
  tft.fillRoundRect(kRxPanelX, kRxPanelY, panelW, kRxPanelH, 10, tft.color565(4, 10, 20));
  tft.drawRoundRect(kRxPanelX, kRxPanelY, panelW, kRxPanelH, 10, tft.color565(45, 80, 120));

  if (logDetailActive) {
    drawDetailScreen(logDetailEntry, "LOG DETAIL", panelW, now);
    return;
  }

  tft.setTextFont(2);
  tft.setTextColor(tft.color565(160, 220, 255), tft.color565(4, 10, 20));
  tft.drawString("LOG", 16, 74);

  char countLabel[20];
  snprintf(countLabel, sizeof(countLabel), "%d entries", logCount);
  tft.setTextColor(tft.color565(115, 145, 190), tft.color565(4, 10, 20));
  tft.setTextDatum(TR_DATUM);
  tft.drawString(countLabel, kRxPanelX + panelW - 16, 74);
  tft.setTextDatum(TL_DATUM);

  int y = kLogContentY;
  for (int row = 0; row < kLogVisibleRows; ++row) {
    const int i = logScrollOffset + row;
    if (i >= logCount) {
      break;
    }

    const LogEntry& entry = logEntries[i];
    const uint32_t ageSec = (now - entry.timestampMs) / 1000;

    char meta[40];
    if (entry.hasRfMeta) {
      snprintf(meta, sizeof(meta), "%s %lus  %ddBm  %0.1fdB", entry.isTx ? "TX" : "RX",
               static_cast<unsigned long>(ageSec), entry.rssi,
               static_cast<float>(entry.snrTimes10) / 10.0f);
    } else {
      snprintf(meta, sizeof(meta), "%s %lus", entry.isTx ? "TX" : "RX",
               static_cast<unsigned long>(ageSec));
    }

    tft.setTextFont(1);
    if (entry.isTx) {
      tft.setTextColor(tft.color565(130, 205, 255), tft.color565(4, 10, 20));
    } else {
      tft.setTextColor(tft.color565(118, 230, 152), tft.color565(4, 10, 20));
    }
    tft.drawString(meta, 16, y);

    tft.setTextColor(TFT_WHITE, tft.color565(4, 10, 20));
    tft.drawString(summarizeLogLine(entry.text), 16, y + 10);

    tft.drawFastHLine(14, y + kLogRowHeight - 1, panelW - 28, tft.color565(20, 38, 58));
    y += kLogRowHeight;
  }

  if (logCount == 0) {
    tft.setTextFont(1);
    tft.setTextColor(tft.color565(150, 170, 200), tft.color565(4, 10, 20));
    tft.drawString("No traffic logged yet", 16, 112);
  }

  tft.setTextFont(1);
  tft.setTextColor(tft.color565(120, 150, 188), tft.color565(4, 10, 20));
  tft.drawString("Tap entry: details", 16, 188);
  drawRxScrollBar();
}
}  // namespace

namespace UI {

void begin() {
  tft.init();
  // Flipped landscape so the hardware button side is reversed.
  tft.setRotation(3);
  tft.setSwapBytes(true);
}

void setCallsign(const String& callsign) {
  String next = callsign;
  next.trim();
  if (next.length() == 0) {
    next = "N0CALL-7";
  }

  if (deviceCallsign != next) {
    deviceCallsign = next;
    uiLayoutDirty = true;
    uiDirty = true;
  }
}

void showSplash(const String& title, const String& subtitle) {
  drawGradientBackground();

  tft.fillRoundRect(28, 38, tft.width() - 56, 152, 16, TFT_BLACK);
  tft.drawRoundRect(28, 38, tft.width() - 56, 152, 16, tft.color565(80, 170, 255));

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(tft.color565(180, 228, 255), TFT_BLACK);
  tft.setTextFont(4);
  tft.drawString(title, tft.width() / 2, 78);

  const String versionBadge = versionBadgeText();
  int versionX = tft.width() / 2 + (tft.textWidth(title, 4) / 2) + 8;
  const int maxVersionX = tft.width() - 24 - tft.textWidth(versionBadge, 1);
  if (versionX > maxVersionX) {
    versionX = maxVersionX;
  }
  if (versionX < 24) {
    versionX = 24;
  }

  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(tft.color565(170, 205, 232), TFT_BLACK);
  tft.drawString(versionBadge, versionX, 64);

  tft.setTextDatum(MC_DATUM);

  tft.setTextFont(2);
  tft.setTextColor(tft.color565(210, 235, 255), TFT_BLACK);
  tft.drawString(subtitle, tft.width() / 2, 104);

  tft.setTextColor(tft.color565(160, 190, 220), TFT_BLACK);
  tft.drawString("Heltec V4 Expansion APRS Terminal", tft.width() / 2, 122);

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

void setWifiState(bool connected, bool apMode) {
  if (wifiConnected != connected || wifiApMode != apMode) {
    wifiConnected = connected;
    wifiApMode = apMode;
    uiDirty = true;
  }
}

void setBatteryPercent(int percent) {
  int next = percent;
  if (next < -1) {
    next = -1;
  }
  if (next > 100) {
    next = 100;
  }

  if (batteryPercent != next) {
    batteryPercent = next;
    uiDirty = true;
  }
}

void setGpsState(bool hasFix, int satellites, float speedKmh) {
  const bool changed = (gpsHasFix != hasFix) || (gpsSatellites != satellites);
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

  rxEntries[0].text = normalizePacketText(packet);
  rxEntries[0].rssi = static_cast<int>(rssi);
  rxEntries[0].snrTimes10 = static_cast<int>(snr * 10.0f);
  rxEntries[0].timestampMs = millis();
  pushLogEntry(false, packet, static_cast<int>(rssi), static_cast<int>(snr * 10.0f), true);

  statusText = "packet received";
  uiDirty = true;
}

void addConversation(const String& peer, const String& preview) {
  if (conversationCount < kMaxConversations) {
    ++conversationCount;
  }

  for (int i = conversationCount - 1; i > 0; --i) {
    conversationEntries[i] = conversationEntries[i - 1];
  }

  conversationEntries[0].peer = peer;
  conversationEntries[0].preview = summarizeConversationPreview(preview);
  conversationEntries[0].unreadCount = 0;
  uiDirty = true;
}

void setConversationComposer(bool active,
                            bool enteringMessage,
                            const String& callsign,
                            const String& message) {
  const bool changed = (composerActive != active) || (composerEnteringMessage != enteringMessage) ||
                       (composerCallsign != callsign) || (composerMessage != message);
  if (!changed) {
    return;
  }

  composerActive = active;
  composerEnteringMessage = enteringMessage;
  composerCallsign = callsign;
  composerMessage = summarizeConversationPreview(message);
  uiDirty = true;
}

void scrollLogNewer() { setRxScrollOffset(logScrollOffset - 1); }

void scrollLogOlder() { setRxScrollOffset(logScrollOffset + 1); }

void scrollLogPageNewer() { setRxScrollOffset(logScrollOffset - kLogVisibleRows); }

void scrollLogPageOlder() { setRxScrollOffset(logScrollOffset + kLogVisibleRows); }

void resetLogScroll() { setRxScrollOffset(0); }

bool openMainDetailAt(int16_t x, int16_t y) {
  if (activeScreen != Screen::Main || mainDetailActive) {
    return false;
  }

  const int panelW = tft.width() - 16;
  const int messageX = 16;
  const int messageW = panelW - 30;

  const bool inRxLine = (x >= messageX && x < (messageX + messageW) && y >= 100 && y < 116);
  const bool inTxLine = (x >= messageX && x < (messageX + messageW) && y >= 152 && y < 168);

  if (inRxLine && rxCount > 0) {
    const RxEntry& entry = rxEntries[0];
    mainDetailEntry.text = entry.text;
    mainDetailEntry.isTx = false;
    mainDetailEntry.hasRfMeta = true;
    mainDetailEntry.rssi = entry.rssi;
    mainDetailEntry.snrTimes10 = entry.snrTimes10;
    mainDetailEntry.timestampMs = entry.timestampMs;
    mainDetailActive = true;
    uiDirty = true;
    return true;
  }

  if (inTxLine && hasLastTx) {
    mainDetailEntry.text = lastTxText;
    mainDetailEntry.isTx = true;
    mainDetailEntry.hasRfMeta = false;
    mainDetailEntry.rssi = 0;
    mainDetailEntry.snrTimes10 = 0;
    mainDetailEntry.timestampMs = lastTxTimestampMs;
    mainDetailActive = true;
    uiDirty = true;
    return true;
  }

  return false;
}

bool handleMainDetailTouch(int16_t x, int16_t y) {
  if (!mainDetailActive) {
    return false;
  }

  if (!isLogDetailBackTouch(x, y)) {
    return false;
  }

  mainDetailActive = false;
  uiDirty = true;
  return true;
}

bool isMainDetailActive() { return mainDetailActive; }

bool openLogDetailAt(int16_t x, int16_t y) {
  if (activeScreen != Screen::Conversations || logDetailActive) {
    return false;
  }

  if (x < 14 || x >= (logScrollTrackX() - 2)) {
    return false;
  }

  const int rowsHeight = kLogVisibleRows * kLogRowHeight;
  if (y < kLogContentY || y >= (kLogContentY + rowsHeight)) {
    return false;
  }

  const int row = (y - kLogContentY) / kLogRowHeight;
  const int idx = logScrollOffset + row;
  if (idx < 0 || idx >= logCount) {
    return false;
  }

  logDetailEntry = logEntries[idx];
  logDetailActive = true;
  uiDirty = true;
  return true;
}

bool handleLogDetailTouch(int16_t x, int16_t y) {
  if (!logDetailActive) {
    return false;
  }

  if (!isLogDetailBackTouch(x, y)) {
    return false;
  }

  logDetailActive = false;
  uiDirty = true;
  return true;
}

bool isLogDetailActive() { return logDetailActive; }

bool handleLogScrollButtonTouch(int16_t x, int16_t y) {
  if (activeScreen != Screen::Conversations || logDetailActive) {
    return false;
  }

  if (isLogScrollUpButtonTouch(x, y)) {
    scrollLogNewer();
    return true;
  }

  if (isLogScrollDownButtonTouch(x, y)) {
    scrollLogOlder();
    return true;
  }

  return false;
}

void showScreen(Screen screen) {
  if (activeScreen != screen) {
    logDetailActive = false;
    mainDetailActive = false;
    activeScreen = screen;
    uiLayoutDirty = true;
    uiDirty = true;
  }
}

void nextScreen() {
  if (activeScreen == Screen::Main) {
    showScreen(Screen::Conversations);
  } else {
    showScreen(Screen::Main);
  }
}

void previousScreen() { nextScreen(); }

Screen currentScreen() { return activeScreen; }

void flashButton(TouchButton button) {
  flashedButton = button;
  flashButtonUntilMs = millis() + kTouchFlashDurationMs;
  uiDirty = true;
}

void noteTxPacket(const String& packet) {
  lastTxText = normalizePacketText(packet);
  lastTxTimestampMs = millis();
  hasLastTx = true;
  pushLogEntry(true, packet, 0, 0, false);
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

  if (flashedButton != TouchButton::None && static_cast<int32_t>(flashButtonUntilMs - now) <= 0) {
    flashedButton = TouchButton::None;
    uiDirty = true;
  }

  if (!force && !uiDirty) {
    return;
  }

  if (!force && (now - lastRenderMs) < 80) {
    return;
  }

  static uint32_t lastRenderLogMs = 0;
  if (force || (now - lastRenderLogMs >= 5000)) {
    Serial.printf("[UI-RENDER] force=%d dirty=%d screen=%d\n", force ? 1 : 0,
                  uiDirty ? 1 : 0, static_cast<int>(activeScreen));
    lastRenderLogMs = now;
  }

  uiDirty = false;
  lastRenderMs = now;

  const bool fullRepaint = uiLayoutDirty;
  if (fullRepaint) {
    drawGradientBackground();
    drawHeader();
    uiLayoutDirty = false;
  }

  drawScreenTag();
  if (activeScreen == Screen::Conversations) {
    drawConversationsScreen();
  } else {
    drawMainScreen();
  }
  drawStatusBar();
}

}  // namespace UI
