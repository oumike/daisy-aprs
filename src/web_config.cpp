#include "web_config.h"

#include <WebServer.h>
#include <WiFi.h>

#include <ctype.h>
#include <math.h>
#include <string.h>

#include "app_config.h"
#include "runtime_config.h"

namespace {
constexpr uint32_t kConnectTimeoutMs = 10000;
constexpr char kApSsid[] = "DAISY-APRS-AP";
constexpr char kApPassword[] = "daisyaprs";
static_assert((sizeof(kApPassword) - 1) >= 8,
              "AP password must be at least 8 characters for WPA2");
static_assert((sizeof(kApPassword) - 1) <= 63,
              "AP password must be 63 characters or fewer");
constexpr uint32_t kStaDropToApMs = 5000;

WebServer server(80);
RuntimeConfig* gCfg = nullptr;
WebConfigSaveCb gOnSave = nullptr;
bool gRunning = false;
bool gApMode = false;
uint32_t gStaLostSinceMs = 0;
char gIpBuf[24] = "";
char gApSsid[32] = "";
String gImportYaml;
bool gImportTooLarge = false;

constexpr size_t kMaxImportYamlBytes = 16384;

constexpr int kMaxHeardNodes = 64;
constexpr int kMaxHeardEvents = 80;
constexpr uint32_t kStaleNodeMs = 12UL * 60UL * 60UL * 1000UL;

struct HeardNode {
  char callsign[16];
  bool hasPosition;
  float latitude;
  float longitude;
  float lastRssi;
  float lastSnr;
  uint32_t heardCount;
  uint32_t lastHeardMs;
  char lastPacket[112];
};

struct HeardEvent {
  char callsign[16];
  bool hasPosition;
  float latitude;
  float longitude;
  float rssi;
  float snr;
  uint32_t seenAtMs;
  char summary[96];
};

HeardNode gHeardNodes[kMaxHeardNodes];
int gHeardNodeCount = 0;

HeardEvent gHeardEvents[kMaxHeardEvents];
int gHeardEventCount = 0;
int gHeardEventNext = 0;

bool gSelfHasPosition = false;
float gSelfLatitude = 0.0f;
float gSelfLongitude = 0.0f;

String escapeHtml(const String& in) {
  String out;
  out.reserve(in.length() + 12);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '&') {
      out += "&amp;";
    } else if (c == '<') {
      out += "&lt;";
    } else if (c == '>') {
      out += "&gt;";
    } else if (c == '\"') {
      out += "&quot;";
    } else {
      out += c;
    }
  }
  return out;
}

void copyToArray(char* dst, size_t dstLen, const String& src) {
  if (dstLen == 0) {
    return;
  }
  const size_t n = (src.length() < (dstLen - 1)) ? src.length() : (dstLen - 1);
  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
}

String escapeJson(const String& in) {
  String out;
  out.reserve(in.length() + 12);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out += c;
    }
  }
  return out;
}

String summarizePacketForFeed(const String& packet) {
  String out = packet;
  out.replace("\r", " ");
  out.replace("\n", " ");
  out.trim();

  while (out.indexOf("  ") >= 0) {
    out.replace("  ", " ");
  }

  if (out.length() > 92) {
    out = out.substring(0, 92) + "...";
  }
  return out;
}

String sanitizeCallsignToken(const String& in) {
  String out;
  out.reserve(15);
  for (size_t i = 0; i < in.length(); ++i) {
    const char raw = in[i];
    const char c = static_cast<char>(toupper(static_cast<unsigned char>(raw)));
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') {
      out += c;
    }
  }
  if (out.length() > 15) {
    out = out.substring(0, 15);
  }
  return out;
}

String extractAprsSourceCallsign(const String& aprsPacket) {
  const int sep = aprsPacket.indexOf('>');
  if (sep <= 0) {
    return "";
  }
  String source = aprsPacket.substring(0, sep);
  source.trim();
  return sanitizeCallsignToken(source);
}

bool parseAprsCoord(const String& encoded,
                    int degreeDigits,
                    char positiveHemisphere,
                    char negativeHemisphere,
                    float* outValue) {
  if (!outValue || encoded.length() != static_cast<unsigned int>(degreeDigits + 6)) {
    return false;
  }

  const char hemisphere =
      static_cast<char>(toupper(static_cast<unsigned char>(encoded[encoded.length() - 1])));
  if (hemisphere != positiveHemisphere && hemisphere != negativeHemisphere) {
    return false;
  }

  String degToken = encoded.substring(0, degreeDigits);
  String minToken = encoded.substring(degreeDigits, degreeDigits + 5);
  if (minToken.length() != 5 || minToken[2] != '.') {
    return false;
  }

  for (size_t i = 0; i < degToken.length(); ++i) {
    if (!isdigit(static_cast<unsigned char>(degToken[i]))) {
      return false;
    }
  }

  for (int i = 0; i < 5; ++i) {
    if (i == 2) {
      continue;
    }
    if (!isdigit(static_cast<unsigned char>(minToken[i]))) {
      return false;
    }
  }

  const float degrees = degToken.toFloat();
  const float minutes = minToken.toFloat();
  if (minutes < 0.0f || minutes >= 60.0f) {
    return false;
  }

  float value = degrees + (minutes / 60.0f);
  if (hemisphere == negativeHemisphere) {
    value = -value;
  }
  *outValue = value;
  return true;
}

bool parseAprsPositionFromPayload(const String& payload, float* outLat, float* outLon) {
  if (!outLat || !outLon || payload.length() < 19) {
    return false;
  }

  int start = -1;
  const char packetType = payload[0];
  if (packetType == '!' || packetType == '=') {
    start = 1;
  } else if ((packetType == '/' || packetType == '@') && payload.length() >= 27) {
    start = 8;
  }

  if (start < 0 || payload.length() < (start + 19)) {
    return false;
  }

  const String latToken = payload.substring(start, start + 8);
  const String lonToken = payload.substring(start + 9, start + 18);

  float lat = 0.0f;
  float lon = 0.0f;
  if (!parseAprsCoord(latToken, 2, 'N', 'S', &lat)) {
    return false;
  }
  if (!parseAprsCoord(lonToken, 3, 'E', 'W', &lon)) {
    return false;
  }

  if (fabs(lat) > 90.0f || fabs(lon) > 180.0f) {
    return false;
  }

  *outLat = lat;
  *outLon = lon;
  return true;
}

bool parseAprsPositionFromPacket(const String& aprsPacket, float* outLat, float* outLon) {
  const int payloadSep = aprsPacket.indexOf(':');
  if (payloadSep < 0 || payloadSep >= (aprsPacket.length() - 1)) {
    return false;
  }

  const String payload = aprsPacket.substring(payloadSep + 1);
  return parseAprsPositionFromPayload(payload, outLat, outLon);
}

int findHeardNodeIndex(const char* callsign) {
  if (!callsign || callsign[0] == '\0') {
    return -1;
  }

  for (int i = 0; i < gHeardNodeCount; ++i) {
    if (strcmp(gHeardNodes[i].callsign, callsign) == 0) {
      return i;
    }
  }
  return -1;
}

int findOldestHeardNodeIndex() {
  if (gHeardNodeCount <= 0) {
    return -1;
  }

  int oldestIdx = 0;
  uint32_t oldestSeenMs = gHeardNodes[0].lastHeardMs;
  for (int i = 1; i < gHeardNodeCount; ++i) {
    if (gHeardNodes[i].lastHeardMs < oldestSeenMs) {
      oldestSeenMs = gHeardNodes[i].lastHeardMs;
      oldestIdx = i;
    }
  }
  return oldestIdx;
}

void pruneStaleHeardNodes(uint32_t nowMs) {
  int writeIdx = 0;
  for (int readIdx = 0; readIdx < gHeardNodeCount; ++readIdx) {
    const HeardNode& node = gHeardNodes[readIdx];
    if ((nowMs - node.lastHeardMs) > kStaleNodeMs) {
      continue;
    }
    if (writeIdx != readIdx) {
      gHeardNodes[writeIdx] = node;
    }
    ++writeIdx;
  }
  gHeardNodeCount = writeIdx;
}

void appendHeardEvent(const String& callsign,
                      const String& packet,
                      float rssi,
                      float snr,
                      bool hasPosition,
                      float lat,
                      float lon) {
  HeardEvent& event = gHeardEvents[gHeardEventNext];
  memset(&event, 0, sizeof(event));
  copyToArray(event.callsign, sizeof(event.callsign), callsign);
  copyToArray(event.summary, sizeof(event.summary), summarizePacketForFeed(packet));
  event.rssi = rssi;
  event.snr = snr;
  event.hasPosition = hasPosition;
  event.latitude = lat;
  event.longitude = lon;
  event.seenAtMs = millis();

  gHeardEventNext = (gHeardEventNext + 1) % kMaxHeardEvents;
  if (gHeardEventCount < kMaxHeardEvents) {
    ++gHeardEventCount;
  }
}

void upsertHeardNode(const String& callsign,
                    const String& packet,
                    float rssi,
                    float snr,
                    bool hasPosition,
                    float lat,
                    float lon) {
  if (callsign.length() == 0) {
    return;
  }

  const uint32_t nowMs = millis();
  pruneStaleHeardNodes(nowMs);

  int idx = findHeardNodeIndex(callsign.c_str());
  if (idx < 0) {
    if (gHeardNodeCount < kMaxHeardNodes) {
      idx = gHeardNodeCount;
      ++gHeardNodeCount;
    } else {
      idx = findOldestHeardNodeIndex();
    }

    if (idx < 0) {
      return;
    }

    memset(&gHeardNodes[idx], 0, sizeof(gHeardNodes[idx]));
    copyToArray(gHeardNodes[idx].callsign, sizeof(gHeardNodes[idx].callsign), callsign);
  }

  HeardNode& node = gHeardNodes[idx];
  node.lastHeardMs = nowMs;
  node.lastRssi = rssi;
  node.lastSnr = snr;
  node.heardCount += 1;
  copyToArray(node.lastPacket, sizeof(node.lastPacket), summarizePacketForFeed(packet));

  if (hasPosition) {
    node.hasPosition = true;
    node.latitude = lat;
    node.longitude = lon;
  }

  appendHeardEvent(callsign, packet, rssi, snr, hasPosition, lat, lon);
}

void sortNodeIndexesByRecency(int* indexes, int count) {
  for (int i = 1; i < count; ++i) {
    const int candidate = indexes[i];
    int j = i - 1;
    while (j >= 0 && gHeardNodes[indexes[j]].lastHeardMs < gHeardNodes[candidate].lastHeardMs) {
      indexes[j + 1] = indexes[j];
      --j;
    }
    indexes[j + 1] = candidate;
  }
}

String buildNodesJson() {
  const uint32_t nowMs = millis();
  pruneStaleHeardNodes(nowMs);

  String json;
  json.reserve(18000);
  json += "{";
  json += "\"self\":{";
  json += "\"callsign\":\"";
  json += escapeJson(String(gCfg ? gCfg->callsign : ""));
  json += "\",";
  json += "\"has_position\":";
  json += gSelfHasPosition ? "true" : "false";
  json += ",\"lat\":";
  json += String(gSelfLatitude, 6);
  json += ",\"lon\":";
  json += String(gSelfLongitude, 6);
  json += "},\"nodes\":[";

  int sortedIndexes[kMaxHeardNodes];
  for (int i = 0; i < gHeardNodeCount; ++i) {
    sortedIndexes[i] = i;
  }
  sortNodeIndexesByRecency(sortedIndexes, gHeardNodeCount);

  for (int i = 0; i < gHeardNodeCount; ++i) {
    const HeardNode& node = gHeardNodes[sortedIndexes[i]];
    if (i > 0) {
      json += ',';
    }

    json += "{\"callsign\":\"";
    json += escapeJson(String(node.callsign));
    json += "\",\"heard_count\":";
    json += String(node.heardCount);
    json += ",\"age_s\":";
    json += String((nowMs - node.lastHeardMs) / 1000UL);
    json += ",\"rssi\":";
    json += String(node.lastRssi, 1);
    json += ",\"snr\":";
    json += String(node.lastSnr, 1);
    json += ",\"has_position\":";
    json += node.hasPosition ? "true" : "false";
    json += ",\"lat\":";
    json += String(node.latitude, 6);
    json += ",\"lon\":";
    json += String(node.longitude, 6);
    json += ",\"last_packet\":\"";
    json += escapeJson(String(node.lastPacket));
    json += "\"}";
  }

  json += "],\"feed\":[";

  for (int i = 0; i < gHeardEventCount; ++i) {
    const int idx = (gHeardEventNext - 1 - i + kMaxHeardEvents) % kMaxHeardEvents;
    const HeardEvent& event = gHeardEvents[idx];
    if (i > 0) {
      json += ',';
    }

    json += "{\"callsign\":\"";
    json += escapeJson(String(event.callsign));
    json += "\",\"age_s\":";
    json += String((nowMs - event.seenAtMs) / 1000UL);
    json += ",\"rssi\":";
    json += String(event.rssi, 1);
    json += ",\"snr\":";
    json += String(event.snr, 1);
    json += ",\"has_position\":";
    json += event.hasPosition ? "true" : "false";
    json += ",\"lat\":";
    json += String(event.latitude, 6);
    json += ",\"lon\":";
    json += String(event.longitude, 6);
    json += ",\"summary\":\"";
    json += escapeJson(String(event.summary));
    json += "\"}";
  }

  json += "]}";
  return json;
}

String argOr(const char* key, const String& fallback) {
  if (server.hasArg(key)) {
    return server.arg(key);
  }
  return fallback;
}

int argToInt(const char* key, int fallback) {
  if (!server.hasArg(key)) {
    return fallback;
  }
  return server.arg(key).toInt();
}

uint32_t argToULong(const char* key, uint32_t fallback) {
  if (!server.hasArg(key)) {
    return fallback;
  }
  return static_cast<uint32_t>(strtoul(server.arg(key).c_str(), nullptr, 10));
}

float argToFloat(const char* key, float fallback) {
  if (!server.hasArg(key)) {
    return fallback;
  }
  return server.arg(key).toFloat();
}

String normalizeCallsignInput(const String& in) {
  String out = in;
  out.trim();
  out.toUpperCase();
  return out;
}

String normalizeRouteInput(const String& in) {
  String out = in;
  out.trim();
  out.toUpperCase();
  return out;
}

String trimQuotes(const String& in) {
  String out = in;
  out.trim();
  bool quoted = false;
  if (out.length() >= 2) {
    const char first = out[0];
    const char last = out[out.length() - 1];
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      out = out.substring(1, out.length() - 1);
      quoted = true;
    }
  }

  if (quoted) {
    String unescaped;
    unescaped.reserve(out.length());
    bool esc = false;
    for (size_t i = 0; i < out.length(); ++i) {
      const char c = out[i];
      if (esc) {
        if (c == '"' || c == '\\') {
          unescaped += c;
        } else if (c == 'n') {
          unescaped += '\n';
        } else {
          unescaped += c;
        }
        esc = false;
      } else if (c == '\\') {
        esc = true;
      } else {
        unescaped += c;
      }
    }
    if (esc) {
      unescaped += '\\';
    }
    out = unescaped;
  }

  return out;
}

String yamlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '"') {
      out += "\\\"";
    } else {
      out += c;
    }
  }
  return out;
}

String yamlQuoted(const String& in) {
  return String("\"") + yamlEscape(in) + "\"";
}

bool parseBoolValue(const String& in, bool fallback) {
  String v = in;
  v.trim();
  v.toLowerCase();
  if (v == "1" || v == "true" || v == "yes" || v == "on") {
    return true;
  }
  if (v == "0" || v == "false" || v == "no" || v == "off") {
    return false;
  }
  return fallback;
}

String exportFilenameFromCallsign(const char* callsign) {
  String base = callsign ? String(callsign) : String("daisy");
  base.trim();
  if (base.length() == 0) {
    base = "daisy";
  }

  String safe;
  safe.reserve(base.length());
  for (size_t i = 0; i < base.length(); ++i) {
    const char c = base[i];
    if (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
      safe += c;
    } else {
      safe += '_';
    }
  }

  if (safe.length() == 0) {
    safe = "daisy";
  }

  return safe + "_config.yml";
}

String buildConfigYaml(const RuntimeConfig& cfg) {
  String y;
  y.reserve(1200);
  y += "# Daisy APRS runtime config export\n";
  y += "callsign: ";
  y += yamlQuoted(String(cfg.callsign));
  y += "\n";
  y += "destination: ";
  y += yamlQuoted(String(cfg.destination));
  y += "\n";
  y += "path: ";
  y += yamlQuoted(String(cfg.path));
  y += "\n";
  y += "comment: ";
  y += yamlQuoted(String(cfg.comment));
  y += "\n";
  y += "aprsph_message: ";
  y += yamlQuoted(String(cfg.aprsphMessage));
  y += "\n";
  y += "hotg_message: ";
  y += yamlQuoted(String(cfg.hotgMessage));
  y += "\n";
  y += "symbol_table: ";
  y += yamlQuoted(String(cfg.symbolTable));
  y += "\n";
  y += "symbol: ";
  y += yamlQuoted(String(cfg.symbol));
  y += "\n";
  y += "beacon_interval_ms: ";
  y += String(cfg.beaconIntervalMs);
  y += "\n";
  y += "screen_timeout_sec: ";
  y += String(cfg.screenTimeoutSec);
  y += "\n";
  y += "frequency_mhz: ";
  y += String(cfg.frequencyMhz, 3);
  y += "\n";
  y += "spreading_factor: ";
  y += String(cfg.spreadingFactor);
  y += "\n";
  y += "bandwidth_khz: ";
  y += String(cfg.bandwidthKhz, 1);
  y += "\n";
  y += "coding_rate: ";
  y += String(cfg.codingRate);
  y += "\n";
  y += "tx_power_dbm: ";
  y += String(cfg.txPowerDbm);
  y += "\n";
  y += "allow_manual_position: ";
  y += cfg.allowManualPosition ? "true" : "false";
  y += "\n";
  y += "manual_lat_e7: ";
  y += String(cfg.manualLatE7);
  y += "\n";
  y += "manual_lon_e7: ";
  y += String(cfg.manualLonE7);
  y += "\n";
  y += "manual_alt_meters: ";
  y += String(cfg.manualAltMeters);
  y += "\n";
  y += "wifi_ssid: ";
  y += yamlQuoted(String(cfg.wifiSsid));
  y += "\n";
  y += "wifi_pass: ";
  y += yamlQuoted(String(cfg.wifiPass));
  y += "\n";
  return y;
}

bool parseYamlConfig(const String& yaml, RuntimeConfig& next, String& errorOut) {
  int lineStart = 0;
  while (lineStart < yaml.length()) {
    int lineEnd = yaml.indexOf('\n', lineStart);
    if (lineEnd < 0) {
      lineEnd = yaml.length();
    }

    String line = yaml.substring(lineStart, lineEnd);
    line.replace("\r", "");
    line.trim();
    lineStart = lineEnd + 1;

    if (line.length() == 0 || line.startsWith("#")) {
      continue;
    }

    const int colon = line.indexOf(':');
    if (colon <= 0) {
      continue;
    }

    String key = line.substring(0, colon);
    key.trim();
    String val = line.substring(colon + 1);
    val = trimQuotes(val);

    if (key == "callsign") {
      copyToArray(next.callsign, sizeof(next.callsign), normalizeCallsignInput(val));
    } else if (key == "destination") {
      copyToArray(next.destination, sizeof(next.destination), normalizeCallsignInput(val));
    } else if (key == "path") {
      copyToArray(next.path, sizeof(next.path), normalizeRouteInput(val));
    } else if (key == "comment") {
      copyToArray(next.comment, sizeof(next.comment), val);
    } else if (key == "aprsph_message") {
      copyToArray(next.aprsphMessage, sizeof(next.aprsphMessage), val);
    } else if (key == "hotg_message") {
      copyToArray(next.hotgMessage, sizeof(next.hotgMessage), val);
    } else if (key == "symbol_table") {
      if (val.length() > 0) {
        next.symbolTable = val[0];
      }
    } else if (key == "symbol") {
      if (val.length() > 0) {
        next.symbol = val[0];
      }
    } else if (key == "beacon_interval_ms") {
      next.beaconIntervalMs = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
    } else if (key == "screen_timeout_sec") {
      next.screenTimeoutSec = static_cast<uint16_t>(strtoul(val.c_str(), nullptr, 10));
    } else if (key == "frequency_mhz") {
      next.frequencyMhz = val.toFloat();
    } else if (key == "spreading_factor") {
      next.spreadingFactor = val.toInt();
    } else if (key == "bandwidth_khz") {
      next.bandwidthKhz = val.toFloat();
    } else if (key == "coding_rate") {
      next.codingRate = val.toInt();
    } else if (key == "tx_power_dbm") {
      next.txPowerDbm = val.toInt();
    } else if (key == "allow_manual_position") {
      next.allowManualPosition = parseBoolValue(val, next.allowManualPosition);
    } else if (key == "manual_lat_e7") {
      next.manualLatE7 = static_cast<int32_t>(strtol(val.c_str(), nullptr, 10));
    } else if (key == "manual_lon_e7") {
      next.manualLonE7 = static_cast<int32_t>(strtol(val.c_str(), nullptr, 10));
    } else if (key == "manual_alt_meters") {
      next.manualAltMeters = val.toInt();
    } else if (key == "wifi_ssid") {
      copyToArray(next.wifiSsid, sizeof(next.wifiSsid), val);
    } else if (key == "wifi_pass") {
      copyToArray(next.wifiPass, sizeof(next.wifiPass), val);
    }
  }

  if (next.callsign[0] == '\0') {
    errorOut = "Import missing callsign";
    return false;
  }

  return true;
}

void sendConfigPage(const char* msg);

void sendConfigPageAndReboot(const char* msg) {
  sendConfigPage(msg);
  // Give the browser a brief moment to receive the response before restarting.
  delay(250);
  ESP.restart();
}

void appendPageShellStart(String& html) {
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Daisy APRS Config</title>";
  html += "<link rel='stylesheet' href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css' crossorigin=''>";
  html += "<style>";
  html += "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;max-width:740px;margin:1.2em auto;padding:0 .9em;background:#0f1522;color:#ecf2ff}";
  html += "h2{margin:.2em 0 .6em;color:#9bc7ff}h3{margin:1em 0 .35em;color:#d9e6ff}";
  html += "label{display:block;margin:.45em 0 .15em;color:#dbe6fb;font-size:.92em}";
  html += "input,select{width:100%;padding:.5em;border:1px solid #3b4d71;border-radius:6px;background:#18233a;color:#eff5ff;box-sizing:border-box}";
  html += "button{margin-top:1.1em;padding:.6em 1.2em;background:#4f8cff;color:white;border:none;border-radius:6px;font-weight:600;cursor:pointer}";
  html += "button.danger{background:#b53a3a}";
  html += "button.tab-btn{margin-top:0;padding:.5em .9em;background:#1b2943;border:1px solid #3f5f92;color:#d9e8ff;font-weight:700}";
  html += "button.tab-btn.active{background:#4f8cff;border-color:#6ea2ff;color:#fff}";
  html += ".row2{display:grid;grid-template-columns:1fr 1fr;gap:.6em}.row3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:.6em}";
  html += ".msg{margin:.45em 0;color:#9ef0ae}.small{font-size:.85em;color:#b7c8e8}";
  html += ".tabs{display:flex;gap:.45em;flex-wrap:wrap;margin:.8em 0 .7em}";
  html += ".tab-panel{display:block;opacity:0;transform:translateY(10px);max-height:0;overflow:hidden;pointer-events:none;transition:opacity .22s ease,transform .22s ease,max-height .28s ease}";
  html += ".tab-panel.active{opacity:1;transform:none;max-height:5000px;pointer-events:auto}";
  html += ".cfg-section{margin:.75em 0;border:1px solid #2e456c;border-radius:10px;background:#121d30;overflow:hidden}";
  html += ".cfg-section>summary{cursor:pointer;list-style:none;padding:.62em .78em;font-weight:700;color:#d9e6ff;background:#17253d;user-select:none}";
  html += ".cfg-section>summary::-webkit-details-marker{display:none}";
  html += ".cfg-section>summary::after{content:'+';float:right;color:#9bc7ff}";
  html += ".cfg-section[open]>summary::after{content:'-'}";
  html += ".cfg-section[open]>summary{border-bottom:1px solid #2e456c}";
  html += ".cfg-body{padding:.55em .78em .75em}";
  html += ".map-wrap{position:relative;margin-top:.6em;border:1px solid #33517f;border-radius:10px;overflow:hidden;background:#0b111d}";
  html += "#nodes-map{height:340px;width:100%}";
  html += "#map-me-btn{position:absolute;top:10px;right:10px;z-index:5000;margin:0;padding:.45em .75em;background:#0f5cb8;border:1px solid #78b2ff;border-radius:8px;font-weight:800;letter-spacing:.02em}";
  html += "#nodes-meta{margin:.55em 0 .2em}";
  html += ".scroll-list{max-height:220px;overflow:auto;border:1px solid #2e456c;border-radius:8px;background:#111b2d;padding:.4em .6em}";
  html += ".list-item{padding:.45em 0;border-bottom:1px solid #273d63}.list-item:last-child{border-bottom:none}";
  html += ".list-head{display:flex;justify-content:space-between;gap:.6em;align-items:baseline}";
  html += ".list-meta{font-size:.82em;color:#b8cae7}";
  html += "@media (prefers-reduced-motion:reduce){.tab-panel{transition:none}}";
  html += "@media (max-width:700px){.row2,.row3{grid-template-columns:1fr}}";
  html += "</style></head><body>";
}

void appendPageHeader(String& html, const char* msg) {
  html += "<h2>Daisy APRS Web Config</h2>";
  html += "<p class='small'>Mode: ";
  html += gApMode ? "AP fallback" : "Home Wi-Fi";
  html += " | IP: ";
  html += webConfigIP();
  if (gApMode) {
    html += " | AP SSID: ";
    html += webConfigSsid();
  }
  html += "</p>";

  if (msg && msg[0]) {
    html += "<p class='msg'>";
    html += escapeHtml(msg);
    html += "</p>";
  }
}

void appendConfigForm(String& html) {
  html += "<form method='POST' action='/save'>";

  html += "<details class='cfg-section' open><summary>Wi-Fi</summary><div class='cfg-body'>";
  html += "<label>Home SSID</label><input name='wifi_ssid' maxlength='63' value='";
  html += escapeHtml(String(gCfg->wifiSsid));
  html += "'>";
  html += "<label>Home Password</label><input type='password' name='wifi_pass' maxlength='63' value='";
  html += escapeHtml(String(gCfg->wifiPass));
  html += "'>";
  html += "<p class='small'>On reboot, device tries Home Wi-Fi first. If unavailable, it starts its own AP for config.</p>";
  html += "</div></details>";

  html += "<details class='cfg-section' open><summary>APRS Identity</summary><div class='cfg-body'>";
  html += "<div class='row3'>";
  html += "<div><label>Callsign</label><input name='callsign' maxlength='15' value='";
  html += escapeHtml(String(gCfg->callsign));
  html += "'></div>";
  html += "<div><label>Destination</label><input name='destination' maxlength='15' value='";
  html += escapeHtml(String(gCfg->destination));
  html += "'></div>";
  html += "<div><label>Path</label><input name='path' maxlength='31' value='";
  html += escapeHtml(String(gCfg->path));
  html += "'></div></div>";
  html += "<label>Comment</label><input name='comment' maxlength='79' value='";
  html += escapeHtml(String(gCfg->comment));
  html += "'>";
  html += "<label>APRSPH Message</label><input name='aprsph_message' maxlength='31' value='";
  html += escapeHtml(String(gCfg->aprsphMessage));
  html += "'>";
  html += "<label>HOTG Message</label><input name='hotg_message' maxlength='31' value='";
  html += escapeHtml(String(gCfg->hotgMessage));
  html += "'>";

  html += "<div class='row2'>";
  html += "<div><label>Symbol Table</label><input name='symbol_table' maxlength='1' value='";
  html += escapeHtml(String(gCfg->symbolTable));
  html += "'></div>";
  html += "<div><label>Symbol</label><input name='symbol' maxlength='1' value='";
  html += escapeHtml(String(gCfg->symbol));
  html += "'></div></div>";
  html += "</div></details>";

  html += "<details class='cfg-section' open><summary>Beacon and GPS</summary><div class='cfg-body'>";
  html += "<label>Beacon Interval (minutes)</label><input type='number' min='5' name='beacon_min' value='";
  html += String(gCfg->beaconIntervalMs / 60000UL);
  html += "'>";

  html += "<label>Screen Timeout (seconds)</label><input type='number' min='1' max='300' name='screen_timeout_sec' value='";
  html += String(gCfg->screenTimeoutSec);
  html += "'>";

  html += "<label><input type='checkbox' name='manual_pos' value='1'";
  if (gCfg->allowManualPosition) {
    html += " checked";
  }
  html += "> Allow manual position fallback when GPS has no fix</label>";

  html += "<div class='row3'>";
  html += "<div><label>Manual Lat (deg)</label><input type='number' step='0.0000001' name='lat' value='";
  html += String(gCfg->manualLatE7 / 1e7, 7);
  html += "'></div>";
  html += "<div><label>Manual Lon (deg)</label><input type='number' step='0.0000001' name='lon' value='";
  html += String(gCfg->manualLonE7 / 1e7, 7);
  html += "'></div>";
  html += "<div><label>Manual Alt (m)</label><input type='number' name='alt' value='";
  html += String(gCfg->manualAltMeters);
  html += "'></div></div>";
  html += "</div></details>";

  html += "<details class='cfg-section' open><summary>LoRa APRS Radio</summary><div class='cfg-body'>";
  html += "<div class='row3'>";
  html += "<div><label>Frequency (MHz)</label><input type='number' step='0.001' min='137' max='1020' name='freq' value='";
  html += String(gCfg->frequencyMhz, 3);
  html += "'></div>";
  html += "<div><label>Bandwidth (kHz)</label><input type='number' step='0.1' min='7.8' max='500' name='bw' value='";
  html += String(gCfg->bandwidthKhz, 1);
  html += "'></div>";
  html += "<div><label>TX Power (dBm)</label><input type='number' min='-9' max='22' name='pwr' value='";
  html += String(gCfg->txPowerDbm);
  html += "'></div></div>";

  html += "<div class='row2'>";
  html += "<div><label>Spreading Factor</label><input type='number' min='7' max='12' name='sf' value='";
  html += String(gCfg->spreadingFactor);
  html += "'></div>";
  html += "<div><label>Coding Rate (5..8 means 4/5..4/8)</label><input type='number' min='5' max='8' name='cr' value='";
  html += String(gCfg->codingRate);
  html += "'></div></div>";
  html += "</div></details>";

  html += "<button type='submit'>Save Config</button>";
  html += "</form>";
}

void appendUtilityForms(String& html) {
  html += "<h3>Import / Export</h3>";
  html += "<p class='small'>Export downloads YAML. Import uploads a YAML config file and applies it immediately.</p>";
  html += "<form method='GET' action='/export'>";
  html += "<button type='submit'>Export YAML</button>";
  html += "</form>";

  html += "<form method='POST' action='/import' enctype='multipart/form-data'>";
  html += "<label>Import YAML file</label>";
  html += "<input type='file' name='config_file' accept='.yml,.yaml,text/yaml'>";
  html += "<button type='submit'>Import YAML</button>";
  html += "</form>";

  html += "<h3>Factory Reset</h3>";
  html += "<p class='small'>Clears all saved settings and reboots to first-boot behavior (AP fallback until configured).</p>";
  html += "<form method='POST' action='/reset' onsubmit='return confirm(\"Erase all saved settings and reboot?\")'>";
  html += "<button class='danger' type='submit'>Factory Reset</button>";
  html += "</form>";
}

void appendNodesPanel(String& html) {
  html += "<h3>Nodes</h3>";
  html += "<p class='small'>Live APRS radios heard by this device. Heat intensity reflects stronger RSSI values.</p>";
  html += "<div class='map-wrap'><div id='nodes-map'></div>";
  html += "<button id='map-me-btn' type='button' onclick='centerOnMeMap()'>ME</button></div>";
  html += "<p id='nodes-meta' class='small'>Waiting for live node data...</p>";
  html += "<h3>Radios Heard</h3>";
  html += "<div id='nodes-list' class='scroll-list small'>No radios heard yet.</div>";
}

void appendLivePanel(String& html) {
  html += "<h3>Live Feed</h3>";
  html += "<p class='small'>Most recent APRS packets heard by this device.</p>";
  html += "<div id='live-feed' class='scroll-list small'>Waiting for packets...</div>";
}

void appendPageScripts(String& html) {
  html += "<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js' crossorigin=''></script>";
  html += "<script src='https://unpkg.com/leaflet.heat@0.2.0/dist/leaflet-heat.js' crossorigin=''></script>";
  html += R"WEBJS(<script>
(function() {
  const tabButtons = Array.from(document.querySelectorAll('.tab-btn'));
  const tabPanels = Array.from(document.querySelectorAll('.tab-panel'));
  let pollHandle = null;
  let nodeMap = null;
  let markerLayer = null;
  let heatLayer = null;
  let selfMarker = null;
  let hasAutoFrame = false;
  const nodeMarkers = new Map();
  let lastSelf = { callsign: '', has_position: false, lat: 0, lon: 0 };

  function esc(value) {
    return String(value === undefined ? '' : value)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/\"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  function rssiToWeight(rssi) {
    const mag = Math.min(Math.abs(Number(rssi) || 130), 170);
    return Math.max(0.15, 1.0 - (mag / 170.0));
  }

  function activateTab(name) {
    tabButtons.forEach((btn) => btn.classList.toggle('active', btn.dataset.tab === name));
    tabPanels.forEach((panel) => panel.classList.toggle('active', panel.id === ('tab-' + name)));
    if (window.history && window.history.replaceState) {
      window.history.replaceState(null, '', '#tab-' + name);
    }

    if (name === 'nodes' || name === 'live') {
      if (name === 'nodes') {
        ensureNodeMap();
        setTimeout(() => {
          if (nodeMap) {
            nodeMap.invalidateSize();
          }
        }, 80);
      }
      refreshNodes();
      startPolling();
      return;
    }

    stopPolling();
  }

  function ensureNodeMap() {
    if (nodeMap || !window.L) {
      return;
    }

    nodeMap = L.map('nodes-map', {
      zoomControl: true,
      preferCanvas: true,
    }).setView([20, 0], 2);

    L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
      maxZoom: 19,
      attribution: '&copy; OpenStreetMap contributors',
    }).addTo(nodeMap);

    markerLayer = L.layerGroup().addTo(nodeMap);
    if (window.L.heatLayer) {
      heatLayer = L.heatLayer([], {
        radius: 28,
        blur: 22,
        maxZoom: 17,
        minOpacity: 0.30,
        gradient: {
          0.20: '#1d4ed8',
          0.50: '#06b6d4',
          0.75: '#f59e0b',
          1.00: '#dc2626',
        },
      }).addTo(nodeMap);
    }
  }

  window.centerOnMeMap = function centerOnMeMap() {
    ensureNodeMap();
    if (!nodeMap || !lastSelf || !lastSelf.has_position) {
      return;
    }

    const targetZoom = nodeMap.getZoom() < 13 ? 13 : nodeMap.getZoom();
    nodeMap.setView([Number(lastSelf.lat), Number(lastSelf.lon)], targetZoom);
  };

  function renderNodes(payload) {
    const nodes = Array.isArray(payload && payload.nodes) ? payload.nodes : [];
    const feed = Array.isArray(payload && payload.feed) ? payload.feed : [];
    lastSelf = payload && payload.self
      ? payload.self
      : { callsign: '', has_position: false, lat: 0, lon: 0 };

    const metaEl = document.getElementById('nodes-meta');
    if (metaEl) {
      let metaText = String(nodes.length) + ' radios tracked';
      if (lastSelf.has_position) {
        const meName = lastSelf.callsign ? String(lastSelf.callsign) : 'ME';
        metaText +=
          ' | ' +
          meName +
          ': ' +
          Number(lastSelf.lat).toFixed(5) +
          ', ' +
          Number(lastSelf.lon).toFixed(5);
      }
      metaEl.textContent = metaText;
    }

    ensureNodeMap();
    if (nodeMap) {
      const seen = new Set();
      const heatPoints = [];
      const bounds = [];

      nodes.forEach((node) => {
        if (!node || !node.callsign || !node.has_position) {
          return;
        }

        const lat = Number(node.lat);
        const lon = Number(node.lon);
        if (!Number.isFinite(lat) || !Number.isFinite(lon)) {
          return;
        }

        const key = String(node.callsign);
        seen.add(key);
        let marker = nodeMarkers.get(key);
        if (!marker) {
          marker = L.circleMarker([lat, lon], {
            radius: 6,
            color: '#a7f3d0',
            weight: 2,
            fillColor: '#22c55e',
            fillOpacity: 0.35,
          }).addTo(markerLayer);
          nodeMarkers.set(key, marker);
        } else {
          marker.setLatLng([lat, lon]);
        }

        marker.bindPopup(
          '<strong>' +
            esc(node.callsign) +
            '</strong><br>RSSI ' +
            Number(node.rssi).toFixed(1) +
            ' dBm | SNR ' +
            Number(node.snr).toFixed(1) +
            ' dB<br>Heard ' +
            Number(node.age_s) +
            's ago'
        );

        heatPoints.push([lat, lon, rssiToWeight(node.rssi)]);
        bounds.push([lat, lon]);
      });

      if (lastSelf.has_position) {
        const meLat = Number(lastSelf.lat);
        const meLon = Number(lastSelf.lon);
        if (Number.isFinite(meLat) && Number.isFinite(meLon)) {
          if (!selfMarker) {
            selfMarker = L.circleMarker([meLat, meLon], {
              radius: 8,
              color: '#bfdbfe',
              weight: 3,
              fillColor: '#2563eb',
              fillOpacity: 0.45,
            }).addTo(markerLayer);
          } else {
            selfMarker.setLatLng([meLat, meLon]);
          }

          const meName = lastSelf.callsign ? String(lastSelf.callsign) : 'ME';
          selfMarker.bindPopup(
            '<strong>ME: ' + esc(meName) + '</strong><br>' +
            Number(meLat).toFixed(5) + ', ' + Number(meLon).toFixed(5)
          );
          bounds.push([meLat, meLon]);
        }
      } else if (selfMarker) {
        markerLayer.removeLayer(selfMarker);
        selfMarker = null;
      }

      nodeMarkers.forEach((marker, key) => {
        if (!seen.has(key)) {
          markerLayer.removeLayer(marker);
          nodeMarkers.delete(key);
        }
      });

      if (heatLayer) {
        heatLayer.setLatLngs(heatPoints);
      }

      if (!hasAutoFrame && bounds.length > 0) {
        nodeMap.fitBounds(bounds, { padding: [24, 24] });
        hasAutoFrame = true;
      }
    }

    const listEl = document.getElementById('nodes-list');
    if (listEl) {
      if (nodes.length === 0) {
        listEl.innerHTML = "<div class='list-item'>No radios heard yet.</div>";
      } else {
        let listHtml = '';
        nodes.forEach((node) => {
          const posText = node.has_position
            ? Number(node.lat).toFixed(5) + ', ' + Number(node.lon).toFixed(5)
            : 'No position';

          listHtml +=
            "<div class='list-item'><div class='list-head'><strong>" +
            esc(node.callsign) +
            "</strong><span class='list-meta'>" +
            Number(node.age_s) +
            "s ago</span></div><div class='list-meta'>RSSI " +
            Number(node.rssi).toFixed(1) +
            ' dBm | SNR ' +
            Number(node.snr).toFixed(1) +
            ' dB | Heard ' +
            Number(node.heard_count) +
            "x</div><div class='list-meta'>" +
            esc(posText) +
            '</div></div>';
        });
        listEl.innerHTML = listHtml;
      }
    }

    const feedEl = document.getElementById('live-feed');
    if (feedEl) {
      if (feed.length === 0) {
        feedEl.innerHTML = "<div class='list-item'>Waiting for packets...</div>";
      } else {
        let feedHtml = '';
        feed.forEach((event) => {
          feedHtml +=
            "<div class='list-item'><div class='list-head'><strong>" +
            esc(event.callsign) +
            "</strong><span class='list-meta'>" +
            Number(event.age_s) +
            "s ago</span></div><div class='list-meta'>RSSI " +
            Number(event.rssi).toFixed(1) +
            ' dBm | SNR ' +
            Number(event.snr).toFixed(1) +
            " dB</div><div>" +
            esc(event.summary) +
            '</div></div>';
        });
        feedEl.innerHTML = feedHtml;
      }
    }
  }

  async function refreshNodes() {
    try {
      const response = await fetch('/nodes', { cache: 'no-store' });
      if (!response.ok) {
        throw new Error('HTTP ' + response.status);
      }

      const payload = await response.json();
      renderNodes(payload);
    } catch (error) {
      const metaEl = document.getElementById('nodes-meta');
      if (metaEl) {
        metaEl.textContent = 'Nodes update failed';
      }

      const feedEl = document.getElementById('live-feed');
      if (feedEl) {
        feedEl.innerHTML = "<div class='list-item'>Live update failed</div>";
      }
    }
  }

  function startPolling() {
    if (pollHandle) {
      return;
    }
    pollHandle = window.setInterval(refreshNodes, 2500);
  }

  function stopPolling() {
    if (!pollHandle) {
      return;
    }
    window.clearInterval(pollHandle);
    pollHandle = null;
  }

  tabButtons.forEach((btn) => {
    btn.addEventListener('click', () => activateTab(btn.dataset.tab));
  });

  const hash = String(window.location.hash || '').replace('#tab-', '');
  if (hash === 'utility' || hash === 'nodes' || hash === 'live' || hash === 'config') {
    activateTab(hash);
  } else {
    activateTab('config');
  }

  window.addEventListener('beforeunload', stopPolling);
})();
</script>)WEBJS";
}

void sendConfigPage(const char* msg = nullptr) {
  if (!gCfg) {
    server.send(500, "text/plain", "No config");
    return;
  }

  String html;
  html.reserve(22000);
  appendPageShellStart(html);
  appendPageHeader(html, msg);

  html += "<div class='tabs'>";
  html += "<button type='button' class='tab-btn active' data-tab='config'>Config</button>";
  html += "<button type='button' class='tab-btn' data-tab='utility'>Utility</button>";
  html += "<button type='button' class='tab-btn' data-tab='nodes'>Nodes</button>";
  html += "<button type='button' class='tab-btn' data-tab='live'>Live</button>";
  html += "</div>";

  html += "<section id='tab-config' class='tab-panel active'>";
  appendConfigForm(html);
  html += "</section>";

  html += "<section id='tab-utility' class='tab-panel'>";
  appendUtilityForms(html);
  html += "</section>";

  html += "<section id='tab-nodes' class='tab-panel'>";
  appendNodesPanel(html);
  html += "</section>";

  html += "<section id='tab-live' class='tab-panel'>";
  appendLivePanel(html);
  html += "</section>";

  appendPageScripts(html);
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  if (!gCfg) {
    server.send(500, "text/plain", "No config");
    return;
  }

  RuntimeConfig next = *gCfg;

  copyToArray(next.wifiSsid, sizeof(next.wifiSsid), argOr("wifi_ssid", String(next.wifiSsid)));
  copyToArray(next.wifiPass, sizeof(next.wifiPass), argOr("wifi_pass", String(next.wifiPass)));

  copyToArray(next.callsign, sizeof(next.callsign),
              normalizeCallsignInput(argOr("callsign", String(next.callsign))));
  copyToArray(next.destination, sizeof(next.destination),
              normalizeCallsignInput(argOr("destination", String(next.destination))));
  copyToArray(next.path, sizeof(next.path),
              normalizeRouteInput(argOr("path", String(next.path))));

  copyToArray(next.comment, sizeof(next.comment), argOr("comment", String(next.comment)));
  copyToArray(next.aprsphMessage, sizeof(next.aprsphMessage),
              argOr("aprsph_message", String(next.aprsphMessage)));
  copyToArray(next.hotgMessage, sizeof(next.hotgMessage),
              argOr("hotg_message", String(next.hotgMessage)));

  const String symbolTable = argOr("symbol_table", String(next.symbolTable));
  if (symbolTable.length() > 0) {
    next.symbolTable = symbolTable[0];
  }

  const String symbol = argOr("symbol", String(next.symbol));
  if (symbol.length() > 0) {
    next.symbol = symbol[0];
  }

  next.beaconIntervalMs = argToULong("beacon_min", next.beaconIntervalMs / 60000UL) * 60000UL;

  int screenTimeoutSec = argToInt("screen_timeout_sec", next.screenTimeoutSec);
  if (screenTimeoutSec < 1) {
    screenTimeoutSec = AppConfig::kScreenTimeoutSec;
  }
  if (screenTimeoutSec > AppConfig::kScreenTimeoutMaxSec) {
    screenTimeoutSec = AppConfig::kScreenTimeoutMaxSec;
  }
  next.screenTimeoutSec = static_cast<uint16_t>(screenTimeoutSec);

  next.allowManualPosition = server.hasArg("manual_pos");
  next.manualLatE7 = static_cast<int32_t>(roundf(argToFloat("lat", next.manualLatE7 / 1e7f) * 1e7f));
  next.manualLonE7 = static_cast<int32_t>(roundf(argToFloat("lon", next.manualLonE7 / 1e7f) * 1e7f));
  next.manualAltMeters = argToInt("alt", next.manualAltMeters);

  next.frequencyMhz = argToFloat("freq", next.frequencyMhz);
  next.bandwidthKhz = argToFloat("bw", next.bandwidthKhz);
  next.txPowerDbm = argToInt("pwr", next.txPowerDbm);
  next.spreadingFactor = argToInt("sf", next.spreadingFactor);
  next.codingRate = argToInt("cr", next.codingRate);

  if (runtimeConfigSave(next)) {
    *gCfg = next;
    if (gOnSave) {
      gOnSave();
    }
    sendConfigPageAndReboot("Saved. Rebooting now...");
    return;
  }

  sendConfigPage("Save failed.");
}

void handleFactoryReset() {
  if (!gCfg) {
    server.send(500, "text/plain", "No config");
    return;
  }

  RuntimeConfig resetCfg;
  if (!runtimeConfigFactoryReset(resetCfg)) {
    sendConfigPage("Factory reset failed.");
    return;
  }

  *gCfg = resetCfg;
  if (gOnSave) {
    gOnSave();
  }

  sendConfigPageAndReboot("Factory reset complete. Rebooting now...");
}

void updateIpBuffer() {
  IPAddress ip = gApMode ? WiFi.softAPIP() : WiFi.localIP();
  snprintf(gIpBuf, sizeof(gIpBuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

bool connectStation() {
  if (!gCfg || gCfg->wifiSsid[0] == '\0') {
    return false;
  }

  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(120);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(gCfg->wifiSsid, gCfg->wifiPass);

  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < kConnectTimeoutMs) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  // Ensure the AP fallback starts from a clean state.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(120);
  return false;
}

bool startAccessPoint() {
  snprintf(gApSsid, sizeof(gApSsid), "%s", kApSsid);

  const IPAddress apIp(192, 168, 4, 1);
  const IPAddress apGw(192, 168, 4, 1);
  const IPAddress apMask(255, 255, 255, 0);

  for (int attempt = 0; attempt < 3; ++attempt) {
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_OFF);
    delay(120);

    WiFi.mode(WIFI_AP);
    delay(150);
    WiFi.setSleep(false);
    WiFi.softAPConfig(apIp, apGw, apMask);

    if (WiFi.softAP(gApSsid, kApPassword)) {
      Serial.printf("[WEB] AP started (WPA2): SSID=%s IP=%s\n", gApSsid,
                    WiFi.softAPIP().toString().c_str());
      return true;
    }

    delay(150);
  }

  return false;
}

void configureRoutes() {
  server.on("/", HTTP_GET, []() { sendConfigPage(); });
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reset", HTTP_POST, handleFactoryReset);
  server.on("/export", HTTP_GET, []() {
    if (!gCfg) {
      server.send(500, "text/plain", "No config");
      return;
    }

    const String fileName = exportFilenameFromCallsign(gCfg->callsign);
    const String yaml = buildConfigYaml(*gCfg);
    server.sendHeader("Content-Disposition", String("attachment; filename=\"") + fileName + "\"");
    server.send(200, "application/x-yaml", yaml);
  });
  server.on(
      "/import", HTTP_POST,
      []() {
        if (!gCfg) {
          server.send(500, "text/plain", "No config");
          return;
        }

        if (gImportTooLarge) {
          sendConfigPage("Import failed: file too large.");
          gImportYaml = "";
          gImportTooLarge = false;
          return;
        }

        if (gImportYaml.length() == 0) {
          sendConfigPage("Import failed: empty YAML file.");
          gImportTooLarge = false;
          return;
        }

        RuntimeConfig next = *gCfg;
        String parseErr;
        if (!parseYamlConfig(gImportYaml, next, parseErr)) {
          String msg = "Import failed: ";
          msg += parseErr;
          sendConfigPage(msg.c_str());
          gImportYaml = "";
          gImportTooLarge = false;
          return;
        }

        if (!runtimeConfigSave(next)) {
          sendConfigPage("Import failed: could not save config.");
          gImportYaml = "";
          gImportTooLarge = false;
          return;
        }

        *gCfg = next;
        if (gOnSave) {
          gOnSave();
        }
        gImportYaml = "";
        gImportTooLarge = false;
        sendConfigPageAndReboot("Import successful. Rebooting now...");
      },
      []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          gImportYaml = "";
          gImportTooLarge = false;
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if ((gImportYaml.length() + upload.currentSize) <= kMaxImportYamlBytes) {
            for (size_t i = 0; i < upload.currentSize; ++i) {
              gImportYaml += static_cast<char>(upload.buf[i]);
            }
          } else {
            gImportTooLarge = true;
          }
        }
      });
  server.on("/nodes", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", buildNodesJson());
  });
  server.on("/status", HTTP_GET, []() {
    String json = "{";
    json += "\"mode\":\"";
    json += gApMode ? "ap" : "sta";
    json += "\",\"ip\":\"";
    json += webConfigIP();
    json += "\",\"ssid\":\"";
    json += gApMode ? String(gApSsid) : String(gCfg ? gCfg->wifiSsid : "");
    json += "\"}";
    server.send(200, "application/json", json);
  });
}
}  // namespace

bool webConfigBegin(RuntimeConfig* cfg, WebConfigSaveCb onSave) {
  gCfg = cfg;
  gOnSave = onSave;

  if (!gCfg) {
    return false;
  }

  gApMode = false;
  gApSsid[0] = '\0';
  gStaLostSinceMs = 0;

  if (connectStation()) {
    updateIpBuffer();
  } else {
    gApMode = true;
    if (!startAccessPoint()) {
      return false;
    }
    updateIpBuffer();
  }

  configureRoutes();
  server.begin();
  gRunning = true;
  return true;
}

void webConfigEnd() {
  if (!gRunning) {
    return;
  }

  server.stop();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);

  gRunning = false;
  gApMode = false;
  gStaLostSinceMs = 0;
  gIpBuf[0] = '\0';
  gApSsid[0] = '\0';
}

void webConfigLoop() {
  if (!gRunning) {
    return;
  }

  if (!gApMode) {
    if (WiFi.status() == WL_CONNECTED) {
      gStaLostSinceMs = 0;
    } else {
      if (gStaLostSinceMs == 0) {
        gStaLostSinceMs = millis();
      } else if ((millis() - gStaLostSinceMs) >= kStaDropToApMs) {
        Serial.println("[WEB] STA dropped, switching to AP fallback");
        if (startAccessPoint()) {
          gApMode = true;
          updateIpBuffer();
          Serial.printf("[WEB] AP fallback active: SSID=%s URL=http://%s\n", gApSsid, gIpBuf);
        }
        gStaLostSinceMs = 0;
      }
    }
  }

  server.handleClient();
}

bool webConfigRunning() { return gRunning; }

bool webConfigIsApMode() { return gRunning && gApMode; }

const char* webConfigIP() { return gIpBuf; }

const char* webConfigSsid() { return gApSsid; }

void webConfigNoteHeardPacket(const String& aprsPacket, float rssi, float snr) {
  const String callsign = extractAprsSourceCallsign(aprsPacket);
  if (callsign.length() == 0) {
    return;
  }

  float lat = 0.0f;
  float lon = 0.0f;
  const bool hasPosition = parseAprsPositionFromPacket(aprsPacket, &lat, &lon);
  upsertHeardNode(callsign, aprsPacket, rssi, snr, hasPosition, lat, lon);
}

void webConfigSetSelfPosition(bool hasPosition, double latitude, double longitude) {
  gSelfHasPosition = hasPosition;
  if (!hasPosition) {
    gSelfLatitude = 0.0f;
    gSelfLongitude = 0.0f;
    return;
  }

  gSelfLatitude = static_cast<float>(latitude);
  gSelfLongitude = static_cast<float>(longitude);
}
