#include "web_config.h"

#include <WebServer.h>
#include <WiFi.h>

#include <ctype.h>
#include <math.h>
#include <string.h>

#include "runtime_config.h"

namespace {
constexpr uint32_t kConnectTimeoutMs = 10000;
constexpr const char* kApPassword = "daisyaprs";
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
  y += "symbol_table: ";
  y += yamlQuoted(String(cfg.symbolTable));
  y += "\n";
  y += "symbol: ";
  y += yamlQuoted(String(cfg.symbol));
  y += "\n";
  y += "beacon_interval_ms: ";
  y += String(cfg.beaconIntervalMs);
  y += "\n";
  y += "no_fix_log_interval_ms: ";
  y += String(cfg.noFixLogIntervalMs);
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
    } else if (key == "no_fix_log_interval_ms") {
      next.noFixLogIntervalMs = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
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

void sendConfigPage(const char* msg = nullptr) {
  if (!gCfg) {
    server.send(500, "text/plain", "No config");
    return;
  }

  String html;
  html.reserve(9000);
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Daisy APRS Config</title>";
  html += "<style>";
  html += "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;max-width:740px;margin:1.2em auto;padding:0 .9em;background:#0f1522;color:#ecf2ff}";
  html += "h2{margin:.2em 0 .6em;color:#9bc7ff}h3{margin:1em 0 .35em;color:#d9e6ff}";
  html += "label{display:block;margin:.45em 0 .15em;color:#dbe6fb;font-size:.92em}";
  html += "input,select{width:100%;padding:.5em;border:1px solid #3b4d71;border-radius:6px;background:#18233a;color:#eff5ff;box-sizing:border-box}";
  html += "button{margin-top:1.1em;padding:.6em 1.2em;background:#4f8cff;color:white;border:none;border-radius:6px;font-weight:600;cursor:pointer}";
  html += ".row2{display:grid;grid-template-columns:1fr 1fr;gap:.6em}.row3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:.6em}";
  html += ".msg{margin:.45em 0;color:#9ef0ae}.small{font-size:.85em;color:#b7c8e8}";
  html += "@media (max-width:700px){.row2,.row3{grid-template-columns:1fr}}";
  html += "</style></head><body>";

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

  html += "<form method='POST' action='/save'>";

  html += "<h3>Wi-Fi</h3>";
  html += "<label>Home SSID</label><input name='wifi_ssid' maxlength='63' value='";
  html += escapeHtml(String(gCfg->wifiSsid));
  html += "'>";
  html += "<label>Home Password</label><input type='password' name='wifi_pass' maxlength='63' value='";
  html += escapeHtml(String(gCfg->wifiPass));
  html += "'>";
  html += "<p class='small'>On reboot, device tries Home Wi-Fi first. If unavailable, it starts its own AP for config.</p>";

  html += "<h3>APRS Identity</h3>";
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

  html += "<div class='row2'>";
  html += "<div><label>Symbol Table</label><input name='symbol_table' maxlength='1' value='";
  html += escapeHtml(String(gCfg->symbolTable));
  html += "'></div>";
  html += "<div><label>Symbol</label><input name='symbol' maxlength='1' value='";
  html += escapeHtml(String(gCfg->symbol));
  html += "'></div></div>";

  html += "<h3>Beacon and GPS</h3>";
  html += "<div class='row2'>";
  html += "<div><label>Beacon Interval (s)</label><input type='number' min='5' name='beacon_s' value='";
  html += String(gCfg->beaconIntervalMs / 1000UL);
  html += "'></div>";
  html += "<div><label>No-Fix Log Interval (s)</label><input type='number' min='5' name='nofix_s' value='";
  html += String(gCfg->noFixLogIntervalMs / 1000UL);
  html += "'></div></div>";

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

  html += "<h3>LoRa APRS Radio</h3>";
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

  html += "<button type='submit'>Save Config</button>";
  html += "</form>";

  html += "<h3>Import / Export</h3>";
  html += "<p class='small'>Export downloads YAML. Import uploads a YAML config file and applies it immediately.</p>";
  html += "<form method='GET' action='/export'>";
  html += "<button type='submit'>Export YAML</button>";
  html += "</form>";

  html += "<form method='POST' action='/import' enctype='multipart/form-data'>";
  html += "<label>Import YAML file</label>";
  html += "<input type='file' name='config_file' accept='.yml,.yaml,text/yaml'>";
  html += "<button type='submit'>Import YAML</button>";
  html += "</form></body></html>";

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

  const String symbolTable = argOr("symbol_table", String(next.symbolTable));
  if (symbolTable.length() > 0) {
    next.symbolTable = symbolTable[0];
  }

  const String symbol = argOr("symbol", String(next.symbol));
  if (symbol.length() > 0) {
    next.symbol = symbol[0];
  }

  next.beaconIntervalMs = argToULong("beacon_s", next.beaconIntervalMs / 1000UL) * 1000UL;
  next.noFixLogIntervalMs = argToULong("nofix_s", next.noFixLogIntervalMs / 1000UL) * 1000UL;

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
  const uint64_t mac = ESP.getEfuseMac();
  const uint32_t suffix = static_cast<uint32_t>(mac & 0xFFFFFF);
  snprintf(gApSsid, sizeof(gApSsid), "DAISY-APRS-%06lX", static_cast<unsigned long>(suffix));

  const IPAddress apIp(192, 168, 4, 1);
  const IPAddress apGw(192, 168, 4, 1);
  const IPAddress apMask(255, 255, 255, 0);

  for (int attempt = 0; attempt < 3; ++attempt) {
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_OFF);
    delay(120);

    const bool modeOk = WiFi.mode(WIFI_AP);
    delay(150);
    WiFi.setSleep(false);
    const bool cfgOk = WiFi.softAPConfig(apIp, apGw, apMask);

    Serial.printf("[WEB] AP attempt %d modeOk=%d cfgOk=%d mode=%d heap=%u\n", attempt + 1,
                  modeOk ? 1 : 0, cfgOk ? 1 : 0, static_cast<int>(WiFi.getMode()),
                  static_cast<unsigned int>(ESP.getFreeHeap()));

    // Open AP fallback is intentionally used for maximum bring-up reliability.
    if (WiFi.softAP(gApSsid)) {
      Serial.printf("[WEB] AP started (OPEN): SSID=%s IP=%s\n", gApSsid,
                    WiFi.softAPIP().toString().c_str());
      return true;
    }

    // If custom SSID failed, retry with a very short SSID.
    if (WiFi.softAP("daisy-aprs")) {
      strncpy(gApSsid, "daisy-aprs", sizeof(gApSsid) - 1);
      gApSsid[sizeof(gApSsid) - 1] = '\0';
      Serial.printf("[WEB] AP started (OPEN short SSID): SSID=%s IP=%s\n", gApSsid,
                    WiFi.softAPIP().toString().c_str());
      return true;
    }

    Serial.printf("[WEB] AP attempt %d failed\n", attempt + 1);
    delay(150);
  }

  return false;
}

void configureRoutes() {
  server.on("/", HTTP_GET, []() { sendConfigPage(); });
  server.on("/save", HTTP_POST, handleSave);
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
