#pragma once

#include "runtime_config.h"

typedef void (*WebConfigSaveCb)();

// Starts the HTTP configuration service in STA mode when possible,
// otherwise AP fallback mode.
bool webConfigBegin(RuntimeConfig* cfg, WebConfigSaveCb onSave);

// Stops web server and tears down Wi-Fi state created by webConfigBegin.
void webConfigEnd();

// Pumps web server state; call from main loop.
void webConfigLoop();

// Runtime status helpers.
bool webConfigRunning();
bool webConfigIsApMode();
const char* webConfigIP();
const char* webConfigSsid();

// Feeds a received APRS packet into the web nodes/live-feed model.
void webConfigNoteHeardPacket(const String& aprsPacket, float rssi, float snr);

// Updates this device position for the Nodes tab "ME" recenter action.
void webConfigSetSelfPosition(bool hasPosition, double latitude, double longitude);
