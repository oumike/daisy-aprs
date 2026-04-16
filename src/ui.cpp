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
constexpr int kRxContentY = 92;
constexpr int kRxContentBottomY = 198;
constexpr int kRxRowHeight = 30;
constexpr int kRxMetaOffsetY = 0;
constexpr int kRxTextOffsetY = 11;
constexpr int kRxVisibleRows = (kRxContentBottomY - kRxContentY) / kRxRowHeight;

RxEntry rxEntries[kMaxRxEntries];
int rxCount = 0;
int rxScrollOffset = 0;

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

String statusText = "booting";
uint32_t lastRenderMs = 0;
bool uiDirty = true;

String summarizePacket(const String& packet) {
  String s = packet;
  s.trim();

  if (s.length() > 33) {
    s = s.substring(0, 33) + "...";
  }

  return s;
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
  if (rxCount <= kRxVisibleRows) {
    return 0;
  }
  return rxCount - kRxVisibleRows;
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

  if (next != rxScrollOffset) {
    rxScrollOffset = next;
    uiDirty = true;
  }
}

void drawRxScrollBar() {
  if (rxCount <= kRxVisibleRows) {
    return;
  }

  const int panelW = tft.width() - 16;
  const int trackX = kRxPanelX + panelW - 10;
  const int trackY = kRxContentY;
  const int trackH = kRxContentBottomY - kRxContentY;
  const int maxOffset = maxRxScrollOffset();

  tft.fillRoundRect(trackX, trackY, 4, trackH, 2, tft.color565(22, 42, 65));

  int thumbH = (trackH * kRxVisibleRows) / rxCount;
  if (thumbH < 10) {
    thumbH = 10;
  }

  int thumbY = trackY;
  if (maxOffset > 0) {
    thumbY = trackY + ((trackH - thumbH) * rxScrollOffset) / maxOffset;
  }

  tft.fillRoundRect(trackX, thumbY, 4, thumbH, 2, tft.color565(100, 195, 255));
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

  char line[32];
  snprintf(line, sizeof(line), "RADIO:%s", radioReady ? "OK" : "WAIT");
  tft.drawString(line, 18, 40);
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

  Serial.printf(
      "[UI-WIFI-DRAW] label=%s uiConnected=%d uiAp=%d\n", wifiLabel,
      wifiConnected ? 1 : 0, wifiApMode ? 1 : 0);

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
    tft.drawString("CONVOS", tft.width() - 14, 14);
  } else {
    tft.drawString("MAIN", tft.width() - 14, 14);
  }
  tft.setTextDatum(TL_DATUM);
}

void drawMainScreen() {
  const int panelW = tft.width() - 16;
  tft.fillRoundRect(kRxPanelX, kRxPanelY, panelW, kRxPanelH, 10, tft.color565(4, 10, 20));
  tft.drawRoundRect(kRxPanelX, kRxPanelY, panelW, kRxPanelH, 10, tft.color565(45, 80, 120));

  tft.setTextFont(2);
  tft.setTextColor(tft.color565(160, 220, 255), tft.color565(4, 10, 20));
  tft.drawString(deviceCallsign, 16, kRxTitleY);

  if (rxCount > 0) {
    const int viewStart = rxScrollOffset + 1;
    const int viewEnd = (rxScrollOffset + kRxVisibleRows < rxCount) ? (rxScrollOffset + kRxVisibleRows)
                                                                     : rxCount;
    char view[20];
    snprintf(view, sizeof(view), "%d-%d/%d", viewStart, viewEnd, rxCount);
    tft.setTextColor(tft.color565(115, 145, 190), tft.color565(4, 10, 20));
    tft.setTextDatum(TR_DATUM);
    tft.drawString(view, kRxPanelX + panelW - 16, kRxTitleY);
    tft.setTextDatum(TL_DATUM);
  }

  const uint32_t now = millis();
  int y = kRxContentY;
  for (int row = 0; row < kRxVisibleRows; ++row) {
    const int i = rxScrollOffset + row;
    if (i >= rxCount) {
      break;
    }

    const RxEntry& entry = rxEntries[i];
    const uint32_t ageSec = (now - entry.timestampMs) / 1000;

    char meta[32];
    snprintf(meta, sizeof(meta), "%lus  %ddBm  %0.1fdB", static_cast<unsigned long>(ageSec),
             entry.rssi, static_cast<float>(entry.snrTimes10) / 10.0f);

    tft.setTextFont(1);
    tft.setTextColor(tft.color565(115, 145, 190), tft.color565(4, 10, 20));
    tft.drawString(meta, 16, y + kRxMetaOffsetY);

    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, tft.color565(4, 10, 20));
    tft.drawString(entry.text, 16, y + kRxTextOffsetY);

    tft.drawFastHLine(14, y + kRxRowHeight - 3, panelW - 28, tft.color565(20, 38, 58));

    y += kRxRowHeight;
  }

  if (rxCount == 0) {
    tft.setTextColor(tft.color565(150, 170, 200), tft.color565(4, 10, 20));
    tft.drawString("No packets received yet", 16, 116);
    tft.drawString("Listening on LoRa APRS...", 16, 132);
  }

  tft.setTextFont(1);
  tft.setTextColor(tft.color565(120, 150, 188), tft.color565(4, 10, 20));
  tft.drawString("Touch top-left: beacon, top-right: conversations", 16, kRxContentBottomY - 10);

  drawRxScrollBar();
}

void drawConversationsScreen() {
  const int panelW = tft.width() - 16;
  tft.fillRoundRect(kRxPanelX, kRxPanelY, panelW, kRxPanelH, 10, tft.color565(4, 10, 20));
  tft.drawRoundRect(kRxPanelX, kRxPanelY, panelW, kRxPanelH, 10, tft.color565(45, 80, 120));

  tft.setTextFont(2);
  tft.setTextColor(tft.color565(160, 220, 255), tft.color565(4, 10, 20));
  tft.drawString(deviceCallsign, 16, 74);

  char countLabel[20];
  snprintf(countLabel, sizeof(countLabel), "%d total", conversationCount);
  tft.setTextColor(tft.color565(115, 145, 190), tft.color565(4, 10, 20));
  tft.setTextDatum(TR_DATUM);
  tft.drawString(countLabel, kRxPanelX + panelW - 16, 74);
  tft.setTextDatum(TL_DATUM);

  const int composerBoxY = 154;
  const int contentBottomLimit = composerActive ? (composerBoxY - 4) : (kRxContentBottomY - 16);

  int y = kRxContentY;
  for (int i = 0; i < conversationCount && y < contentBottomLimit; ++i) {
    const ConversationEntry& entry = conversationEntries[i];

    tft.setTextFont(2);
    tft.setTextColor(tft.color565(201, 225, 245), tft.color565(4, 10, 20));
    tft.drawString(entry.peer, 16, y);

    if (entry.unreadCount > 0) {
      char badge[8];
      snprintf(badge, sizeof(badge), "%u", static_cast<unsigned int>(entry.unreadCount));
      tft.fillRoundRect(kRxPanelX + panelW - 34, y, 22, 14, 7, tft.color565(56, 108, 165));
      tft.setTextFont(1);
      tft.setTextColor(TFT_WHITE, tft.color565(56, 108, 165));
      tft.drawCentreString(badge, kRxPanelX + panelW - 23, y + 3, 1);
    }

    tft.setTextFont(1);
    tft.setTextColor(tft.color565(140, 170, 204), tft.color565(4, 10, 20));
    tft.drawString(entry.preview, 16, y + 14);

    tft.drawFastHLine(14, y + 25, panelW - 28, tft.color565(20, 38, 58));
    y += 26;
  }

  if (conversationCount == 0) {
    tft.setTextFont(2);
    tft.setTextColor(tft.color565(150, 170, 200), tft.color565(4, 10, 20));
    tft.drawString("No conversations yet", 16, 112);
    tft.setTextFont(1);
    tft.setTextColor(tft.color565(120, 150, 188), tft.color565(4, 10, 20));
    tft.drawString("Send APRS message via serial tx command", 16, 130);
  }

  if (composerActive) {
    tft.fillRoundRect(14, composerBoxY, panelW - 20, 40, 8, tft.color565(10, 22, 38));
    tft.drawRoundRect(14, composerBoxY, panelW - 20, 40, 8, tft.color565(70, 122, 176));

    tft.setTextFont(1);
    tft.setTextColor(composerEnteringMessage ? tft.color565(125, 150, 185) : tft.color565(255, 210, 140),
                     tft.color565(10, 22, 38));
    tft.drawString("TO:", 20, composerBoxY + 6);
    tft.setTextColor(TFT_WHITE, tft.color565(10, 22, 38));
    tft.drawString(composerCallsign.length() > 0 ? composerCallsign : String("_"), 46, composerBoxY + 6);

    tft.setTextColor(composerEnteringMessage ? tft.color565(255, 210, 140)
                                             : tft.color565(125, 150, 185),
                     tft.color565(10, 22, 38));
    tft.drawString("MSG:", 20, composerBoxY + 22);
    tft.setTextColor(TFT_WHITE, tft.color565(10, 22, 38));
    tft.drawString(composerMessage.length() > 0 ? composerMessage : String("_"), 50, composerBoxY + 22);

    tft.setTextColor(tft.color565(120, 150, 188), tft.color565(4, 10, 20));
    tft.drawString("Enter: next/send  Del: erase", 16, kRxContentBottomY - 10);
  } else {
    tft.setTextFont(1);
    tft.setTextColor(tft.color565(120, 150, 188), tft.color565(4, 10, 20));
    tft.drawString("Tap screen to return to Main", 16, kRxContentBottomY - 10);
  }
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
    Serial.printf("[UI-WIFI] setWifiState %d/%d -> %d/%d\n", wifiConnected ? 1 : 0,
                  wifiApMode ? 1 : 0, connected ? 1 : 0, apMode ? 1 : 0);
    wifiConnected = connected;
    wifiApMode = apMode;
    uiDirty = true;
  } else {
    static uint32_t lastNoChangeLogMs = 0;
    const uint32_t now = millis();
    if (now - lastNoChangeLogMs >= 10000) {
      Serial.printf("[UI-WIFI] setWifiState unchanged=%d/%d\n", connected ? 1 : 0,
                    apMode ? 1 : 0);
      lastNoChangeLogMs = now;
    }
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
  rxScrollOffset = 0;

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

void scrollRxNewer() { setRxScrollOffset(rxScrollOffset - 1); }

void scrollRxOlder() { setRxScrollOffset(rxScrollOffset + 1); }

void scrollRxPageNewer() { setRxScrollOffset(rxScrollOffset - kRxVisibleRows); }

void scrollRxPageOlder() { setRxScrollOffset(rxScrollOffset + kRxVisibleRows); }

void resetRxScroll() { setRxScrollOffset(0); }

void showScreen(Screen screen) {
  if (activeScreen != screen) {
    activeScreen = screen;
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

  drawGradientBackground();
  drawHeader();
  drawScreenTag();
  if (activeScreen == Screen::Conversations) {
    drawConversationsScreen();
  } else {
    drawMainScreen();
  }
  drawStatusBar();
}

}  // namespace UI
