#pragma once

#include <Arduino.h>

namespace UI {
// UI screens shown on the TFT.
enum class Screen : uint8_t {
	Main = 0,
	Conversations = 1,  // Message list/detail screen.
};

// Touchable controls that support flash feedback.
enum class TouchButton : uint8_t {
	None = 0,
	ScreenToggle,
	Beacon,
	Test,
	Aprsph,
	Thurs,
	Wx,
	LogScrollUp,
	LogScrollDown,
};

// Initializes display state and rendering backend.
void begin();

// Shows a blocking splash screen used during startup.
void showSplash(const String& title, const String& subtitle);

// Device/runtime state setters that trigger re-rendering as needed.
void setCallsign(const String& callsign);
void setRadioState(bool isReady);
void setWifiState(bool connected, bool apMode);
void setBatteryPercent(int percent);
void setGpsState(bool hasFix, int satellites, float speedKmh);

// Packet/status update hooks.
void noteRxPacket(const String& packet, float rssi, float snr);

// Message log navigation helpers.
void scrollLogNewer();
void scrollLogOlder();
void scrollLogPageNewer();
void scrollLogPageOlder();
void resetLogScroll();

// Detail panel interaction helpers.
bool openMainDetailAt(int16_t x, int16_t y);
bool handleMainDetailTouch(int16_t x, int16_t y);
bool isMainDetailActive();
bool openLogDetailAt(int16_t x, int16_t y);
bool handleLogDetailTouch(int16_t x, int16_t y);
bool isLogDetailActive();
bool handleLogScrollButtonTouch(int16_t x, int16_t y);

// Screen navigation and rendering control.
void showScreen(Screen screen);
void nextScreen();
void previousScreen();
Screen currentScreen();
void setMainActionPressed(TouchButton button);
void flashButton(TouchButton button, uint32_t durationMs = 220);
void noteTxPacket(const String& packet);
void noteStatus(const String& status);
void render(bool force = false);
}  // namespace UI
